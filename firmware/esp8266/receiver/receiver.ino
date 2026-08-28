/*
 * PROJEKT: ESP8266 UDP Alarm-System (Empfaenger)
 * -----------------------------------------------
 * Architektur: Hardware Abstraction Layer (HAL) + Finite State Machine (FSM)
 *
 * Schichten-Modell:
 *   [FSM]      - Zustandslogik: INIT -> WLAN_VERBINDEN -> BEREIT <-> ALARM -> WERKSRESET
 *   [Service]  - Sicherheit (HMAC, Replay-Window), Protokoll, Heartbeat, UDP-Verarbeitung
 *   [HAL]      - Hardware-Abstraktion: GPIO, WiFi, UDP, Flash, Timer, Telnet, mDNS
 *
 * Security: HMAC-SHA256, strikt monotoner Replay-Schutz (LittleFS-persistent),
 *           Constant-Time Signatur-Vergleich, isdigit-Validierung,
 *           Binary String Obfuscation (Flash), DoS Rate-Limiting
 *
 * Resilienz: Flash Wear-Leveling (LittleFS), Emergency QoS (Log-Filterung im Alarm),
 *            Remote-Override via Heartbeat-Kanal, Zero-Allocation UDP-Parsing
 *
 * Priority-Mode: Im ALARM-Zustand werden Heartbeat (bis auf Remote Control) und WLAN-Scan pausiert.
 *                Nur Hardware-Steuerung und UDP-Empfang laufen (Stop-the-World).
 *
 * Hardware:   NodeMCU V2 (ESP8266)
 * Autor:      Philip Keminer
 * Version:    V15.0 (signierte ACKs + persistente API-Replay-Abwehr)
 * Datum:      2026-03-04
 */

// ============================================================================
// BIBLIOTHEKEN
// ============================================================================

#include <FS.h>                      // Dateisystem-Grundfunktionen
#include <LittleFS.h>                // Flash-Dateisystem fuer ESP8266
#include <ArduinoJson.h>             // JSON-Parser fuer Konfiguration
#include "replay_schutz.h"           // Strikte Reihenfolge fuer Alarmbefehle
#include <ESP8266WiFi.h>             // WLAN-Funktionen
#include <WiFiManager.h>             // Captive Portal fuer WLAN-Konfiguration
#include <WiFiUdp.h>                 // UDP-Kommunikation
#include <ESP8266mDNS.h>             // mDNS-Responder (*.local)
#include <ESP8266HTTPClient.h>       // HTTP-Client fuer API-Calls
#include <WiFiClient.h>              // TCP-Client
#include <WiFiClientSecure.h>        // Fuer BearSSL HMAC-Bibliothek
#ifndef ALARM_ENABLE_TELNET
#define ALARM_ENABLE_TELNET 0        // Nur explizite Debug-Builds aktivieren
#endif
#if ALARM_ENABLE_TELNET
#include <TelnetStream.h>            // Debug-Konsole ueber Telnet
#endif
#include <Ticker.h>                  // Timer-Interrupts
#include <bearssl/bearssl.h>         // Kryptografie-Bibliothek
#include <libb64/cdecode.h>          // Base64-Decoding fuer verschluesselte Remote-Config
extern "C" {
#include <user_interface.h>          // Hardware-Zufallsquelle os_random()
}

// ============================================================================
// KONSTANTEN
// ============================================================================

const char* DEVICE_NAME = "alarm-receiver";  // Geraete-Identifikation

// --- Timing ---
const unsigned long ALARM_TOGGLE_INTERVALL = 200;          // LED/Summer-Wechsel alle 200ms (5 Hz)
const unsigned long WLAN_WIEDERHOLUNGS_INTERVALL = 20000;  // WLAN-Reconnect alle 20s
const unsigned long WLAN_SCAN_INTERVALL = 30000;           // Netzwerk-Scan alle 30s
const uint8_t STABILITAETS_SCHWELLWERT = 3;                // Hauptnetz muss 3x stabil sein
const int8_t RSSI_SCHWELLWERT = -75;                       // Mindest-Signalstaerke in dBm
const unsigned long TASTER_LANG_DRUCK = 1000;              // Kurzdruck-Schwelle: <1s = Toggle
const unsigned long TASTER_RESET_DRUCK = 10000;            // Langdruck: >10s = Factory Reset
const unsigned long BLINK_INTERVALL = 500;                 // WLAN-LED Blink-Intervall
const unsigned long HEARTBEAT_INTERVALL = 2000;            // Telemetrie alle 2s
const int WATCHDOG_TIMEOUT_SEK = 30;                       // Loop haengt nach 30s
const unsigned long TELNET_TIMEOUT = 300000;               // Session-Timeout: 5 Minuten

// --- Sicherheit ---
const uint8_t UDP_MAX_PAKETE_PRO_MINUTE = 60;             // DoS Rate-Limit
const uint8_t MAX_TELNET_VERSUCHE = 3;                    // Brute-Force-Schutz
const uint8_t REPLAY_FENSTER_GROESSE = 25;                // Persistenzformat-Kompatibilitaet

// --- Pins ---
#define PIN_LED_ROT  D1   // Alarm-LED 1 (wechselt mit Gelb)
#define PIN_LED_GELB D2   // Alarm-LED 2 (wechselt mit Rot)
#define PIN_LED_WLAN D3   // WLAN-Status-LED
#define PIN_SUMMER_1 D5   // Akustischer Alarm 1
#define PIN_SUMMER_2 D6   // Akustischer Alarm 2
#define PIN_TASTER   D7   // Toggle (<1s) / Reset (>10s)

// --- Obfuscated Payloads (Muss mit Sender uebereinstimmen!) ---
const char* CMD_ALARM_AN  = "NICE_TRY_WIRESHARK_USER";     // Verschleierter ALARM_ON Befehl
const char* CMD_ALARM_AUS = "ENCRYPTION_IS_YOUR_FRIEND";   // Verschleierter ALARM_OFF Befehl

// ============================================================================
// QUELLTEXT-VERSCHLEIERUNG (Kein Klartext in der Firmware)
// ============================================================================
// Diese Defaults hier sind nur Fallbacks fuer den allerersten Start.

// Decoder-Tabelle fuer Substitutions-Chiffre (liegt im Flash)
const char DECODER_TABLE[128] PROGMEM = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, '1', 34, '3', '4', '5', '7', 39, '9', '0', '8', 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    '2', 'V', 'F', 'Z', 'B', 'G', 'E', 'D', 'L', 'R', 'U', 'X', 'I', 'J', 'W', 'Q',
    'S', 'C', 'N', 'P', 'A', 'M', 'H', 'Y', 'T', 'O', 'K', 91, 92, 93, '6', 95,
    96, 'm', 'd', 'h', 'f', 'z', 'v', 'e', 'c', 'k', 'q', 'j', 'w', 'r', 'p', 'n',
    'i', 'o', 'u', 'g', 'x', 'b', 'y', 'l', 't', 's', 'a', 123, 124, 125, 126, 127
};

// Entschluesselt einen Text mit der Substitutions-Chiffre
void deobfuscate(char* text) {
    for (int i = 0; text[i] != '\0'; i++) {              // Durchlaufe String
        unsigned char idx = (unsigned char)text[i];      // Hole Zeichen
        if (idx < 128) {                                 // Nur ASCII
            char decoded = (char)pgm_read_byte(&DECODER_TABLE[idx]);  // Aus Flash lesen
            if (decoded != (char)idx) text[i] = decoded; // Ersetze wenn verschieden
        }
    }
}

// ============================================================================
// DATENSTRUKTUREN
// ============================================================================

// Zentrale System-Konfiguration
struct SystemKonfiguration {
    char udpToken[41] = "";             // HMAC-Secret (40 Zeichen + \0)
    char mdnsName[33] = "alarm-receiver"; // Eigener mDNS-Hostname
    char telnetPasswort[21] = "";  // Telnet-Login (verschleiert)
    char backupSsid[33] = "";           // Fallback-WLAN
    char backupPasswort[65] = "";       // Fallback-WLAN-Passwort
    char hauptWlanName[33] = "";        // Primaeres WLAN
    char hauptWlanPasswort[65] = "";    // Primaeres WLAN-Passwort
    char apPasswort[65] = "";  // Access-Point-Passwort (verschleiert)
    char apiServer[33] = "";            // API-Server IP-Adresse
    char apiToken[33] = "";             // Geraetespezifischer Bearer-Token
    char apiHmacToken[41] = "";         // Geraetespezifischer Delivery-HMAC
};

bool leseKonfigurationDatei(const char* pfad, SystemKonfiguration& ziel);

// FSM-Zustaende
enum SystemZustand {
    ZUSTAND_INIT,            // Einmalige Initialisierung
    ZUSTAND_WLAN_VERBINDEN,  // Captive Portal oder Direktverbindung
    ZUSTAND_BEREIT,          // Normalbetrieb: UDP empfangen, Heartbeat, Telnet
    ZUSTAND_ALARM,           // Priority Mode: Nur Hardware + UDP
    ZUSTAND_WERKSRESET       // Factory Reset
};

// ============================================================================
// HAL - HARDWARE ABSTRACTION LAYER
// ============================================================================

namespace HAL {

    // --- GPIO ---

    // Initialisiert alle GPIO-Pins
    void gpioInit() {
        pinMode(PIN_LED_WLAN, OUTPUT);      // WLAN-LED als Ausgang
        pinMode(PIN_LED_ROT, OUTPUT);       // Rote LED als Ausgang
        pinMode(PIN_LED_GELB, OUTPUT);      // Gelbe LED als Ausgang
        pinMode(PIN_SUMMER_1, OUTPUT);      // Summer 1 als Ausgang
        pinMode(PIN_SUMMER_2, OUTPUT);      // Summer 2 als Ausgang
        pinMode(PIN_TASTER, INPUT_PULLUP);  // Taster mit Pull-Up

        digitalWrite(PIN_LED_WLAN, LOW);    // WLAN-LED aus
        digitalWrite(PIN_LED_ROT, LOW);     // Rote LED aus
        digitalWrite(PIN_LED_GELB, LOW);    // Gelbe LED aus
        digitalWrite(PIN_SUMMER_1, LOW);    // Summer 1 aus
        digitalWrite(PIN_SUMMER_2, LOW);    // Summer 2 aus
    }

    // WLAN-LED setzen
    void wlanLed(bool an) {
        digitalWrite(PIN_LED_WLAN, an ? HIGH : LOW);  // LED setzen
    }

    // WLAN-LED umschalten (fuer Blink-Effekt)
    void wlanLedToggle() {
        digitalWrite(PIN_LED_WLAN, !digitalRead(PIN_LED_WLAN));  // Toggle
    }

    // Alarm-Hardware: Abwechselnd Rot/Gelb + Summer im 200ms-Takt
    void alarmHardwareSetzen(bool phase) {
        digitalWrite(PIN_LED_ROT,  phase ? HIGH : LOW);   // Rot an/aus
        digitalWrite(PIN_SUMMER_2, phase ? HIGH : LOW);   // Summer 2 an/aus
        digitalWrite(PIN_LED_GELB, phase ? LOW : HIGH);   // Gelb invertiert
        digitalWrite(PIN_SUMMER_1, phase ? HIGH : LOW);   // Summer 1 an/aus
    }

    // Schaltet alle Alarm-Hardware aus
    void alarmHardwareAus() {
        digitalWrite(PIN_SUMMER_1, LOW);    // Summer 1 aus
        digitalWrite(PIN_SUMMER_2, LOW);    // Summer 2 aus
        digitalWrite(PIN_LED_ROT, LOW);     // Rot aus
        digitalWrite(PIN_LED_GELB, LOW);    // Gelb aus
    }

    // Prueft ob Taster gedrueckt ist
    bool tasterGedrueckt() {
        return digitalRead(PIN_TASTER) == LOW;  // LOW = gedrueckt (Pull-Up)
    }

    // --- Watchdog ---

    Ticker watchdogTicker;           // Timer-Objekt
    volatile int watchdogZaehler = 0;  // Sekunden-Zaehler (ISR-sicher)
    volatile bool mussNeustarten = false;  // Neustart-Flag (ISR-sicher)

    // Watchdog-Interrupt (jede Sekunde)
    void ICACHE_RAM_ATTR watchdogISR() {
        watchdogZaehler++;                                    // Zaehler erhoehen
        if (watchdogZaehler >= WATCHDOG_TIMEOUT_SEK) mussNeustarten = true;  // Timeout
    }

    // Startet den Software-Watchdog
    void watchdogStarten() {
        watchdogTicker.attach(1.0, watchdogISR);  // ISR alle 1 Sekunde
    }

    // Stoppt den Software-Watchdog
    void watchdogStoppen() {
        watchdogTicker.detach();  // Timer deaktivieren
    }

    // Fuettert den Watchdog (setzt Zaehler zurueck)
    void watchdogFuettern() {
        watchdogZaehler = 0;  // Zaehler zuruecksetzen
    }

    // Prueft ob Watchdog ausgeloest wurde
    bool watchdogAusgeloest() {
        return mussNeustarten;  // Flag pruefen
    }

    // --- System ---

    // HAL-Initialisierung
    void init() {
        Serial.begin(9600);  // Serielle Konsole starten
        gpioInit();          // GPIO-Pins initialisieren
    }

    // System-Neustart
    void neustart() {
        delay(100);      // Kurz warten
        ESP.restart();   // Hardware-Neustart
    }

    unsigned long zeitMs() { return millis(); }          // Systemzeit in Millisekunden
    uint32_t freierHeap() { return ESP.getFreeHeap(); }  // Freier RAM
    String resetGrund() { return ESP.getResetReason(); } // Letzter Reset-Grund
    void cpuFreigeben() { yield(); }                     // CPU fuer WLAN freigeben

    // --- Flash (LittleFS) ---

    bool flashInit() { return LittleFS.begin(); }  // Dateisystem mounten
    void flashFormatieren() { LittleFS.format(); } // Dateisystem loeschen

    // --- WiFi ---

    bool wlanVerbunden() { return WiFi.status() == WL_CONNECTED; }  // Verbindungsstatus
    String wlanSsid() { return WiFi.SSID(); }       // Aktuelles WLAN
    int wlanRssi() { return WiFi.RSSI(); }          // Signalstaerke
    String wlanIp() { return WiFi.localIP().toString(); }  // IP-Adresse

    void wlanVerbinden(const char* ssid, const char* pw) { WiFi.begin(ssid, pw); }  // Verbinden
    void wlanTrennen() { WiFi.disconnect(true); }   // Trennen + Auto-Reconnect aus

    int wlanScanStarten() { WiFi.scanNetworks(true); return 0; }  // Async Scan
    int wlanScanErgebnis() { return WiFi.scanComplete(); }        // Scan-Status
    String wlanScanSsid(int i) { return WiFi.SSID(i); }           // SSID aus Scan
    int wlanScanRssi(int i) { return WiFi.RSSI(i); }              // RSSI aus Scan
    void wlanScanLoeschen() { WiFi.scanDelete(); }                // Scan-Ergebnisse freigeben

    // Loescht gespeicherte WLAN-Credentials (WiFiManager)
    void wlanCredentialsLoeschen() {
        WiFiManager wm;      // WiFiManager-Objekt
        wm.resetSettings();  // Gespeicherte SSIDs loeschen
    }

    // --- UDP ---

    WiFiUDP udpSocket;                   // UDP-Socket-Objekt
    const unsigned int LOKALER_PORT = 4210;   // Empfaenger-Port

    // Zwischenspeicher fuer Sender-Adresse (vor strtok sichern!)
    IPAddress letzterSenderIp;           // Absender-IP
    unsigned int letzterSenderPort = 0;  // Absender-Port
    IPAddress gebundeneSenderIp;          // Nach erster gueltiger HMAC gebundener Peer
    bool senderGebunden = false;

    // Startet UDP-Socket
    void udpStarten() {
        udpSocket.stop();
        udpSocket.begin(LOKALER_PORT);
        senderGebunden = false;           // Netzwechsel darf eine neue Peer-IP zulassen
    }

    // Prueft ob UDP-Paket empfangen wurde
    int udpPaketVerfuegbar() { return udpSocket.parsePacket(); }

    // Liest Paket und merkt sich Absender-Info fuer ACK
    int udpLesen(char* buf, size_t maxLen) {
        letzterSenderIp = udpSocket.remoteIP();    // Absender-IP merken
        letzterSenderPort = udpSocket.remotePort();  // Absender-Port merken
        return udpSocket.read(buf, maxLen);        // Paket lesen
    }

    // Sendet UDP-Antwort an letzten Absender
    void udpAntworten(const char* daten) {
        udpSocket.beginPacket(letzterSenderIp, letzterSenderPort);  // Paket starten
        udpSocket.print(daten);                    // Daten schreiben
        udpSocket.endPacket();                     // Paket absenden
    }

    bool udpAbsenderPlausibel() {
        if (letzterSenderPort != 4211) return false;
        return !senderGebunden || letzterSenderIp == gebundeneSenderIp;
    }

    void udpSenderBinden() {
        if (!senderGebunden) {
            gebundeneSenderIp = letzterSenderIp;
            senderGebunden = true;
        }
    }

    // Verwirft empfangenes Paket
    void udpFlush() { udpSocket.flush(); }

    // --- mDNS ---

    bool mdnsStarten(const char* hostname) { return MDNS.begin(hostname); }  // mDNS-Responder
    void mdnsUpdate() { MDNS.update(); }  // mDNS-Verarbeitung

    // --- Telnet ---

#if ALARM_ENABLE_TELNET
    void telnetStarten() { TelnetStream.begin(); }  // Telnet-Server starten
    bool telnetVerfuegbar() { return TelnetStream.available(); }  // Daten verfuegbar
    String telnetLesen() { String s = TelnetStream.readStringUntil('\n'); s.trim(); return s; }  // Zeile lesen
    void telnetSchreiben(const char* msg) { TelnetStream.println(msg); TelnetStream.flush(); }  // Zeile senden
    void telnetStoppen() { TelnetStream.stop(); }  // Telnet-Server stoppen
#else
    void telnetStarten() {}
    bool telnetVerfuegbar() { return false; }
    String telnetLesen() { return String(); }
    void telnetSchreiben(const char*) {}
    void telnetStoppen() {}
#endif

} // namespace HAL

// ============================================================================
// SECURITY - Sicherheitsfunktionen (plattformunabhaengig)
// ============================================================================

namespace Security {

    // Constant-Time Vergleich gegen Timing-Seitenkanalangriffe
    bool sichererVergleich(const char* a, const char* b, size_t laenge) {
        volatile uint8_t ergebnis = 0;               // Volatile gegen Optimierung
        for (size_t i = 0; i < laenge; i++) {        // Alle Bytes pruefen
            ergebnis |= (uint8_t)a[i] ^ (uint8_t)b[i];  // XOR akkumulieren
        }
        return ergebnis == 0;  // Nur 0 wenn alle Bytes gleich
    }

    // Prueft ob String nur aus Dezimalziffern besteht
    bool nurZiffern(const char* str) {
        if (str == NULL || str[0] == '\0') return false;  // Leerer String
        for (size_t i = 0; str[i] != '\0'; i++) {         // Alle Zeichen pruefen
            if (!isdigit((unsigned char)str[i])) return false;  // Nicht-Ziffer gefunden
        }
        return true;  // Alle Zeichen sind Ziffern
    }

    bool istHex(const char* str, size_t erwarteteLaenge) {
        if (str == NULL || strlen(str) != erwarteteLaenge) return false;
        for (size_t i = 0; i < erwarteteLaenge; i++) {
            if (!isxdigit((unsigned char)str[i])) return false;
        }
        return true;
    }

    void berechneHmacBytes(const char* nachricht, size_t nachrichtLen,
                           const char* secret, size_t secretLen,
                           uint8_t* result) {
        br_hmac_key_context kc;   // HMAC-Schluessel-Kontext
        br_hmac_context ctx;       // HMAC-Hash-Kontext
        br_hmac_key_init(&kc, &br_sha256_vtable, secret, secretLen);  // Schluessel initialisieren
        br_hmac_init(&ctx, &kc, 0);                  // HMAC-Kontext initialisieren
        br_hmac_update(&ctx, nachricht, nachrichtLen);  // Nachricht hashen
        br_hmac_out(&ctx, result);                   // Ergebnis extrahieren
    }

    // HMAC-SHA256 -> 64 Hex-Zeichen in hexOut (min. 65 Bytes)
    void berechneHMAC(const char* nachricht, size_t nachrichtLen,
                      const char* secret, size_t secretLen,
                      char* hexOut) {
        uint8_t result[32];
        berechneHmacBytes(nachricht, nachrichtLen, secret, secretLen, result);
        for (int i = 0; i < 32; i++)                 // In Hex konvertieren
            sprintf(hexOut + (i * 2), "%02x", result[i]);  // 2 Hex-Zeichen pro Byte
        hexOut[64] = '\0';                           // Null-Terminierung
    }

    // --- Strikter Replay-Schutz fuer zustandssetzende Befehle ---

    unsigned long replayWindowBase = 0;  // Hoechste akzeptierte Sequenznummer
    uint32_t replayBitmap = 0;           // Bitmap fuer letzte 25 Sequenzen
    unsigned long replayStateGeneration = 0;
    bool persistierterAlarm = false;
    bool replayStateCorrupt = false;

    enum ReplayErgebnis { REPLAY_NEU, REPLAY_DUPLIKAT, REPLAY_ZU_ALT };

    // Nur eine strikt hoehere Sequenz darf ALARM_ON/OFF anwenden. Ein exaktes
    // Duplikat wird nur erneut bestaetigt (ACK-Verlust); niedrigere, verspaetet
    // eintreffende Zustandsbefehle duerfen einen neueren Zustand nie umkehren.
    ReplayErgebnis pruefeReplay(unsigned long seq) {
        ReplaySchutz::Ergebnis ergebnis =
            ReplaySchutz::pruefeStrikt(seq, replayWindowBase, replayBitmap);
        if (ergebnis == ReplaySchutz::NEU) return REPLAY_NEU;
        if (ergebnis == ReplaySchutz::DUPLIKAT) return REPLAY_DUPLIKAT;
        return REPLAY_ZU_ALT;
    }

    struct ReplayStateRecord {
        unsigned long generation;
        unsigned long base;
        uint32_t bitmap;
        bool alarm;
    };

    bool leseReplayDatei(const char* pfad, ReplayStateRecord& state) {
        if (!LittleFS.exists(pfad)) return false;
        File f = LittleFS.open(pfad, "r");
        if (!f) return false;
        StaticJsonDocument<192> doc;
        DeserializationError error = deserializeJson(doc, f);
        f.close();
        if (error || !doc["generation"].is<unsigned long>() || !doc["base"].is<unsigned long>() ||
            !doc["bitmap"].is<unsigned long>() || !doc["alarm"].is<bool>()) return false;
        state.generation = doc["generation"].as<unsigned long>();
        state.base = doc["base"].as<unsigned long>();
        state.bitmap = doc["bitmap"].as<unsigned long>() & ((1UL << REPLAY_FENSTER_GROESSE) - 1UL);
        state.alarm = doc["alarm"].as<bool>();
        if (state.base > 0) state.bitmap |= 1UL;
        return state.base == 0 || state.bitmap != 0;
    }

    bool speichereReplayZustand(bool alarm) {
        unsigned long neueGeneration = replayStateGeneration + 1;
        StaticJsonDocument<192> doc;
        doc["generation"] = neueGeneration;
        doc["base"] = replayWindowBase;
        doc["bitmap"] = replayBitmap;
        doc["alarm"] = alarm;
        File f = LittleFS.open("/udp_replay.tmp", "w");
        if (!f) return false;
        if (serializeJson(doc, f) == 0) { f.close(); return false; }
        f.flush();
        f.close();
        ReplayStateRecord pruefung;
        if (!leseReplayDatei("/udp_replay.tmp", pruefung) || pruefung.generation != neueGeneration ||
            pruefung.alarm != alarm) return false;
        LittleFS.remove("/udp_replay.json");
        if (!LittleFS.rename("/udp_replay.tmp", "/udp_replay.json")) return false;
        if (!leseReplayDatei("/udp_replay.json", pruefung) || pruefung.generation != neueGeneration ||
            pruefung.alarm != alarm) return false;
        replayStateGeneration = neueGeneration;
        persistierterAlarm = alarm;
        return true;
    }

    bool speichereSequenz() { return speichereReplayZustand(persistierterAlarm); }

    bool leseLegacyReplay(const char* pfad, unsigned long& base, uint32_t& bitmap) {
        if (!LittleFS.exists(pfad)) return false;
        File f = LittleFS.open(pfad, "r");
        if (!f) return false;
        StaticJsonDocument<128> doc;
        DeserializationError error = deserializeJson(doc, f);
        f.close();
        if (error || !doc["base"].is<unsigned long>() || !doc["bitmap"].is<unsigned long>() ||
            doc.containsKey("generation") || doc.containsKey("alarm")) return false;
        base = doc["base"].as<unsigned long>();
        bitmap = doc["bitmap"].as<unsigned long>() & ((1UL << REPLAY_FENSTER_GROESSE) - 1UL);
        if (base > 0) bitmap |= 1UL;
        return base == 0 || bitmap != 0;
    }

    void ladeSequenz() {
        bool finalVorhanden = LittleFS.exists("/udp_replay.json");
        bool tempVorhanden = LittleFS.exists("/udp_replay.tmp");
        ReplayStateRecord finalState = {}, tempState = {};
        bool finalOk = leseReplayDatei("/udp_replay.json", finalState);
        bool tempOk = leseReplayDatei("/udp_replay.tmp", tempState);
        if (finalOk || tempOk) {
            ReplayStateRecord& state = tempOk && (!finalOk || tempState.generation > finalState.generation)
                                       ? tempState : finalState;
            replayStateGeneration = state.generation;
            replayWindowBase = state.base;
            replayBitmap = state.bitmap;
            persistierterAlarm = state.alarm;
            if (tempOk && !speichereReplayZustand(persistierterAlarm)) replayStateCorrupt = true;
            return;
        }

        unsigned long legacyBase = 0; uint32_t legacyBitmap = 0;
        if (leseLegacyReplay("/udp_replay.json", legacyBase, legacyBitmap) ||
            leseLegacyReplay("/udp_replay.tmp", legacyBase, legacyBitmap)) {
            replayWindowBase = legacyBase; replayBitmap = legacyBitmap; persistierterAlarm = false;
            if (!speichereReplayZustand(false)) replayStateCorrupt = true;
            return;
        }
        if (!finalVorhanden && !tempVorhanden && LittleFS.exists("/seq.dat")) {
            File f = LittleFS.open("/seq.dat", "r");
            char buf[12] = "";
            size_t len = f ? f.readBytes(buf, sizeof(buf) - 1) : 0;
            if (f) f.close();
            buf[len] = '\0';
            if (nurZiffern(buf)) {
                replayWindowBase = strtoul(buf, NULL, 10); replayBitmap = replayWindowBase ? 1UL : 0UL;
                if (!speichereReplayZustand(false)) replayStateCorrupt = true;
                return;
            }
            replayStateCorrupt = true; persistierterAlarm = true;
            return;
        }
        if (finalVorhanden || tempVorhanden || LittleFS.exists("/config.json") ||
            LittleFS.exists("/config.tmp")) {
            replayStateCorrupt = true;
            persistierterAlarm = true;
        } else if (!speichereReplayZustand(false)) {
            replayStateCorrupt = true;
            persistierterAlarm = true;
        }
    }

    // --- Signierte HTTP-Delivery: durables Write-Ahead-Apply-Journal ---
    unsigned long apiStateGeneration = 0;
    unsigned long apiSequence = 0;
    char lastAppliedId[33] = "";
    char pendingDeliveryAck[33] = "";
    bool apiApplyPending = false;
    unsigned long apiApplySequence = 0;
    char apiApplyId[33] = "";
    char apiApplyType[9] = "";
    char apiApplyPayload[513] = "";
    bool apiStateCorrupt = false;

    struct ApiStateRecord {
        unsigned long generation;
        unsigned long sequence;
        char last[33];
        char ack[33];
        bool pending;
        unsigned long pendingSequence;
        char pendingId[33];
        char pendingType[9];
        char pendingPayload[513];
    };

    bool leseApiZustand(const char* pfad, ApiStateRecord& state) {
        if (!LittleFS.exists(pfad)) return false;
        File f = LittleFS.open(pfad, "r");
        if (!f) return false;
        StaticJsonDocument<1024> doc;
        DeserializationError error = deserializeJson(doc, f);
        f.close();
        if (error || !doc["generation"].is<unsigned long>() ||
            !doc["sequence"].is<unsigned long>() || !doc["pending"].is<bool>()) return false;
        state.generation = doc["generation"].as<unsigned long>();
        state.sequence = doc["sequence"].as<unsigned long>();
        strlcpy(state.last, doc["last"] | "", sizeof(state.last));
        strlcpy(state.ack, doc["ack"] | "", sizeof(state.ack));
        state.pending = doc["pending"].as<bool>();
        state.pendingSequence = doc["pending_sequence"] | 0UL;
        strlcpy(state.pendingId, doc["pending_id"] | "", sizeof(state.pendingId));
        strlcpy(state.pendingType, doc["pending_type"] | "", sizeof(state.pendingType));
        strlcpy(state.pendingPayload, doc["pending_payload"] | "", sizeof(state.pendingPayload));
        if ((state.last[0] != '\0' && !istHex(state.last, 32)) ||
            (state.ack[0] != '\0' && !istHex(state.ack, 32))) return false;
        if (!state.pending) return state.pendingSequence == 0 && state.pendingId[0] == '\0' &&
                                   state.pendingType[0] == '\0' && state.pendingPayload[0] == '\0';
        return state.pendingSequence > state.sequence && istHex(state.pendingId, 32) &&
               (strcmp(state.pendingType, "config") == 0 || strcmp(state.pendingType, "command") == 0) &&
               strlen(state.pendingPayload) <= 512;
    }

    bool leseLegacyApiZustand(const char* pfad, unsigned long& sequence, char* ack) {
        if (!LittleFS.exists(pfad)) return false;
        File f = LittleFS.open(pfad, "r");
        if (!f) return false;
        StaticJsonDocument<192> doc;
        DeserializationError error = deserializeJson(doc, f);
        f.close();
        if (error || !doc["sequence"].is<unsigned long>() || doc.containsKey("generation") ||
            doc.containsKey("pending")) return false;
        sequence = doc["sequence"].as<unsigned long>();
        strlcpy(ack, doc["ack"] | "", 33);
        return ack[0] == '\0' || istHex(ack, 32);
    }

    bool speichereApiZustand() {
        unsigned long neueGeneration = apiStateGeneration + 1;
        StaticJsonDocument<1024> doc;
        doc["generation"] = neueGeneration;
        doc["sequence"] = apiSequence;
        doc["last"] = lastAppliedId;
        doc["ack"] = pendingDeliveryAck;
        doc["pending"] = apiApplyPending;
        doc["pending_sequence"] = apiApplyPending ? apiApplySequence : 0UL;
        doc["pending_id"] = apiApplyPending ? apiApplyId : "";
        doc["pending_type"] = apiApplyPending ? apiApplyType : "";
        doc["pending_payload"] = apiApplyPending ? apiApplyPayload : "";
        File f = LittleFS.open("/api_delivery.tmp", "w");
        if (!f) return false;
        if (serializeJson(doc, f) == 0) { f.close(); return false; }
        f.flush();
        f.close();
        ApiStateRecord pruefung;
        if (!leseApiZustand("/api_delivery.tmp", pruefung) || pruefung.generation != neueGeneration) return false;
        LittleFS.remove("/api_delivery.json");
        if (!LittleFS.rename("/api_delivery.tmp", "/api_delivery.json")) return false;
        if (!leseApiZustand("/api_delivery.json", pruefung) || pruefung.generation != neueGeneration) return false;
        apiStateGeneration = neueGeneration;
        return true;
    }

    void uebernehmeApiZustand(const ApiStateRecord& state) {
        apiStateGeneration = state.generation;
        apiSequence = state.sequence;
        strlcpy(lastAppliedId, state.last, sizeof(lastAppliedId));
        strlcpy(pendingDeliveryAck, state.ack, sizeof(pendingDeliveryAck));
        apiApplyPending = state.pending;
        apiApplySequence = state.pendingSequence;
        strlcpy(apiApplyId, state.pendingId, sizeof(apiApplyId));
        strlcpy(apiApplyType, state.pendingType, sizeof(apiApplyType));
        strlcpy(apiApplyPayload, state.pendingPayload, sizeof(apiApplyPayload));
    }

    void ladeApiZustand() {
        bool finalVorhanden = LittleFS.exists("/api_delivery.json");
        bool tempVorhanden = LittleFS.exists("/api_delivery.tmp");
        ApiStateRecord finalState = {}, tempState = {};
        bool finalOk = leseApiZustand("/api_delivery.json", finalState);
        bool tempOk = leseApiZustand("/api_delivery.tmp", tempState);
        if (!finalVorhanden && !tempVorhanden) {
            if (LittleFS.exists("/config.json") || LittleFS.exists("/config.tmp") ||
                !speichereApiZustand())
                apiStateCorrupt = true;
            return;
        }
        if (!finalOk && !tempOk) {
            unsigned long finalSeq = 0, tempSeq = 0; char finalAck[33] = "", tempAck[33] = "";
            bool finalLegacy = leseLegacyApiZustand("/api_delivery.json", finalSeq, finalAck);
            bool tempLegacy = leseLegacyApiZustand("/api_delivery.tmp", tempSeq, tempAck);
            if (finalLegacy || tempLegacy) {
                bool waehleTemp = tempLegacy && (!finalLegacy || tempSeq > finalSeq);
                apiSequence = waehleTemp ? tempSeq : finalSeq;
                strlcpy(pendingDeliveryAck, waehleTemp ? tempAck : finalAck, sizeof(pendingDeliveryAck));
                strlcpy(lastAppliedId, pendingDeliveryAck, sizeof(lastAppliedId));
                if (!speichereApiZustand()) apiStateCorrupt = true;
                return;
            }
            apiStateCorrupt = true; return;
        }
        uebernehmeApiZustand(tempOk && (!finalOk || tempState.generation > finalState.generation)
                            ? tempState : finalState);
        if (tempOk && !speichereApiZustand()) apiStateCorrupt = true;
    }

    bool beginApiApply(unsigned long sequence, const char* id, const char* type, const char* payload) {
        if (apiStateCorrupt || sequence <= apiSequence || !istHex(id, 32) ||
            (strcmp(type, "config") != 0 && strcmp(type, "command") != 0) || strlen(payload) > 512) return false;
        if (apiApplyPending) {
            return sequence == apiApplySequence && strcmp(id, apiApplyId) == 0 &&
                   strcmp(type, apiApplyType) == 0 && strcmp(payload, apiApplyPayload) == 0;
        }
        apiApplyPending = true;
        apiApplySequence = sequence;
        strlcpy(apiApplyId, id, sizeof(apiApplyId));
        strlcpy(apiApplyType, type, sizeof(apiApplyType));
        strlcpy(apiApplyPayload, payload, sizeof(apiApplyPayload));
        if (speichereApiZustand()) return true;
        apiApplyPending = false; apiApplySequence = 0;
        apiApplyId[0] = apiApplyType[0] = apiApplyPayload[0] = '\0';
        return false;
    }

    bool completeApiApply() {
        if (apiStateCorrupt || !apiApplyPending || apiApplySequence <= apiSequence) return false;
        unsigned long alteSequence = apiSequence;
        char altesLast[33], altesAck[33], alteId[33], alterTyp[9], alterPayload[513];
        strlcpy(altesLast, lastAppliedId, sizeof(altesLast));
        strlcpy(altesAck, pendingDeliveryAck, sizeof(altesAck));
        strlcpy(alteId, apiApplyId, sizeof(alteId));
        strlcpy(alterTyp, apiApplyType, sizeof(alterTyp));
        strlcpy(alterPayload, apiApplyPayload, sizeof(alterPayload));
        unsigned long altePendingSequence = apiApplySequence;
        apiSequence = apiApplySequence;
        strlcpy(lastAppliedId, apiApplyId, sizeof(lastAppliedId));
        strlcpy(pendingDeliveryAck, apiApplyId, sizeof(pendingDeliveryAck));
        apiApplyPending = false; apiApplySequence = 0;
        apiApplyId[0] = apiApplyType[0] = apiApplyPayload[0] = '\0';
        if (speichereApiZustand()) return true;
        apiSequence = alteSequence;
        strlcpy(lastAppliedId, altesLast, sizeof(lastAppliedId));
        strlcpy(pendingDeliveryAck, altesAck, sizeof(pendingDeliveryAck));
        apiApplyPending = true; apiApplySequence = altePendingSequence;
        strlcpy(apiApplyId, alteId, sizeof(apiApplyId));
        strlcpy(apiApplyType, alterTyp, sizeof(apiApplyType));
        strlcpy(apiApplyPayload, alterPayload, sizeof(apiApplyPayload));
        return false;
    }

    bool erneuereAckFuerDuplikat(const char* id) {
        if (apiStateCorrupt || !istHex(id, 32) || strcmp(id, lastAppliedId) != 0) return false;
        char vorherigesAck[33];
        strlcpy(vorherigesAck, pendingDeliveryAck, sizeof(vorherigesAck));
        strlcpy(pendingDeliveryAck, id, sizeof(pendingDeliveryAck));
        if (speichereApiZustand()) return true;
        strlcpy(pendingDeliveryAck, vorherigesAck, sizeof(pendingDeliveryAck));
        return false;
    }

    bool deliveryAckBestaetigt() {
        if (pendingDeliveryAck[0] == '\0') return true;
        char vorherigesAck[33];
        strlcpy(vorherigesAck, pendingDeliveryAck, sizeof(vorherigesAck));
        pendingDeliveryAck[0] = '\0';
        if (speichereApiZustand()) return true;
        strlcpy(pendingDeliveryAck, vorherigesAck, sizeof(pendingDeliveryAck));
        return false;
    }

} // namespace Security

// ============================================================================
// SERIELLE DIAGNOSE
// ============================================================================

// Gibt eine formatierte Diagnose-Nachricht auf dem seriellen Monitor aus.
// Format: [M:SS.mmm] [KATEG ] Nachricht
// Kategorien (6 Zeichen, linksbuendig): SYSTEM|WLAN|SCAN|SEC|HB|ALARM|TLNT|PROTO
void logSerial(const char* kategorie, const String& nachricht) {
    unsigned long ms  = millis();          // Aktuelle Zeit in ms
    unsigned long sec = ms / 1000;         // Sekunden
    unsigned long min = sec / 60;          // Minuten
    ms  %= 1000;                           // Rest-Millisekunden
    sec %= 60;                             // Rest-Sekunden
    char kopf[16];
    snprintf(kopf, sizeof(kopf), "[%lu:%02lu.%03lu]", min, sec, ms);  // [M:SS.mmm]
    char kat[9];
    snprintf(kat, sizeof(kat), "%-6s", kategorie);  // Kategorie auf 6 Zeichen
    Serial.print(kopf);
    Serial.print(" [");
    Serial.print(kat);
    Serial.print("] ");
    Serial.println(nachricht);
}

// ============================================================================
// GLOBALER ZUSTAND
// ============================================================================

SystemKonfiguration config;                  // System-Konfiguration
SystemZustand aktuellerZustand = ZUSTAND_INIT;  // Aktueller FSM-Zustand

// Flags
bool konfigurationSpeichern = false;  // Config-Save-Flag (WiFiManager Callback)
bool telnetAutorisiert = false;       // Telnet-Login erfolgreich
bool darfLoggen = false;              // Logging zur API erlaubt

// Alarm
volatile bool alarmAktiv = false;     // Alarm-Status (volatile fuer ISR-Safety)
bool ledPhase = false;                // LED-Toggle-Phase
unsigned long letzterToggle = 0;      // Timestamp letzter LED-Toggle

// Timing
unsigned long letzterVerbindungsVersuch = 0;  // WLAN-Reconnect-Timestamp
unsigned long letzterScanStart = 0;           // Netzwerk-Scan-Timestamp
unsigned long letzterHeartbeat = 0;           // Letzter Heartbeat
unsigned long letzterTelnetInput = 0;         // Letzter Telnet-Input (Auto-Logout)
unsigned long letztesBlinken = 0;             // LED-Blink-Timestamp
unsigned long aktuellesHeartbeatIntervall = 2000;  // Dynamisches Heartbeat-Intervall
uint8_t hbFehlversuche = 0;                       // Aufeinanderfolgende HB-Fehler (TCP-Stack-Bug-Detektion)
int scanStatus = -1;                          // Async-Scan-Status

// Sicherheit
uint8_t telnetFehlversuche = 0;       // Login-Fehlversuche
unsigned long telnetSperreBis = 0;    // Timestamp Sperrende
uint8_t udpPaketZaehler = 0;          // UDP-Pakete pro Minute (DoS-Schutz)
unsigned long udpZaehlerReset = 0;    // Timestamp letzter Zaehler-Reset

// ============================================================================
// CONFIG PERSISTENZ
// ============================================================================

bool configStateCorrupt = false;

// Callback von WiFiManager (wird bei Config-Aenderung aufgerufen)
void konfigurationSpeichernCallback() {
    konfigurationSpeichern = true;  // Flag setzen
}

// Laedt Konfiguration aus config.json
bool leseKonfigurationDatei(const char* pfad, SystemKonfiguration& ziel) {
    if (!LittleFS.exists(pfad)) return false;
    File datei = LittleFS.open(pfad, "r");
    if (!datei) return false;
    StaticJsonDocument<1024> doc;
    DeserializationError fehler = deserializeJson(doc, datei);
    datei.close();
    if (fehler || !doc["token"].is<const char*>() || !doc["apitoken"].is<const char*>() ||
        !doc["dhmac"].is<const char*>()) return false;
    strlcpy(ziel.udpToken, doc["token"] | "", sizeof(ziel.udpToken));
    strlcpy(ziel.mdnsName, doc["name"] | "", sizeof(ziel.mdnsName));
    strlcpy(ziel.telnetPasswort, doc["tpass"] | "", sizeof(ziel.telnetPasswort));
    strlcpy(ziel.backupSsid, doc["bssid"] | "", sizeof(ziel.backupSsid));
    strlcpy(ziel.backupPasswort, doc["bpass"] | "", sizeof(ziel.backupPasswort));
    strlcpy(ziel.hauptWlanName, doc["hssid"] | "", sizeof(ziel.hauptWlanName));
    strlcpy(ziel.hauptWlanPasswort, doc["hpass"] | "", sizeof(ziel.hauptWlanPasswort));
    strlcpy(ziel.apPasswort, doc["appw"] | "", sizeof(ziel.apPasswort));
    strlcpy(ziel.apiServer, doc["apiip"] | "", sizeof(ziel.apiServer));
    strlcpy(ziel.apiToken, doc["apitoken"] | "", sizeof(ziel.apiToken));
    strlcpy(ziel.apiHmacToken, doc["dhmac"] | "", sizeof(ziel.apiHmacToken));
    return true;
}

void ladeKonfiguration() {
    bool finalVorhanden = LittleFS.exists("/config.json");
    bool tempVorhanden = LittleFS.exists("/config.tmp");
    SystemKonfiguration finalConfig = config, tempConfig = config;
    bool finalOk = leseKonfigurationDatei("/config.json", finalConfig);
    bool tempOk = leseKonfigurationDatei("/config.tmp", tempConfig);
    if (tempOk) config = tempConfig;
    else if (finalOk) config = finalConfig;
    else if (finalVorhanden || tempVorhanden) configStateCorrupt = true;
}

// Speichert Konfiguration in config.json
bool speichereKonfiguration() {
    StaticJsonDocument<1024> doc;     // JSON-Dokument
    doc["token"] = config.udpToken;   // Token schreiben
    doc["name"] = config.mdnsName;    // mDNS-Name schreiben
    doc["tpass"] = config.telnetPasswort;  // Telnet-PW schreiben
    doc["bssid"] = config.backupSsid;      // Backup-SSID
    doc["bpass"] = config.backupPasswort;  // Backup-PW
    doc["hssid"] = config.hauptWlanName;   // Haupt-SSID
    doc["hpass"] = config.hauptWlanPasswort;  // Haupt-PW
    doc["appw"]  = config.apPasswort;      // AP-Passwort
    doc["apiip"] = config.apiServer;       // API-Server
    doc["apitoken"] = config.apiToken;     // API-Token
    doc["dhmac"] = config.apiHmacToken;    // Source-getrennter Delivery-HMAC
    File datei = LittleFS.open("/config.tmp", "w");
    if (!datei) return false;
    if (serializeJson(doc, datei) == 0) { datei.close(); return false; }
    datei.flush();
    datei.close();
    SystemKonfiguration pruefung = config;
    if (!leseKonfigurationDatei("/config.tmp", pruefung) ||
        strcmp(pruefung.apiToken, config.apiToken) != 0 ||
        strcmp(pruefung.apiHmacToken, config.apiHmacToken) != 0) return false;
    LittleFS.remove("/config.json");
    if (!LittleFS.rename("/config.tmp", "/config.json")) return false;
    if (!leseKonfigurationDatei("/config.json", pruefung)) return false;
    configStateCorrupt = false;
    return true;
}

bool remoteStringGueltig(JsonVariantConst wert, size_t maxLaenge) {
    return wert.is<const char*>() && strlen(wert.as<const char*>()) < maxLaenge;
}

bool wendeRemoteKonfigurationAn(const char* payload) {
    if (configStateCorrupt || payload == NULL || strlen(payload) > 512) return false;
    StaticJsonDocument<768> doc;
    if (deserializeJson(doc, payload) || !doc.is<JsonObject>()) return false;
    JsonObject obj = doc.as<JsonObject>();
    if (obj.size() == 0) return false;
    for (JsonPair kv : obj) {
        const char* key = kv.key().c_str();
        bool erlaubt = strcmp(key, "mssid") == 0 || strcmp(key, "mpass") == 0 ||
                        strcmp(key, "bssid") == 0 || strcmp(key, "bpass") == 0 ||
                        strcmp(key, "tpass") == 0;
        if (!erlaubt || !kv.value().is<const char*>()) return false;
    }
    if ((obj.containsKey("mssid") && !remoteStringGueltig(obj["mssid"], sizeof(config.hauptWlanName))) ||
        (obj.containsKey("mpass") && !remoteStringGueltig(obj["mpass"], sizeof(config.hauptWlanPasswort))) ||
        (obj.containsKey("bssid") && !remoteStringGueltig(obj["bssid"], sizeof(config.backupSsid))) ||
        (obj.containsKey("bpass") && !remoteStringGueltig(obj["bpass"], sizeof(config.backupPasswort))) ||
        (obj.containsKey("tpass") && !remoteStringGueltig(obj["tpass"], sizeof(config.telnetPasswort)))) return false;
    SystemKonfiguration vorher = config;
    if (obj.containsKey("mssid")) strlcpy(config.hauptWlanName, obj["mssid"], sizeof(config.hauptWlanName));
    if (obj.containsKey("mpass")) strlcpy(config.hauptWlanPasswort, obj["mpass"], sizeof(config.hauptWlanPasswort));
    if (obj.containsKey("bssid")) strlcpy(config.backupSsid, obj["bssid"], sizeof(config.backupSsid));
    if (obj.containsKey("bpass")) strlcpy(config.backupPasswort, obj["bpass"], sizeof(config.backupPasswort));
    if (obj.containsKey("tpass")) strlcpy(config.telnetPasswort, obj["tpass"], sizeof(config.telnetPasswort));
    if (speichereKonfiguration()) return true;
    config = vorher;
    return false;
}

bool setzeAlarmPersistent(bool aktiv) {
    if (Security::replayStateCorrupt || !Security::speichereReplayZustand(aktiv)) return false;
    alarmAktiv = aktiv;
    if (aktiv) {
        aktuellerZustand = ZUSTAND_ALARM;
        ledPhase = true;
        HAL::alarmHardwareSetzen(true);
    } else {
        ledPhase = false;
        HAL::alarmHardwareAus();
        if (aktuellerZustand == ZUSTAND_ALARM) aktuellerZustand = ZUSTAND_BEREIT;
    }
    return true;
}

bool wendeOffenesApiJournalAn() {
    if (Security::apiStateCorrupt) return false;
    if (!Security::apiApplyPending) return true;
    bool wirkungBestaetigt = false;
    if (strcmp(Security::apiApplyType, "config") == 0) {
        wirkungBestaetigt = wendeRemoteKonfigurationAn(Security::apiApplyPayload);
    } else if (strcmp(Security::apiApplyType, "command") == 0) {
        if (strcmp(Security::apiApplyPayload, "REBOOT") == 0) wirkungBestaetigt = true;
        else if (strcmp(Security::apiApplyPayload, "ALARM_ON") == 0) wirkungBestaetigt = setzeAlarmPersistent(true);
        else if (strcmp(Security::apiApplyPayload, "ALARM_OFF") == 0) wirkungBestaetigt = setzeAlarmPersistent(false);
    }
    return wirkungBestaetigt && Security::completeApiApply();
}

// ============================================================================
// LOGGING & PROTOKOLL
// ============================================================================

// Sendet Log-Nachricht an API-Server
void sendeLogAnApi(const char* nachricht) {
    if (!darfLoggen || !HAL::wlanVerbunden() || strlen(config.apiServer) == 0 || strlen(config.apiToken) < 16) return;
    // Im Alarm nur Alarm/UDP/Button-Logs durchlassen (kein Heartbeat-Spam)
    if (alarmAktiv && strncmp(nachricht, "ALARM", 5) != 0  // Kein ALARM-Log
                   && strncmp(nachricht, "UDP", 3) != 0    // Kein UDP-Log
                   && strncmp(nachricht, "Btn", 3) != 0) return;  // Kein Button-Log

    WiFiClient client;                    // HTTP-Client
    client.setTimeout(1000);              // Timeout 1s (im Alarm kurz)
    HTTPClient http;                      // HTTP-Client-Wrapper
    http.setTimeout(1000);                // Timeout 1s
    String serverPath = "http://" + String(config.apiServer) + "/api.php";  // API-URL
    if (http.begin(client, serverPath)) { // HTTP-Verbindung oeffnen
        http.addHeader("Content-Type", "application/json");  // Content-Type setzen
        if (strlen(config.apiToken) > 0) {  // Token vorhanden
            http.addHeader("Authorization", String("Bearer ") + config.apiToken);  // Bearer-Token
            http.addHeader("X-ESP-Token", config.apiToken);  // Custom-Header
        }
        StaticJsonDocument<256> doc;      // JSON-Dokument
        doc["source"] = "receiver";       // Absender
        doc["log"] = nachricht;           // Log-Nachricht
        String body;                      // JSON-String
        serializeJson(doc, body);         // JSON serialisieren
        http.POST(body);                  // POST-Request senden
        http.end();                       // Verbindung schliessen
    }
}

// Sendet Protokoll-Nachricht an alle Ausgabekanaele
void sendeProtokoll(const char* nachricht) {
    logSerial("PROTO", String(nachricht));  // Serial: formatiert mit Zeitstempel
    if (HAL::wlanVerbunden() && telnetAutorisiert)  // Telnet wenn verbunden und autorisiert
        HAL::telnetSchreiben(nachricht);  // Telnet-Ausgabe
    sendeLogAnApi(nachricht);             // API-Logging
}

// ============================================================================
// SERVICE-FUNKTIONEN
// ============================================================================

void sendeSigniertesAck(const char* seqString) {
    char material[40];
    int materialLen = snprintf(material, sizeof(material), "ACK_SECURE:%s", seqString);
    if (materialLen < 0 || (size_t)materialLen >= sizeof(material)) return;
    char sig[65];
    Security::berechneHMAC(material, (size_t)materialLen, config.udpToken, strlen(config.udpToken), sig);
    char ack[112];
    int ackLen = snprintf(ack, sizeof(ack), "%s:%s", material, sig);
    if (ackLen > 0 && (size_t)ackLen < sizeof(ack)) HAL::udpAntworten(ack);
}

// --- UDP-Empfang und Validierung ---
// Rueckgabe: 1 = ALARM_ON, -1 = ALARM_OFF, 0 = nichts/ungueltig
int verarbeiteUdpEmpfang() {
    int paketGroesse = HAL::udpPaketVerfuegbar();  // Paket empfangen?
    if (!paketGroesse) return 0;          // Kein Paket

    // char-Array basiertes Parsing (kein Arduino String auf dem Hot-Path)
    char puffer[512];                     // Buffer fuer Paket
    int laenge = HAL::udpLesen(puffer, sizeof(puffer) - 1);  // Paket lesen
    if (laenge <= 0) return 0;            // Lese-Fehler
    puffer[laenge] = '\0';                // Null-Terminierung
    if (!HAL::udpAbsenderPlausibel() || strlen(config.udpToken) < 32 ||
        Security::replayStateCorrupt) return 0;

    // Whitespace trimmen
    while (laenge > 0 && (puffer[laenge-1] == '\n' || puffer[laenge-1] == '\r' || puffer[laenge-1] == ' '))  // Whitespace am Ende
        puffer[--laenge] = '\0';          // Zeichen entfernen

    // Arbeitskopie fuer strtok (destruktiv)
    char arbeitskopie[512];               // Arbeitskopie
    strlcpy(arbeitskopie, puffer, sizeof(arbeitskopie));  // Kopieren

    // Format: "BEFEHL:SEQUENZNUMMER:HMAC_SIGNATUR"
    char* befehl = strtok(arbeitskopie, ":");  // Befehl extrahieren
    char* seqString = strtok(NULL, ":");       // Sequenz extrahieren
    char* empfangeneSignatur = strtok(NULL, ":");  // Signatur extrahieren

    if (befehl == NULL || seqString == NULL || empfangeneSignatur == NULL) return 0;  // Unvollstaendig
    if (strtok(NULL, ":") != NULL) return 0;  // Zu viele Felder

    bool istAlarmAn  = strcmp(befehl, CMD_ALARM_AN) == 0;
    bool istAlarmAus = strcmp(befehl, CMD_ALARM_AUS) == 0;
    if (!istAlarmAn && !istAlarmAus) return 0; // Unbekannte Payload nie zaehlen/persistieren

    // isdigit-Validierung
    if (!Security::nurZiffern(seqString)) return 0;  // Keine gueltige Zahl

    // HMAC berechnen und vergleichen
    char payload[256];                    // Payload-Buffer
    int payloadLen = snprintf(payload, sizeof(payload), "%s:%s", befehl, seqString);  // Payload bauen
    if (payloadLen < 0 || (size_t)payloadLen >= sizeof(payload)) return 0;  // Overflow

    char berechneteSignatur[65];          // HMAC-Buffer
    Security::berechneHMAC(payload, (size_t)payloadLen,
                           config.udpToken, strlen(config.udpToken),
                           berechneteSignatur);  // HMAC berechnen

    // Signaturlaenge pruefen
    if (!Security::istHex(empfangeneSignatur, 64)) return 0;

    // Lowercase normalisieren
    char sigLower[65];                    // Buffer fuer lowercase
    for (size_t i = 0; i < 64; i++)       // Alle Zeichen
        sigLower[i] = (char)tolower((unsigned char)empfangeneSignatur[i]);  // Zu lowercase
    sigLower[64] = '\0';                  // Null-Terminierung

    // Constant-Time Vergleich
    if (!Security::sichererVergleich(berechneteSignatur, sigLower, 64))  // Signatur falsch
        return 0;                         // Stillschweigend ignorieren

    // Replay-Window pruefen
    unsigned long empfangeneSeq = strtoul(seqString, NULL, 10);  // String zu Zahl
    char kanonischeSeq[12];
    snprintf(kanonischeSeq, sizeof(kanonischeSeq), "%lu", empfangeneSeq);
    if (empfangeneSeq == 0 || strcmp(kanonischeSeq, seqString) != 0) return 0;
    unsigned long alteBase = Security::replayWindowBase;
    uint32_t alteBitmap = Security::replayBitmap;
    Security::ReplayErgebnis replay = Security::pruefeReplay(empfangeneSeq);
    if (replay == Security::REPLAY_ZU_ALT) return 0;
    if (replay == Security::REPLAY_DUPLIKAT) {
        // Ein Duplikat darf einen nach Reboot noch ungebundenen Peer nicht an
        // eine gespoofte Quell-IP binden. Bei bestehender Bindung hat die
        // Plausibilitaetspruefung oben bereits exakt denselben Peer verlangt.
        sendeSigniertesAck(seqString);    // ACK-Verlust: sicher erneut bestaetigen
        return 0;
    }

    // Nur ein neuer, strukturell gueltiger, bekannter und HMAC-authentifizierter
    // Befehl verbraucht das Kontingent. Replay-Spam kann legitime Befehle nicht sperren.
    if (HAL::zeitMs() - udpZaehlerReset >= 60000) {
        udpPaketZaehler = 0;
        udpZaehlerReset = HAL::zeitMs();
    }
    if (udpPaketZaehler >= UDP_MAX_PAKETE_PRO_MINUTE) {
        Security::replayWindowBase = alteBase;
        Security::replayBitmap = alteBitmap;
        return 0;
    }
    udpPaketZaehler++;
    bool neuerAlarmzustand = istAlarmAn;
    if (!Security::speichereReplayZustand(neuerAlarmzustand)) {
        Security::replayWindowBase = alteBase;
        Security::replayBitmap = alteBitmap;
        return 0;                         // Ohne persistente Replay-Sperre keine Wirkung/kein ACK
    }
    HAL::udpSenderBinden();
    bool vorherAktiv = alarmAktiv;
    alarmAktiv = neuerAlarmzustand;
    if (neuerAlarmzustand) {
        aktuellerZustand = ZUSTAND_ALARM;
        ledPhase = true;
        HAL::alarmHardwareSetzen(true);   // Wirkung vor ACK physisch anwenden
    } else {
        ledPhase = false;
        HAL::alarmHardwareAus();
        aktuellerZustand = ZUSTAND_BEREIT;
    }
    sendeSigniertesAck(seqString);        // Erst nach durablem Zustand + Hardware-Wirkung
    if (neuerAlarmzustand && !vorherAktiv) sendeProtokoll("ALARM ON (UDP)");
    if (!neuerAlarmzustand && vorherAktiv) sendeProtokoll("ALARM OFF (UDP)");
    return neuerAlarmzustand ? 1 : -1;
}

// --- Alarm-Hardware aktualisieren ---
void aktualisiereAlarmHardware() {
    if (alarmAktiv) {                     // Alarm aktiv
        if (HAL::zeitMs() - letzterToggle >= ALARM_TOGGLE_INTERVALL) {  // Toggle-Intervall abgelaufen
            letzterToggle = HAL::zeitMs();  // Timestamp aktualisieren
            ledPhase = !ledPhase;         // Phase umschalten
            HAL::alarmHardwareSetzen(ledPhase);  // Hardware setzen
        }
    } else {                              // Alarm nicht aktiv
        if (ledPhase) {                   // Hardware noch an
            HAL::alarmHardwareAus();      // Alles ausschalten
            ledPhase = false;             // Phase zuruecksetzen
        }
    }
}

// --- Taster (Toggle + Reset) ---
// Rueckgabe: 1 = Toggle, 2 = Werksreset, 0 = nichts
int verarbeiteTaster() {
    static unsigned long druckStart = 0;  // Timestamp Druck-Beginn

    if (HAL::tasterGedrueckt()) {         // Taster gedrueckt
        if (druckStart == 0) druckStart = HAL::zeitMs();  // Timestamp merken
        return 0;                         // Noch nicht ausgeloest
    }

    if (druckStart == 0) return 0;        // War nicht gedrueckt

    unsigned long dauer = HAL::zeitMs() - druckStart;  // Druck-Dauer
    druckStart = 0;                       // Timestamp zuruecksetzen

    if (dauer > TASTER_RESET_DRUCK) return 2;        // Factory Reset
    if (dauer < TASTER_LANG_DRUCK) return 1;          // Toggle Alarm
    return 0;                             // Zwischen den Schwellwerten
}

bool pruefeApiDelivery(JsonObject envelope, String& typ, String& payload,
                       String& id, String& nonce, unsigned long& sequence,
                       bool& bereitsAngewendet) {
    bereitsAngewendet = false;
    if (Security::apiStateCorrupt || strlen(config.apiHmacToken) < 32 ||
        (int)(envelope["version"] | 0) != 2) return false;
    id = String((const char*)(envelope["id"] | ""));
    typ = String((const char*)(envelope["type"] | ""));
    nonce = String((const char*)(envelope["nonce"] | ""));
    payload = String((const char*)(envelope["payload"] | ""));
    String sig = String((const char*)(envelope["sig"] | ""));
    sequence = envelope["sequence"] | 0UL;
    if (!Security::istHex(id.c_str(), 32) || !Security::istHex(sig.c_str(), 64)) return false;
    bool duplikat = sequence == Security::apiSequence && id == String(Security::lastAppliedId);
    if (sequence == 0 || sequence < Security::apiSequence ||
        (sequence == Security::apiSequence && !duplikat) ||
        (typ != "command" && typ != "config") || payload.length() > 768) return false;
    if ((typ == "config" && !Security::istHex(nonce.c_str(), 32)) || (typ == "command" && nonce.length() != 0)) return false;

    String material = "ALARMv2\nreceiver\n" + id + "\n" + String(sequence) + "\n" + typ + "\n" + nonce + "\n" + payload;
    char erwartet[65];
    Security::berechneHMAC(material.c_str(), material.length(), config.apiHmacToken, strlen(config.apiHmacToken), erwartet);
    char sigLower[65];
    for (size_t i = 0; i < 64; i++) sigLower[i] = (char)tolower((unsigned char)sig[i]);
    sigLower[64] = '\0';
    bool gueltig = Security::sichererVergleich(erwartet, sigLower, 64);
    bereitsAngewendet = gueltig && duplikat;
    return gueltig;
}

bool entschluessleApiConfig(const String& id, const String& nonce,
                            const String& ciphertext, String& plaintext) {
    if (!Security::istHex(nonce.c_str(), 32) || ciphertext.length() == 0 ||
        ciphertext.length() > 704 || (ciphertext.length() % 4) != 0) return false;
    for (size_t i = 0; i < ciphertext.length(); i++) {
        char c = ciphertext[i];
        if (!(isalnum((unsigned char)c) || c == '+' || c == '/' || c == '=')) return false;
    }
    char decoded[529];
    int decodedLen = base64_decode_chars(ciphertext.c_str(), (int)ciphertext.length(), decoded);
    if (decodedLen <= 0 || decodedLen > 512) return false;
    for (int offset = 0, block = 0; offset < decodedLen; offset += 32, block++) {
        String material = "ALARMv2ENC\nreceiver\n" + id + "\n" + nonce + "\n" + String(block);
        uint8_t stream[32];
        Security::berechneHmacBytes(material.c_str(), material.length(), config.apiHmacToken, strlen(config.apiHmacToken), stream);
        int chunk = min(32, decodedLen - offset);
        for (int i = 0; i < chunk; i++) decoded[offset + i] ^= (char)stream[i];
    }
    decoded[decodedLen] = '\0';
    plaintext = String(decoded);
    return (int)plaintext.length() == decodedLen;
}

void berechneDeliveryAckSignatur(const char* id, char* sig) {
    String material = "ALARMv2ACK\nreceiver\n" + String(id);
    Security::berechneHMAC(material.c_str(), material.length(), config.apiHmacToken, strlen(config.apiHmacToken), sig);
}

// --- Heartbeat mit Server-Befehlen ---
void verarbeiteHeartbeat() {
    if (HAL::zeitMs() - letzterHeartbeat < aktuellesHeartbeatIntervall) return;  // Intervall noch nicht abgelaufen
    if (!HAL::wlanVerbunden()) { letzterHeartbeat = HAL::zeitMs(); return; }  // Nicht verbunden
    if (strlen(config.apiServer) == 0 || strlen(config.apiToken) < 16) { letzterHeartbeat = HAL::zeitMs(); return; }
    // Im Backup-Netz: Pi nicht erreichbar, HB sinnlos und blockiert TCP-Stack
    if (strlen(config.backupSsid) > 0 && HAL::wlanSsid() == String(config.backupSsid)) {
        letzterHeartbeat = HAL::zeitMs();  // Timer zuruecksetzen, kein Retry-Spam
        return;
    }

    WiFiClient client;                    // HTTP-Client
    client.setTimeout(1000);              // Timeout 1s
    HTTPClient http;                      // HTTP-Client-Wrapper
    http.setTimeout(1000);                // Timeout 1s
    String serverPath = "http://" + String(config.apiServer) + "/api.php";  // API-URL
    bool neustartNachAntwort = false;

    if (http.begin(client, serverPath)) { // HTTP-Verbindung oeffnen
        logSerial("HB", "-> " + String(config.apiServer) + " | Heap: " + String(HAL::freierHeap()));  // Log
        http.addHeader("Content-Type", "application/json");  // Content-Type setzen
        if (strlen(config.apiToken) > 0) {  // Token vorhanden
            http.addHeader("Authorization", String("Bearer ") + config.apiToken);  // Bearer-Token
            http.addHeader("X-ESP-Token", config.apiToken);  // Custom-Header
        }
        StaticJsonDocument<512> doc;      // JSON-Dokument inkl. signiertem Delivery-ACK
        doc["source"] = "receiver";       // Absender
        bool ackGesendet = Security::pendingDeliveryAck[0] != '\0';
        if (ackGesendet) {
            doc["delivery_ack"] = Security::pendingDeliveryAck;
            char ackSig[65];
            berechneDeliveryAckSignatur(Security::pendingDeliveryAck, ackSig);
            doc["delivery_ack_sig"] = ackSig;
        }
        doc["ip"] = HAL::wlanIp();        // IP-Adresse
        doc["status_msg"] = alarmAktiv ? "ALARM" : "Bereit";  // Status
        doc["alarm_state"] = alarmAktiv;  // Alarm-Flag
        doc["rssi"] = HAL::wlanRssi();    // Signalstaerke
        doc["heap"] = HAL::freierHeap();  // Freier RAM
        doc["uptime"] = millis() / 1000;  // Uptime in Sekunden
        doc["reset_reason"] = ESP.getResetReason();  // Reset-Grund

        String body;                      // JSON-String
        serializeJson(doc, body);         // JSON serialisieren
        int code = http.POST(body);       // POST-Request senden

        if (code > 0) {                   // Antwort erhalten
            aktuellesHeartbeatIntervall = HEARTBEAT_INTERVALL;  // Normales Intervall
            hbFehlversuche = 0;           // TCP-Stack offensichtlich gesund

            if (code == 200) {            // Erfolgreiche Antwort
                if (ackGesendet) Security::deliveryAckBestaetigt();
                logSerial("HB", "<- 200 OK | " + HAL::wlanSsid() + " | " + String(HAL::wlanRssi()) + " dBm");
                String payload = http.getString();  // Response-Body
                StaticJsonDocument<1024> antwort;   // JSON-Dokument
                deserializeJson(antwort, payload);  // JSON parsen

                if (!antwort.isNull()) {  // JSON gueltig
                    if (antwort.containsKey("logging_active"))  // Logging-Flag
                        darfLoggen = antwort["logging_active"];  // Flag setzen

                    if (antwort.containsKey("delivery")) {
                        String typ, deliveryPayload, deliveryId, deliveryNonce;
                        unsigned long deliverySequence = 0;
                        bool bereitsAngewendet = false;
                        JsonObject envelope = antwort["delivery"];
                        if (pruefeApiDelivery(envelope, typ, deliveryPayload, deliveryId, deliveryNonce,
                                             deliverySequence, bereitsAngewendet)) {
                            if (bereitsAngewendet) {
                                Security::erneuereAckFuerDuplikat(deliveryId.c_str());
                            } else if (typ == "config") {
                                String configPlaintext;
                                if (!entschluessleApiConfig(deliveryId, deliveryNonce, deliveryPayload, configPlaintext)) {
                                    logSerial("SEC", "Remote-Config konnte nicht entschluesselt werden");
                                } else if (Security::beginApiApply(deliverySequence, deliveryId.c_str(), "config",
                                                                   configPlaintext.c_str()) &&
                                           wendeRemoteKonfigurationAn(configPlaintext.c_str()) &&
                                           Security::completeApiApply()) {
                                    neustartNachAntwort = true;
                                } else {
                                    logSerial("SEC", "Remote-Config nicht durable angewendet");
                                }
                            } else if (deliveryPayload == "REBOOT" &&
                                       Security::beginApiApply(deliverySequence, deliveryId.c_str(), "command", "REBOOT") &&
                                       Security::completeApiApply()) {
                                neustartNachAntwort = true;
                            } else if ((deliveryPayload == "ALARM_ON" || deliveryPayload == "ALARM_OFF") &&
                                       Security::beginApiApply(deliverySequence, deliveryId.c_str(), "command",
                                                               deliveryPayload.c_str())) {
                                bool alarmSollAktivSein = deliveryPayload == "ALARM_ON";
                                if (!setzeAlarmPersistent(alarmSollAktivSein) || !Security::completeApiApply())
                                    logSerial("SEC", "Alarm-Delivery nicht durable angewendet");
                            }
                        } else {
                            logSerial("SEC", "API-Delivery abgewiesen (Signatur/Replay/Persistenz)");
                        }
                    }
                }
            }
        } else {                          // Fehler oder Timeout (inkl. HTTP -1)
            logSerial("HB", "<- HTTP-Fehler " + String(code) + ", Intervall 60s");  // Log
            aktuellesHeartbeatIntervall = 60000;  // Langsamer retries (1 Minute)
            hbFehlversuche++;             // Fehlversuch zaehlen
            // Fix Bug 2: ESP8266 TCP-Stack-Bug: nach 3x -1 WiFi-Reconnect erzwingen
            if (hbFehlversuche >= 3) {
                logSerial("WLAN", "TCP-Stack-Bug erkannt, erzwinge WiFi-Reconnect...");
                hbFehlversuche = 0;
                HAL::wlanTrennen();
                unsigned long t = HAL::zeitMs();
                while (HAL::zeitMs() - t < 500) { HAL::watchdogFuettern(); HAL::cpuFreigeben(); }
                HAL::wlanVerbinden(config.hauptWlanName, config.hauptWlanPasswort);
                letzterVerbindungsVersuch = HAL::zeitMs();
            }
        }
        http.end();                       // Verbindung schliessen
        if (neustartNachAntwort) {
            delay(250);
            HAL::neustart();
        }
    } else {                              // Verbindung fehlgeschlagen
        logSerial("HB", "Verbindungsfehler, Intervall 60s");  // Log
        aktuellesHeartbeatIntervall = 60000;  // Langsamer retries
        hbFehlversuche++;
        if (hbFehlversuche >= 3) {
            logSerial("WLAN", "TCP-Stack-Bug erkannt, erzwinge WiFi-Reconnect...");
            hbFehlversuche = 0;
            HAL::wlanTrennen();
            unsigned long t = HAL::zeitMs();
            while (HAL::zeitMs() - t < 500) { HAL::watchdogFuettern(); HAL::cpuFreigeben(); }
            HAL::wlanVerbinden(config.hauptWlanName, config.hauptWlanPasswort);
            letzterVerbindungsVersuch = HAL::zeitMs();
        }
    }
    letzterHeartbeat = HAL::zeitMs();     // Timestamp aktualisieren
}

// --- WLAN-LED ---
void aktualisiereWlanLed() {
    if (HAL::wlanVerbunden()) {           // Verbunden
        HAL::wlanLed(true);               // LED an
    } else {                              // Nicht verbunden
        if (HAL::zeitMs() - letztesBlinken >= BLINK_INTERVALL) {  // Blink-Intervall abgelaufen
            HAL::wlanLedToggle();         // LED umschalten
            letztesBlinken = HAL::zeitMs();  // Timestamp aktualisieren
        }
    }
}

// --- WLAN Failover ---
void verwalteWlanVerbindung() {
    static int stabilitaetsZaehler = 0;        // Zaehlt stabile Hauptnetz-Scans
    static uint8_t hauptnetz_fehlversuche = 0;  // Zaehlt Reconnect-Fehlversuche beim Hauptnetz
    const  uint8_t MAX_HAUPTNETZ_VERSUCHE  = 2; // Max 2x Hauptnetz probieren, dann Backup
    static bool udp_auf_backup_neugestartet = false;  // UDP einmalig nach Backup-Switch neu binden

    // Fix: Non-blocking Rueckwechsel zum Hauptnetz.
    // Problem vorher: while(!verbunden, max 15s) blockierte den loop() komplett -> Receiver war
    // in dieser Zeit taub fuer UDP-Alarmsignale vom Sender.
    static bool reconnect_laeuft               = false;  // Rueckwechsel-Versuch aktiv
    static bool reconnect_verbindung_gestartet = false;  // wlanVerbinden() bereits aufgerufen
    static unsigned long reconnect_ts          = 0;      // Startzeitpunkt des Rueckwechsels

    // --- Non-blocking Rueckwechsel-Phase (hat Vorrang vor allem anderen) ---
    if (reconnect_laeuft) {
        unsigned long abgelaufen = HAL::zeitMs() - reconnect_ts;  // Zeit seit Trennung
        if (abgelaufen < 400) { HAL::cpuFreigeben(); return; }    // Trennpause abwarten (non-blocking)
        if (!reconnect_verbindung_gestartet) {                     // Verbindungsaufbau noch nicht gestartet
            HAL::wlanVerbinden(config.hauptWlanName, config.hauptWlanPasswort);
            reconnect_verbindung_gestartet = true;
            logSerial("WLAN", "Rueckwechsel: Verbindungsaufbau laeuft...");
        }
        if (HAL::wlanVerbunden()) {                                // Rueckwechsel erfolgreich
            HAL::udpStarten();                                     // UDP an neue Netz-IP binden
            HAL::mdnsStarten(config.mdnsName);                     // mDNS mit Hauptnetz-IP neu initialisieren
            letzterVerbindungsVersuch = HAL::zeitMs();             // Reconnect-Cooldown setzen
            letzterHeartbeat = 0;                                  // Sofortiger HB nach Rueckkehr
            udp_auf_backup_neugestartet = false;                   // Flag reset fuer naechsten Backup-Switch
            reconnect_laeuft = false;
            reconnect_verbindung_gestartet = false;
            logSerial("WLAN", "Hauptnetz aktiv | " + HAL::wlanIp() + " | " + String(HAL::wlanRssi()) + " dBm");
        } else if (abgelaufen > 15400) {                           // Timeout nach 15s
            logSerial("WLAN", "Rueckwechsel fehlgeschlagen, bleibe im Backup-Netz");
            HAL::wlanTrennen();                                    // Halboffene Verbindung schliessen
            unsigned long t2 = HAL::zeitMs();
            while (HAL::zeitMs() - t2 < 300) { HAL::watchdogFuettern(); HAL::cpuFreigeben(); }
            HAL::wlanVerbinden(config.backupSsid, config.backupPasswort);  // Zurueck ins Backup
            letzterScanStart = HAL::zeitMs();                      // Scan-Cooldown neu starten
            reconnect_laeuft = false;
            reconnect_verbindung_gestartet = false;
            hauptnetz_fehlversuche = 0;
        }
        return;  // Keine normale Verarbeitung waehrend Rueckwechsel
    }

    if (HAL::wlanVerbunden()) {           // Verbunden
        String ssid = HAL::wlanSsid();    // Aktuelle SSID
        if (ssid == String(config.backupSsid) && strlen(config.backupSsid) > 0) {  // Im Backup-Netz
            // UDP einmalig nach Backup-Switch neu binden (alte IP vom Hauptnetz ist veraltet)
            if (!udp_auf_backup_neugestartet) {
                HAL::udpStarten();                      // UDP an neue Backup-IP binden
                HAL::mdnsStarten(config.mdnsName);      // mDNS neu starten -> kuendigt neue Backup-IP an
                                                        // (Sender loest dann korrekte Backup-IP via mDNS auf)
                logSerial("WLAN", "Backup aktiv | " + HAL::wlanIp() + " | UDP+mDNS neu gebunden");
                udp_auf_backup_neugestartet = true;
            }
            if (HAL::zeitMs() - letzterScanStart > WLAN_SCAN_INTERVALL && scanStatus == -1) {  // Scan starten
                letzterScanStart = HAL::zeitMs();  // Timestamp merken
                HAL::wlanScanStarten();   // Async-Scan starten
                scanStatus = 0;           // Status auf "laufend"
                logSerial("SCAN", "Scanne nach Hauptnetz: " + String(config.hauptWlanName));  // Log
            }
            if (scanStatus == 0) {        // Scan laeuft
                int n = HAL::wlanScanErgebnis();  // Scan-Ergebnis abrufen
                if (n >= 0) {             // Scan fertig
                    bool gefunden = false;  // Hauptnetz gefunden
                    for (int i = 0; i < n; i++) {  // Alle Netze durchgehen
                        if (HAL::wlanScanSsid(i) == String(config.hauptWlanName) &&  // Hauptnetz gefunden
                            String(config.hauptWlanName).length() > 0 &&  // Hauptnetz konfiguriert
                            HAL::wlanScanRssi(i) > RSSI_SCHWELLWERT) {  // Signal stark genug
                            gefunden = true;  // Markieren
                            break;        // Schleife abbrechen
                        }
                    }
                    if (gefunden) {       // Hauptnetz ist wieder da
                        stabilitaetsZaehler++;  // Zaehler erhoehen
                        logSerial("SCAN", "Hauptnetz erkannt (Stabilitaet: " + String(stabilitaetsZaehler) + "/" + String(STABILITAETS_SCHWELLWERT) + ")");  // Log
                        if (stabilitaetsZaehler >= STABILITAETS_SCHWELLWERT) {  // 3x stabil erkannt
                            stabilitaetsZaehler = 0;     // Zaehler zuruecksetzen (verhindert Endlosschleife)
                            hauptnetz_fehlversuche = 0;  // Fehlversuche-Counter zuruecksetzen
                            logSerial("WLAN", "Rueckwechsel -> Hauptnetz wird gestartet");  // Log
                            for (int k = 0; k < 4; k++) { HAL::wlanLedToggle(); delay(50); }  // Kurzes visuelles Signal (200ms)
                            HAL::wlanTrennen();          // Backup-Verbindung trennen
                            reconnect_laeuft = true;     // Non-blocking Rueckwechsel starten
                            reconnect_verbindung_gestartet = false;
                            reconnect_ts = HAL::zeitMs();  // Timestamp fuer Trennpause setzen
                        }
                    } else {              // Hauptnetz nicht gefunden
                        if (stabilitaetsZaehler > 0)
                            logSerial("SCAN", "Hauptnetz nicht stabil, Zaehler zurueckgesetzt");  // Log
                        stabilitaetsZaehler = 0;  // Zaehler zuruecksetzen
                    }
                    HAL::wlanScanLoeschen();  // Scan-Ergebnisse freigeben
                    scanStatus = -1;      // Status zuruecksetzen
                }
            }
        } else {                              // Im Hauptnetz oder unbekanntes Netz
            stabilitaetsZaehler = 0;          // Stabilitaets-Zaehler zuruecksetzen
            hauptnetz_fehlversuche = 0;        // Fehlversuche zuruecksetzen (Hauptnetz aktiv)
            udp_auf_backup_neugestartet = false;  // Flag reset (Hauptnetz aktiv)
        }
    } else {                                  // Nicht verbunden
        if (HAL::zeitMs() - letzterVerbindungsVersuch > WLAN_WIEDERHOLUNGS_INTERVALL) {
            letzterVerbindungsVersuch = HAL::zeitMs();  // Timestamp immer zuerst setzen
            HAL::wlanTrennen();               // Haengende Verbindung sauber trennen
            unsigned long start = HAL::zeitMs();
            while (HAL::zeitMs() - start < 300) { HAL::watchdogFuettern(); HAL::cpuFreigeben(); }  // 300ms Pause
            bool haupt_ok = strlen(config.hauptWlanName) > 0;  // Hauptnetz konfiguriert?
            bool backup_ok = strlen(config.backupSsid)   > 0;  // Backup konfiguriert?
            if (haupt_ok && hauptnetz_fehlversuche < MAX_HAUPTNETZ_VERSUCHE) {
                // Prioritaet: Hauptnetz (max MAX_HAUPTNETZ_VERSUCHE Fehlversuche toleriert)
                logSerial("WLAN", "Reconnect -> Hauptnetz (Versuch " + String(hauptnetz_fehlversuche + 1) + "/" + String(MAX_HAUPTNETZ_VERSUCHE) + ")");  // Log
                HAL::wlanVerbinden(config.hauptWlanName, config.hauptWlanPasswort);
                hauptnetz_fehlversuche++;     // Fehlversuch zaehlen
            } else if (backup_ok) {
                // Fallback: Backup-Netz (naechste Runde wieder Hauptnetz)
                logSerial("WLAN", "Reconnect -> Backup-Netz: " + String(config.backupSsid));  // Log
                HAL::wlanVerbinden(config.backupSsid, config.backupPasswort);
                hauptnetz_fehlversuche = 0;   // Naechste Runde wieder mit Hauptnetz anfangen
                letzterScanStart = HAL::zeitMs();  // Scan-Cooldown neu starten
            } else if (haupt_ok) {
                // Nur Hauptnetz konfiguriert (kein Backup vorhanden)
                logSerial("WLAN", "Reconnect -> Hauptnetz (kein Backup konfiguriert)");  // Log
                HAL::wlanVerbinden(config.hauptWlanName, config.hauptWlanPasswort);
                hauptnetz_fehlversuche = 0;
            }
            stabilitaetsZaehler = 0;          // Stabilitaets-Zaehler bei jedem Reconnect-Versuch zuruecksetzen
        }
    }
}

// --- Telnet ---
void pruefeTelnetZugang() {
    // Fix MITTEL-3: Automatischer Telnet-Reset nach Sperrablauf (verhindert permanenten Lockout durch Nmap)
    if (telnetSperreBis > 0 && HAL::zeitMs() >= telnetSperreBis) {
        telnetSperreBis = 0;          // Sperre loeschen
        telnetFehlversuche = 0;       // Fehlversuche zuruecksetzen
        HAL::telnetStarten();         // Telnet-Server wieder aktivieren
        logSerial("TLNT", "Sperre abgelaufen, Server wieder aktiv");  // Log
    }
    if (HAL::zeitMs() < telnetSperreBis) return;  // Noch gesperrt

    // Auto-Logout nach 5 Minuten Inaktivitaet
    if (telnetAutorisiert && (HAL::zeitMs() - letzterTelnetInput > TELNET_TIMEOUT)) {  // Timeout
        telnetAutorisiert = false;        // Logout
        HAL::telnetSchreiben("\n--- AUTO LOGOUT ---");  // Info-Nachricht
    }

    if (!HAL::telnetVerfuegbar()) return;  // Keine Eingabe
    letzterTelnetInput = HAL::zeitMs();   // Timestamp aktualisieren

    String eingabe = HAL::telnetLesen();  // Zeile lesen
    if (eingabe.length() == 0) return;    // Leer

    if (eingabe == String(config.telnetPasswort)) {  // Passwort korrekt
        telnetAutorisiert = true;         // Login erfolgreich
        telnetFehlversuche = 0;           // Fehlversuche zuruecksetzen
        HAL::telnetSchreiben("LOGIN OK"); // Bestaetigung
        logSerial("TLNT", "Login erfolgreich");  // Log
    }
    else if (eingabe.equalsIgnoreCase("up up down down left right left right b a")) {  // Easter Egg (Konami Code)
        HAL::telnetSchreiben("\n>> CHEAT CODE DETECTED <<");  // Easter Egg
        HAL::telnetSchreiben("   GOD MODE: [ACTIVATED]");     // Easter Egg
        HAL::telnetSchreiben("   UNLIMITED AMMO: [TRUE]");    // Easter Egg
    }
    else {                                // Falsches Passwort
        telnetFehlversuche++;             // Fehlversuche erhoehen
        if (telnetFehlversuche >= MAX_TELNET_VERSUCHE) {  // Zu viele Fehlversuche
            telnetSperreBis = HAL::zeitMs() + 300000;  // 5 Minuten Sperre
            HAL::telnetStoppen();         // Telnet-Server stoppen
            logSerial("TLNT", "GESPERRT fuer 5 Minuten (zu viele Fehlversuche)");  // Log
        } else {                          // Noch Versuche uebrig
            HAL::telnetSchreiben("Wrong PW");  // Fehler-Nachricht
        }
    }
}

// ============================================================================
// FINITE STATE MACHINE
// ============================================================================

// FSM: INIT-Zustand
void fsmInit() {
    HAL::init();                          // Hardware initialisieren (Serial.begin hier)
    Serial.println();
    Serial.println(F("========================================"));
    Serial.println(F("  Alarm-EMPFAENGER V15.0 | ESP8266 NodeMCU"));
    Serial.println(F("========================================"));
    logSerial("SYSTEM", "Hardware initialisiert");
    const bool flashBereit = HAL::flashInit();  // Dateisystem genau einmal pruefen
    // Verschleierte Defaults entschluesseln (einmalig beim Start)
    // Falls config.json existiert, werden diese sowieso ueberschrieben
    deobfuscate(config.apPasswort);       // AP-Passwort entschluesseln
    deobfuscate(config.telnetPasswort);   // Telnet-Passwort entschluesseln
    if (flashBereit) {
        ladeKonfiguration();              // Config aus Flash laden
        Security::ladeSequenz();          // Replay-Window laden
        Security::ladeApiZustand();       // Delivery-Journal laden
    } else {
        // Ein unlesbarer Persistenzspeicher ist kein frisches Geraet. Der
        // Empfaenger bleibt physisch im Alarm und sperrt alle Remote-Pfade.
        configStateCorrupt = true;
        Security::replayStateCorrupt = true;
        Security::persistierterAlarm = true;
        Security::apiStateCorrupt = true;
        HAL::alarmHardwareSetzen(true);
        logSerial("SEC", "LittleFS nicht verfuegbar: Fail-secure-Alarm aktiv");
    }
    sichereErststartSecrets();            // AP/Telnet nie mit bekanntem oder leerem Default starten
    alarmAktiv = Security::persistierterAlarm;
    if (alarmAktiv) {
        ledPhase = true;
        HAL::alarmHardwareSetzen(true);   // Fail-secure: letzter durable Zustand gilt schon beim Boot
    } else {
        HAL::alarmHardwareAus();
    }
    if (flashBereit && !wendeOffenesApiJournalAn())
        logSerial("SEC", "Delivery-Journal ungueltig oder Wirkung nicht wiederherstellbar");
    if (Security::replayStateCorrupt || Security::apiStateCorrupt || configStateCorrupt)
        logSerial("SEC", "Persistenter Sicherheitszustand defekt: Remote-Funktionen gesperrt");
    logSerial("SYSTEM", "Konfiguration und Replay-Window geladen");
    aktuellerZustand = ZUSTAND_WLAN_VERBINDEN;  // Naechster Zustand
    logSerial("SYSTEM", "FSM -> WLAN_VERBINDEN");
}

// FSM: WLAN_VERBINDEN-Zustand
void fsmWlanVerbinden() {
    WiFiManager wm;                       // WiFiManager-Objekt
    wm.setSaveConfigCallback(konfigurationSpeichernCallback);  // Callback registrieren

    // WiFiManager-Parameter definieren
    WiFiManagerParameter p_tpass("tpass", "Telnet PW", config.telnetPasswort, 20);  // Telnet-PW
    WiFiManagerParameter p_token("token", "UDP HMAC Secret", config.udpToken, 40);
    WiFiManagerParameter p_name("name", "mDNS Name", config.mdnsName, 32);  // mDNS-Name
    WiFiManagerParameter p_api("apiip", "API Server IP", config.apiServer, 32);  // API-Server
    WiFiManagerParameter p_bssid("bssid", "Backup SSID", config.backupSsid, 32);  // Backup-SSID
    WiFiManagerParameter p_bpass("bpass", "Backup PW", config.backupPasswort, 64);  // Backup-PW
    WiFiManagerParameter p_hssid("hssid", "Haupt SSID", config.hauptWlanName, 32);  // Haupt-SSID
    WiFiManagerParameter p_hpass("hpass", "Haupt PW", config.hauptWlanPasswort, 64);  // Haupt-PW
    WiFiManagerParameter p_appw("appw", "AP Passwort", config.apPasswort, 64);  // AP-Passwort
    WiFiManagerParameter p_apitoken("apitoken", "API Token", config.apiToken, 32);  // API-Token
    WiFiManagerParameter p_dhmac("dhmac", "Receiver Delivery HMAC", config.apiHmacToken, 40);

    // Parameter zum WiFiManager hinzufuegen
    wm.addParameter(&p_tpass);            // Telnet-PW
    wm.addParameter(&p_token);            // Token
    wm.addParameter(&p_name);             // mDNS-Name
    wm.addParameter(&p_api);              // API-Server
    wm.addParameter(&p_bssid);            // Backup-SSID
    wm.addParameter(&p_bpass);            // Backup-PW
    wm.addParameter(&p_hssid);            // Haupt-SSID
    wm.addParameter(&p_hpass);            // Haupt-PW
    wm.addParameter(&p_appw);             // AP-Passwort
    wm.addParameter(&p_apitoken);         // API-Token
    wm.addParameter(&p_dhmac);            // Geraetespezifischer Delivery-HMAC

    wm.setClass("invert");                // Dunkles Theme
    wm.setConfigPortalTimeout(180);       // Portal-Timeout 3min
    wm.setConnectTimeout(30);             // Verbindungs-Timeout 30s

    logSerial("WLAN", "Verbindungsaufbau gestartet...");

    // Hauptnetz direkt versuchen (WiFi.begin() ist asynchron – Wartezeit noetig!)
    // Bug-Fix: Ohne Wartezeit liefert wlanVerbunden() sofort false -> Portal oeffnet immer
    if (strlen(config.hauptWlanName) > 0) {  // Hauptnetz konfiguriert
        logSerial("WLAN", "Versuche Hauptnetz: " + String(config.hauptWlanName));
        HAL::wlanVerbinden(config.hauptWlanName, config.hauptWlanPasswort);
        int i = 0;
        while (i < 20 && !HAL::wlanVerbunden()) { delay(500); i++; Serial.print("."); }  // Max 10s warten
        Serial.println();                 // Zeilenumbruch nach Fortschrittspunkten
        if (HAL::wlanVerbunden())
            logSerial("WLAN", "Verbunden: " + HAL::wlanSsid() + " | " + HAL::wlanIp() + " | " + String(HAL::wlanRssi()) + " dBm");
    }

    // Captive Portal NUR bei Ersteinrichtung (keine gespeicherten WLAN-Credentials).
    // Mit vorhandenen Credentials kein Portal oeffnen: Reconnect laeuft automatisch
    // ueber verwalteWlanVerbindung() im BEREIT-Loop.
    if (!HAL::wlanVerbunden()) {
        if (strlen(config.hauptWlanName) == 0) {  // Keine Credentials -> Ersteinrichtung noetig
            logSerial("WLAN", "Ersteinrichtung: Setup-Portal wird geoeffnet...");
            Serial.print(F("Setup-AP Passwort (lokale serielle Ausgabe): "));
            Serial.println(config.apPasswort);
            wm.autoConnect("Alarm-Empfaenger-Konfig", config.apPasswort);
        } else {
            // Credentials bekannt: kein Portal, Reconnect erfolgt automatisch
            logSerial("WLAN", "Offline-Start, Reconnect automatisch via verwalteWlanVerbindung()");
        }
    }

    // Parameter aus WiFiManager in Config uebernehmen
    strlcpy(config.udpToken, p_token.getValue(), sizeof(config.udpToken));  // Token
    strlcpy(config.telnetPasswort, p_tpass.getValue(), sizeof(config.telnetPasswort));  // Telnet-PW
    strlcpy(config.mdnsName, p_name.getValue(), sizeof(config.mdnsName));  // mDNS-Name
    strlcpy(config.apiServer, p_api.getValue(), sizeof(config.apiServer));  // API-Server
    strlcpy(config.backupSsid, p_bssid.getValue(), sizeof(config.backupSsid));  // Backup-SSID
    strlcpy(config.backupPasswort, p_bpass.getValue(), sizeof(config.backupPasswort));  // Backup-PW
    strlcpy(config.hauptWlanName, p_hssid.getValue(), sizeof(config.hauptWlanName));  // Haupt-SSID
    strlcpy(config.hauptWlanPasswort, p_hpass.getValue(), sizeof(config.hauptWlanPasswort));  // Haupt-PW
    strlcpy(config.apPasswort, p_appw.getValue(), sizeof(config.apPasswort));  // AP-Passwort
    strlcpy(config.apiToken, p_apitoken.getValue(), sizeof(config.apiToken));  // API-Token
    strlcpy(config.apiHmacToken, p_dhmac.getValue(), sizeof(config.apiHmacToken));
    sichereErststartSecrets();            // Kurze/leere Portalwerte nicht in Betrieb nehmen

    if (strlen(config.udpToken) < 32)
        logSerial("SEC", "UDP-HMAC fehlt/zu kurz: UDP bleibt gesperrt");
    if (strlen(config.apiHmacToken) < 32)
        logSerial("SEC", "Receiver Delivery-HMAC fehlt/zu kurz: Remote-Control bleibt gesperrt");

    if (konfigurationSpeichern || HAL::wlanVerbunden())  // Config geaendert oder verbunden
        speichereKonfiguration();         // Config speichern

    // Dienste starten
    if (HAL::mdnsStarten(config.mdnsName))  // mDNS starten
        logSerial("SYSTEM", "mDNS gestartet: " + String(config.mdnsName));
#if ALARM_ENABLE_TELNET
    HAL::telnetStarten();                 // Nur im expliziten Debug-Build
#endif
    HAL::udpStarten();                    // UDP starten
    logSerial("SYSTEM", ALARM_ENABLE_TELNET ? "Dienste aktiv (UDP/Telnet/mDNS)" : "Dienste aktiv (UDP/mDNS; Telnet aus)");

    HAL::watchdogStarten();               // Watchdog starten
    logSerial("SYSTEM", "Watchdog aktiv - Loop beginnt");
    aktuellerZustand = alarmAktiv ? ZUSTAND_ALARM : ZUSTAND_BEREIT;
    logSerial("SYSTEM", alarmAktiv ? "FSM -> ALARM (persistiert)" : "FSM -> BEREIT");
}

// FSM: BEREIT-Zustand
void fsmBereit() {
    // Zeitkritisch: UDP-Empfang
    int udpResult = verarbeiteUdpEmpfang();  // UDP pruefen
    if (udpResult == 1) { alarmAktiv = true; aktuellerZustand = ZUSTAND_ALARM; return; }  // ALARM_ON
    if (udpResult == -1) { alarmAktiv = false; }  // ALARM_OFF

    // Taster
    int taste = verarbeiteTaster();       // Taster pruefen
    if (taste == 2) { aktuellerZustand = ZUSTAND_WERKSRESET; return; }  // Factory Reset
    if (taste == 1) {                     // Toggle
        if (!setzeAlarmPersistent(!alarmAktiv)) {
            logSerial("SEC", "Button-Toggle nicht persistierbar; Zustand unveraendert");
        } else {
            sendeProtokoll("Btn Toggle");
            if (alarmAktiv) return;
        }
    }

    // Alarm-Hardware aktualisieren (fuer den Fall: OFF-Zustand aufraeumen)
    aktualisiereAlarmHardware();          // Hardware aktualisieren

    // Nicht-zeitkritische Netzwerk-Tasks
    verarbeiteHeartbeat();                // Heartbeat verarbeiten
    pruefeTelnetZugang();                 // Telnet-Login pruefen
    verwalteWlanVerbindung();             // WLAN-Failover
    aktualisiereWlanLed();                // WLAN-LED aktualisieren

    if (HAL::wlanVerbunden())             // Verbunden
        HAL::mdnsUpdate();                // mDNS-Verarbeitung
}

// FSM: ALARM-Zustand
void fsmAlarm() {
    // PRIORITY MODE: Hardware-Steuerung hat Vorrang
    // Heartbeat laeuft weiter (fuer Web-Dashboard-Steuerung),
    // aber Telnet und WLAN-Scan bleiben pausiert.

    aktualisiereAlarmHardware();          // Alarm-Hardware aktualisieren

    int udpResult = verarbeiteUdpEmpfang();  // UDP pruefen
    if (udpResult == -1) {                // ALARM_OFF empfangen
        alarmAktiv = false;               // Alarm aus
        aktuellerZustand = ZUSTAND_BEREIT;  // Zurueck zu BEREIT
        return;                           // Funktion beenden
    }

    // Heartbeat: Ermoeglicht ALARM_OFF vom Web-Dashboard
    // Tradeoff: HTTP-Request kann ~1s blockieren (kurzes LED-Stottern)
    verarbeiteHeartbeat();                // Heartbeat verarbeiten

    // Alarm durch Server-Befehl deaktiviert?
    if (!alarmAktiv) {                    // Server hat Alarm deaktiviert
        aktuellerZustand = ZUSTAND_BEREIT;  // Zurueck zu BEREIT
        return;                           // Funktion beenden
    }

    int taste = verarbeiteTaster();       // Taster pruefen
    if (taste == 2) { aktuellerZustand = ZUSTAND_WERKSRESET; return; }  // Factory Reset
    if (taste == 1) {                     // Toggle
        if (setzeAlarmPersistent(false)) {
            sendeProtokoll("Btn Toggle");
            return;
        }
        logSerial("SEC", "Button-Toggle nicht persistierbar; Alarm bleibt aktiv");
    }

    // WLAN-LED auch im Alarm aktualisieren
    aktualisiereWlanLed();                // WLAN-LED aktualisieren
}

// FSM: WERKSRESET-Zustand
void fsmWerksreset() {
    sendeProtokoll("RESET!");             // Log
    HAL::watchdogStoppen();               // Watchdog aus
    HAL::flashFormatieren();              // Flash loeschen
    HAL::wlanCredentialsLoeschen();       // WLAN-Credentials loeschen
    HAL::neustart();                      // Neustart
}

// Zentraler FSM-Dispatcher
void fsmUpdate() {
    switch (aktuellerZustand) {           // Aktueller Zustand
        case ZUSTAND_INIT:           fsmInit();          break;  // INIT-Handler
        case ZUSTAND_WLAN_VERBINDEN: fsmWlanVerbinden(); break;  // WLAN-Handler
        case ZUSTAND_BEREIT:         fsmBereit();        break;  // BEREIT-Handler
        case ZUSTAND_ALARM:          fsmAlarm();         break;  // ALARM-Handler
        case ZUSTAND_WERKSRESET:     fsmWerksreset();    break;  // RESET-Handler
    }
}

// ============================================================================
// ARDUINO ENTRY POINTS
// ============================================================================

// Wird einmal beim Start ausgefuehrt
void setup() {
    aktuellerZustand = ZUSTAND_INIT;      // Start-Zustand
    fsmUpdate();                          // INIT -> WLAN_VERBINDEN
    fsmUpdate();                          // WLAN_VERBINDEN -> BEREIT
}

// Wird staendig ausgefuehrt
void loop() {
    HAL::watchdogFuettern();              // Watchdog fuettern
    if (HAL::watchdogAusgeloest()) {      // Watchdog ausgeloest
        Serial.println("WATCHDOG RESET!");  // Log
        HAL::neustart();                  // Neustart
    }
    fsmUpdate();                          // FSM aktualisieren
    HAL::cpuFreigeben();                  // CPU fuer WLAN freigeben
}

void generiereZufallsHex(char* ziel, size_t zielGroesse) {
    static const char HEX_ZEICHEN[] = "0123456789abcdef";
    if (zielGroesse < 2) return;
    for (size_t i = 0; i < zielGroesse - 1; i++) {
        if ((i & 7U) == 0) HAL::cpuFreigeben();
        ziel[i] = HEX_ZEICHEN[os_random() & 0x0F];
    }
    ziel[zielGroesse - 1] = '\0';
}

void sichereErststartSecrets() {
    bool geaendert = false;
    bool neuesApPasswort = false;
    bool neuesTelnetPasswort = false;
    if (strlen(config.apPasswort) < 12) {
        generiereZufallsHex(config.apPasswort, 17);
        geaendert = neuesApPasswort = true;
    }
    if (strlen(config.telnetPasswort) < 12) {
        generiereZufallsHex(config.telnetPasswort, 17);
        geaendert = neuesTelnetPasswort = true;
    }
    if (geaendert && !configStateCorrupt) speichereKonfiguration();
    if (neuesApPasswort) {
        Serial.print(F("SETUP AP PASSWORD (local serial only): "));
        Serial.println(config.apPasswort);
    }
    if (neuesTelnetPasswort) {
        Serial.print(F("TELNET PASSWORD (local serial only): "));
        Serial.println(config.telnetPasswort);
    }
}
