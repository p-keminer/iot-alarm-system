/*
 * PROJEKT: ESP8266 UDP Alarm-System (Sender)
 * ------------------------------------------
 * Architektur: Hardware Abstraction Layer (HAL) + Finite State Machine (FSM)
 *
 * Schichten-Modell:
 *   [FSM]      - Zustandslogik: INIT -> WLAN_VERBINDEN -> BEREIT <-> SENDEN -> WERKSRESET
 *   [Service]  - Sicherheit (HMAC, Sequenz), Protokoll, Heartbeat, UDP-Transaktion
 *   [HAL]      - Hardware-Abstraktion: GPIO, WiFi, UDP, Flash, Timer, Telnet, mDNS
 *
 * Security: HMAC-SHA256, Sequenznummer-Persistierung (LittleFS),
 *           Constant-Time ACK-Vergleich, Firmware String Obfuscation (Flash)
 *
 * Resilienz: UDP-Broadcast-Fallback (DNS-Ausfall), Active WiFi-Failback mit Hysterese,
 *            Remote-Konfigurations-Update & Remote-Wipe via API-Rueckkanal,
 *            Software-Watchdog, Remote-Logging / Audit Trail
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
#include <ESP8266WiFi.h>             // WLAN-Funktionen
#include <WiFiManager.h>             // Captive Portal fuer WLAN-Konfiguration
#include <WiFiUdp.h>                 // UDP-Kommunikation
#include <ESP8266mDNS.h>             // mDNS-Namensaufloesung (*.local)
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

const char* DEVICE_NAME = "sender";  // Geraete-Identifikation

// --- Timing ---
const unsigned long WLAN_WIEDERHOLUNGS_INTERVALL = 20000;  // WLAN-Reconnect alle 20s
const unsigned long WLAN_SCAN_INTERVALL = 30000;           // Netzwerk-Scan alle 30s
const uint8_t STABILITAETS_SCHWELLWERT = 3;                // Hauptnetz muss 3x gefunden werden
const int8_t RSSI_SCHWELLWERT = -75;                       // Minimale Signalstaerke in dBm
const unsigned long TASTER_RESET_DRUCK = 10000;            // 10s Tastendruck fuer Reset
const unsigned long BLINK_INTERVALL = 500;                 // LED-Blink-Frequenz
const int WATCHDOG_TIMEOUT_SEK = 30;                       // Loop haengt nach 30s
const unsigned long SENDE_WIEDERHOLUNGS_INTERVALL = 1000;  // Retry alle 1s
const int MAX_SENDE_VERSUCHE = 10;                         // Max 10 Sendeversuche
const unsigned long HEARTBEAT_INTERVALL = 2000;            // Telemetrie alle 2s
const unsigned long TELNET_TIMEOUT = 300000;               // Telnet-Logout nach 5min
const unsigned long IP_UPDATE_INTERVALL = 60000;           // mDNS-Aufloesung alle 60s

// --- Sicherheit ---
const uint8_t MAX_TELNET_VERSUCHE = 3;                     // Max 3 Login-Versuche

// --- Pins ---
#define PIN_RESET_TASTER D3            // Hardware-Reset-Taster
#define PIN_LED_ALARM LED_BUILTIN      // Alarm-LED (invertiert: LOW = an)
#define PIN_LED_WLAN D5                // WLAN-Status-LED

// --- Obfuscated Payloads (Muss mit Empfaenger uebereinstimmen!) ---
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
    char mdnsZiel[33] = "alarm-receiver"; // Empfaenger-Hostname (ohne .local)
    char apiServer[33] = "";            // API-Server IP-Adresse
    char telnetPasswort[21] = "";  // Telnet-Login (verschleiert)
    char backupSsid[33] = "";           // Fallback-WLAN
    char backupPasswort[65] = "";       // Fallback-WLAN-Passwort
    char hauptWlanName[33] = "";        // Primaeres WLAN
    char hauptWlanPasswort[65] = "";    // Primaeres WLAN-Passwort
    char apPasswort[65] = "";  // Access-Point-Passwort (verschleiert)
    char apiToken[33] = "";             // Geraetespezifischer Bearer-Token
    char apiHmacToken[41] = "";         // Geraetespezifischer Delivery-HMAC
};

// Explizit deklariert, damit der Arduino-Prototypgenerator den benutzerdefinierten
// Typ nicht vor seiner Definition referenziert.
bool leseKonfigurationDatei(const char* pfad, SystemKonfiguration& ziel);

// FSM-Zustaende
enum SystemZustand {
    ZUSTAND_INIT,            // Einmalige Initialisierung
    ZUSTAND_WLAN_VERBINDEN,  // Captive Portal oder Direktverbindung
    ZUSTAND_BEREIT,          // Normalbetrieb: Warte auf Befehl
    ZUSTAND_SENDEN,          // UDP-Transaktion: Sende + Retry bis ACK oder Timeout
    ZUSTAND_WERKSRESET       // Factory Reset
};

// ============================================================================
// HAL - HARDWARE ABSTRACTION LAYER
// ============================================================================
// Kapselt alle direkten Hardware-Zugriffe. Bei Portierung auf ESP32 oder
// andere Plattformen muss nur diese Schicht angepasst werden.

namespace HAL {

    // --- GPIO ---

    // Initialisiert alle GPIO-Pins
    void gpioInit() {
        pinMode(PIN_LED_ALARM, OUTPUT);            // Alarm-LED als Ausgang
        digitalWrite(PIN_LED_ALARM, HIGH);         // Invertiert: HIGH = aus
        pinMode(PIN_LED_WLAN, OUTPUT);             // WLAN-LED als Ausgang
        digitalWrite(PIN_LED_WLAN, LOW);           // LED aus
        pinMode(PIN_RESET_TASTER, INPUT_PULLUP);   // Taster mit Pull-Up
    }

    // Alarm-LED (invertiert: LOW = an, HIGH = aus)
    void alarmLed(bool an) {
        digitalWrite(PIN_LED_ALARM, an ? LOW : HIGH);  // Invertierte Logik
    }

    // WLAN-LED setzen
    void wlanLed(bool an) {
        digitalWrite(PIN_LED_WLAN, an ? HIGH : LOW);   // Normale Logik
    }

    // WLAN-LED umschalten (fuer Blink-Effekt)
    void wlanLedToggle() {
        digitalWrite(PIN_LED_WLAN, !digitalRead(PIN_LED_WLAN));  // Toggle
    }

    // Prueft ob Reset-Taster gedrueckt ist
    bool resetTasterGedrueckt() {
        return digitalRead(PIN_RESET_TASTER) == LOW;   // LOW = gedrueckt (Pull-Up)
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

    // Rate-Limited: Max alle 100ms (wie Original-Sender)
    void watchdogFuettern() {
        static unsigned long letzterHappen = 0;        // Letzte Fuetterung
        if (millis() - letzterHappen > 100) {          // Max alle 100ms
            watchdogZaehler = 0;                       // Zaehler zuruecksetzen
            letzterHappen = millis();                  // Zeitstempel merken
        }
    }

    // Prueft ob Watchdog ausgeloest wurde
    bool watchdogAusgeloest() {
        return mussNeustarten;  // Flag pruefen
    }

    // --- System ---

    // HAL-Initialisierung
    void init() {
        delay(1000);             // Warten bis Serial stabil
        Serial.begin(9600);      // Serielle Konsole starten
        Serial.setTimeout(1000); // Read-Timeout
        gpioInit();              // GPIO-Pins initialisieren
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
    const unsigned int LOKALER_PORT = 4211;   // Sender-Port
    const unsigned int ZIEL_PORT = 4210;      // Empfaenger-Port
    IPAddress zielIp;                    // Empfaenger-IP (aus mDNS)
    unsigned long letzteIpAktualisierung = 0;  // Timestamp letzte mDNS-Aufloesung

    // Startet UDP-Socket
    void udpStarten() { udpSocket.begin(LOKALER_PORT); }

    // Prueft ob UDP-Paket empfangen wurde
    int udpPaketVerfuegbar() { return udpSocket.parsePacket(); }

    // Liest UDP-Paket in Buffer
    int udpLesen(char* buf, size_t maxLen) { return udpSocket.read(buf, maxLen); }

    IPAddress udpAbsenderIp() { return udpSocket.remoteIP(); }
    unsigned int udpAbsenderPort() { return udpSocket.remotePort(); }

    bool udpAbsenderIstErwartet() {
        if (udpAbsenderPort() != ZIEL_PORT) return false;
        // Beim Broadcast-Fallback beweist zunächst die HMAC-Signatur die
        // Identität; anschließend wird genau diese IP als Peer gebunden.
        if (zielIp == IPAddress(255, 255, 255, 255)) return true;
        return udpAbsenderIp() == zielIp;
    }

    void udpPeerBinden() { zielIp = udpAbsenderIp(); }

    // Sendet UDP-Paket an Empfaenger
    void udpSenden(const char* daten) {
        udpSocket.beginPacket(zielIp, ZIEL_PORT);  // Paket starten
        udpSocket.print(daten);                    // Daten schreiben
        udpSocket.endPacket();                     // Paket absenden
    }

    // mDNS-Aufloesung der Empfaenger-IP (Rate-Limited)
    void zielIpAktualisieren(const char* zielHostname) {
        if (zeitMs() - letzteIpAktualisierung < IP_UPDATE_INTERVALL &&  // Noch nicht abgelaufen
            zielIp.toString() != "0.0.0.0") return;  // IP schon gueltig
        WiFi.hostByName((String(zielHostname) + ".local").c_str(), zielIp);  // mDNS-Aufloesung
        letzteIpAktualisierung = zeitMs();           // Timestamp aktualisieren
        if (zielIp.toString() == "0.0.0.0")          // Aufloesung fehlgeschlagen
            zielIp = IPAddress(255, 255, 255, 255);  // Broadcast-Fallback
    }

    // Prueft ob gueltige Ziel-IP vorhanden ist
    bool zielIpGueltig() { return zielIp.toString() != "0.0.0.0"; }

    // Invalidiert IP-Cache: erzwingt neue mDNS-Aufloesung beim naechsten zielIpAktualisieren()-Aufruf.
    // Wichtig beim Netzwechsel: verhindert, dass veraltete Haupt-Netz-IP weiter benutzt wird.
    void zielIpZuruecksetzen() {
        zielIp                 = IPAddress(0, 0, 0, 0);  // IP als ungueltig markieren
        letzteIpAktualisierung = 0;                      // Rate-Limit aufheben -> Neuaufloesung erzwingen
    }

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

    // --- Seriell ---

    bool seriellVerfuegbar() { return Serial.available(); }  // Daten im Buffer

    // Liest Zeile von serieller Schnittstelle
    size_t seriellLesen(char* buf, size_t maxLen) {
        size_t len = Serial.readBytesUntil('\n', buf, maxLen - 1);  // Bis Newline lesen
        buf[len] = '\0';                                             // Null-Terminierung
        while (len > 0 && (buf[len-1] == '\r' || buf[len-1] == ' '))  // Whitespace entfernen
            buf[--len] = '\0';                                       // Zeichen loeschen
        return len;  // Tatsaechliche Laenge
    }

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

    // --- Sequenznummer (LittleFS-persistent, ueberlebt Reboot) ---

    unsigned long sequenceNumber = 0;  // Aktuelle Sequenznummer
    bool sequenceStateCorrupt = false;

    // Speichert Sequenznummer im Flash
    bool speichereSequenz() {
        File f = LittleFS.open("/seq.tmp", "w");  // Erst temporaer schreiben
        if (!f) return false;
        char buf[12];
        snprintf(buf, sizeof(buf), "%lu", sequenceNumber);
        if (f.print(buf) == 0) { f.close(); LittleFS.remove("/seq.tmp"); return false; }
        f.close();
        LittleFS.remove("/seq.dat");
        return LittleFS.rename("/seq.tmp", "/seq.dat");
    }

    bool leseSequenzDatei(const char* pfad, unsigned long& wert) {
        if (!LittleFS.exists(pfad)) return false;
        File f = LittleFS.open(pfad, "r");
        if (!f) return false;
        char buf[12];
        size_t len = f.readBytes(buf, sizeof(buf) - 1);
        buf[len] = '\0';
        f.close();
        if (!nurZiffern(buf)) return false;
        wert = strtoul(buf, NULL, 10);
        return true;
    }

    // Laedt auch eine nach Stromverlust liegengebliebene, bereits vollstaendig
    // geschriebene Temp-Datei und nimmt immer die hoechste Sequenz.
    void ladeSequenz() {
        bool finalVorhanden = LittleFS.exists("/seq.dat");
        bool tempVorhanden = LittleFS.exists("/seq.tmp");
        unsigned long finalWert = 0, tempWert = 0;
        bool finalOk = leseSequenzDatei("/seq.dat", finalWert);
        bool tempOk = leseSequenzDatei("/seq.tmp", tempWert);
        if (!finalOk && !tempOk) {
            if (finalVorhanden || tempVorhanden || LittleFS.exists("/config.json") ||
                LittleFS.exists("/config.tmp")) {
                sequenceStateCorrupt = true;
            } else if (!speichereSequenz()) {
                sequenceStateCorrupt = true;
            }
            return;
        }
        sequenceNumber = finalOk ? finalWert : tempWert;
        if (tempOk && tempWert > sequenceNumber) sequenceNumber = tempWert;
        if (tempOk && !speichereSequenz()) sequenceStateCorrupt = true;
    }

    // --- Signierte HTTP-Delivery: durables Write-Ahead-Apply-Journal ---
    // Eine Delivery wird zuerst als pending persistiert. Erst nach bestaetigter
    // Wirkung wird sequence/last/ack atomar committed. Ein Reset zwischen den
    // Phasen fuehrt deshalb beim Boot zu einer idempotenten Wiederholung.
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
            // Nur ein wirklich unprovisioniertes Geraet darf einen neuen
            // High-Watermark anlegen. Nach Provisionierung bedeutet ein
            // fehlendes Journal Metadatenverlust und wird gesperrt.
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
// Kategorien (6 Zeichen, linksbuendig): SYSTEM|WLAN|SCAN|SEC|HB|UDP|TLNT|PROTO
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

// UDP-Transaktion (SENDEN-Zustand)
char ausstehendeNachricht[256] = "";  // Aktuelle UDP-Nachricht
int wiederholungsZaehler = 0;         // Anzahl Sendeversuche
unsigned long letzterSendeZeitpunkt = 0;  // Zeitpunkt letztes Senden
bool letzterBefehlWarAlarmAn = false;  // Fuer LED-Feedback nach ACK

// Timing
unsigned long letzterVerbindungsVersuch = 0;  // WLAN-Reconnect-Timestamp
unsigned long letzterScanStart = 0;           // Netzwerk-Scan-Timestamp
unsigned long letzterHeartbeat = 0;           // Letzter Heartbeat
unsigned long letzterTelnetInput = 0;         // Letzter Telnet-Input (Auto-Logout)
unsigned long letztesBlinken = 0;             // LED-Blink-Timestamp
unsigned long aktuellesHeartbeatIntervall = 2000;  // Dynamisches Heartbeat-Intervall
uint8_t hbFehlversuche = 0;                       // Aufeinanderfolgende HB-Fehler (TCP-Stack-Bug-Detektion)
int scanStatus = -1;                          // Async-Scan-Status

// Telnet Sicherheit
uint8_t telnetFehlversuche = 0;       // Login-Fehlversuche
unsigned long telnetSperreBis = 0;    // Timestamp Sperrende

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
    strlcpy(ziel.mdnsZiel, doc["ziel"] | "", sizeof(ziel.mdnsZiel));
    strlcpy(ziel.telnetPasswort, doc["tpass"] | "", sizeof(ziel.telnetPasswort));
    strlcpy(ziel.backupSsid, doc["bssid"] | "", sizeof(ziel.backupSsid));
    strlcpy(ziel.backupPasswort, doc["bpass"] | "", sizeof(ziel.backupPasswort));
    strlcpy(ziel.hauptWlanName, doc["hssid"] | "", sizeof(ziel.hauptWlanName));
    strlcpy(ziel.hauptWlanPasswort, doc["hpass"] | "", sizeof(ziel.hauptWlanPasswort));
    strlcpy(ziel.apPasswort, doc["appw"] | "", sizeof(ziel.apPasswort));
    strlcpy(ziel.apiServer, doc["api"] | "", sizeof(ziel.apiServer));
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
    doc["ziel"] = config.mdnsZiel;    // Ziel schreiben
    doc["tpass"] = config.telnetPasswort;  // Telnet-PW schreiben
    doc["bssid"] = config.backupSsid;      // Backup-SSID
    doc["bpass"] = config.backupPasswort;  // Backup-PW
    doc["hssid"] = config.hauptWlanName;   // Haupt-SSID
    doc["hpass"] = config.hauptWlanPasswort;  // Haupt-PW
    doc["api"] = config.apiServer;         // API-Server
    doc["appw"]  = config.apPasswort;      // AP-Passwort
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

bool wendeOffenesApiJournalAn() {
    if (Security::apiStateCorrupt) return false;
    if (!Security::apiApplyPending) return true;
    bool wirkungBestaetigt = false;
    if (strcmp(Security::apiApplyType, "config") == 0) {
        wirkungBestaetigt = wendeRemoteKonfigurationAn(Security::apiApplyPayload);
    } else if (strcmp(Security::apiApplyType, "command") == 0 &&
               strcmp(Security::apiApplyPayload, "REBOOT") == 0) {
        // Das Vorhandensein des Journals beim Boot bestaetigt die Reboot-Wirkung.
        wirkungBestaetigt = true;
    }
    return wirkungBestaetigt && Security::completeApiApply();
}

// ============================================================================
// LOGGING & PROTOKOLL
// ============================================================================

// Sendet Log-Nachricht an API-Server
void sendeLogAnApi(const char* nachricht) {
    if (!darfLoggen || !HAL::wlanVerbunden() || strlen(config.apiServer) == 0 || strlen(config.apiToken) < 16) return;

    WiFiClient client;                    // HTTP-Client
    client.setTimeout(2000);              // Timeout 2s
    HTTPClient http;                      // HTTP-Client-Wrapper
    http.setTimeout(2000);                // Timeout 2s
    String serverPath = "http://" + String(config.apiServer) + "/api.php";  // API-URL
    if (http.begin(client, serverPath)) { // HTTP-Verbindung oeffnen
        http.addHeader("Content-Type", "application/json");  // Content-Type setzen
        if (strlen(config.apiToken) > 0) {  // Token vorhanden
            http.addHeader("Authorization", String("Bearer ") + config.apiToken);  // Bearer-Token
            http.addHeader("X-ESP-Token", config.apiToken);  // Custom-Header
        }
        StaticJsonDocument<256> doc;      // JSON-Dokument
        doc["source"] = DEVICE_NAME;      // Absender
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

// --- Signiertes UDP-Paket erstellen und senden ---
void starteUdpTransaktion(const char* klarBefehl) {
    if (Security::sequenceStateCorrupt || configStateCorrupt) {
        logSerial("SEC", "UDP blockiert: persistenter Zustand ungueltig");
        return;
    }
    if (strcmp(klarBefehl, "ALARM_ON") != 0 && strcmp(klarBefehl, "ALARM_OFF") != 0) return;
    if (strlen(config.udpToken) < 32) {
        logSerial("SEC", "UDP blockiert: HMAC-Secret fehlt/ist zu kurz");
        return;
    }
    if (Security::sequenceNumber == 0xFFFFFFFFUL) {
        logSerial("SEC", "UDP blockiert: Sequenzraum erschoepft; neu provisionieren");
        return;
    }
    Security::sequenceNumber++;           // Sequenznummer erhoehen
    // Vor dem Senden persistieren: Nach einem Reset darf dieselbe Sequenz nie
    // erneut für einen anderen Befehl verwendet werden.
    if (!Security::speichereSequenz()) {
        Security::sequenceNumber--;
        logSerial("SEC", "UDP blockiert: Sequenz konnte nicht persistiert werden");
        return;
    }

    const char* echterBefehl = (strcmp(klarBefehl, "ALARM_ON") == 0) ? CMD_ALARM_AN : CMD_ALARM_AUS;  // Obfuscated-Befehl
    letzterBefehlWarAlarmAn = (strcmp(klarBefehl, "ALARM_ON") == 0);  // Merken fuer LED

    // Payload: "OBFUSCATED_CMD:SEQ"
    char payload[128];                    // Payload-Buffer
    int pLen = snprintf(payload, sizeof(payload), "%s:%lu", echterBefehl, Security::sequenceNumber);  // Payload bauen
    if (pLen < 0 || (size_t)pLen >= sizeof(payload)) return;  // Overflow-Check

    // HMAC berechnen
    char signatur[65];                    // HMAC-Buffer (64 Hex + \0)
    Security::berechneHMAC(payload, (size_t)pLen, config.udpToken, strlen(config.udpToken), signatur);  // HMAC-SHA256

    // Komplett: "OBFUSCATED_CMD:SEQ:HMAC"
    snprintf(ausstehendeNachricht, sizeof(ausstehendeNachricht), "%s:%s", payload, signatur);  // Finale Nachricht

    wiederholungsZaehler = 0;             // Retry-Zaehler zuruecksetzen
    letzterSendeZeitpunkt = HAL::zeitMs();  // Timestamp merken

    HAL::zielIpAktualisieren(config.mdnsZiel);  // IP aktualisieren
    HAL::udpSenden(ausstehendeNachricht); // Erstes Senden
    logSerial("UDP", "Sende: " + String(klarBefehl) + " | Seq: " + String(Security::sequenceNumber));  // Log

    aktuellerZustand = ZUSTAND_SENDEN;    // In Sende-Zustand wechseln
}

// --- UDP-ACK pruefen (SENDEN-Zustand) ---
// Rueckgabe: 1 = ACK empfangen, 0 = noch wartend, -1 = Timeout
int pruefeUdpAntwort() {
    int paketGroesse = HAL::udpPaketVerfuegbar();  // Paket empfangen?

    if (paketGroesse) {                   // Paket vorhanden
        char puffer[255];                 // Buffer fuer Paket
        int laenge = HAL::udpLesen(puffer, sizeof(puffer) - 1);  // Paket lesen
        if (laenge <= 0) return 0;        // Lese-Fehler
        puffer[laenge] = '\0';            // Null-Terminierung

        // Whitespace trimmen
        while (laenge > 0 && (puffer[laenge-1] == '\n' || puffer[laenge-1] == '\r' || puffer[laenge-1] == ' '))  // Whitespace am Ende
            puffer[--laenge] = '\0';      // Zeichen entfernen

        if (!HAL::udpAbsenderIstErwartet() || strlen(config.udpToken) < 32) return 0;

        // Erwartet: ACK_SECURE:<SEQ>:<HMAC(ACK_SECURE:<SEQ>)>
        char arbeitskopie[255];
        strlcpy(arbeitskopie, puffer, sizeof(arbeitskopie));
        char* art = strtok(arbeitskopie, ":");
        char* seq = strtok(NULL, ":");
        char* sig = strtok(NULL, ":");
        if (!art || !seq || !sig || strtok(NULL, ":") != NULL) return 0;
        if (strcmp(art, "ACK_SECURE") != 0 || !Security::nurZiffern(seq) || !Security::istHex(sig, 64)) return 0;
        if (strtoul(seq, NULL, 10) != Security::sequenceNumber) return 0;

        char material[40];
        int materialLen = snprintf(material, sizeof(material), "ACK_SECURE:%s", seq);
        if (materialLen < 0 || (size_t)materialLen >= sizeof(material)) return 0;
        char erwartet[65];
        Security::berechneHMAC(material, (size_t)materialLen, config.udpToken, strlen(config.udpToken), erwartet);
        char sigLower[65];
        for (size_t i = 0; i < 64; i++) sigLower[i] = (char)tolower((unsigned char)sig[i]);
        sigLower[64] = '\0';
        if (Security::sichererVergleich(erwartet, sigLower, 64)) {
            HAL::udpPeerBinden();
            return 1;
        }
    }

    // Retry-Logik
    if (wiederholungsZaehler < MAX_SENDE_VERSUCHE &&  // Noch Versuche uebrig
        HAL::zeitMs() - letzterSendeZeitpunkt >= SENDE_WIEDERHOLUNGS_INTERVALL) {  // Intervall abgelaufen
        wiederholungsZaehler++;           // Zaehler erhoehen
        char msg[48];                     // Log-Nachricht
        snprintf(msg, sizeof(msg), "Wiederholung %d", wiederholungsZaehler);  // Log-Nachricht
        sendeProtokoll(msg);              // Protokollieren

        if (HAL::zielIpGueltig())         // IP vorhanden
            HAL::udpSenden(ausstehendeNachricht);  // Erneut senden
        letzterSendeZeitpunkt = HAL::zeitMs();  // Timestamp aktualisieren
    }

    if (wiederholungsZaehler >= MAX_SENDE_VERSUCHE)  // Alle Versuche aufgebraucht
        return -1;                        // Timeout

    return 0;                             // Noch wartend
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

    String material = "ALARMv2\nsender\n" + id + "\n" + String(sequence) + "\n" + typ + "\n" + nonce + "\n" + payload;
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
        String material = "ALARMv2ENC\nsender\n" + id + "\n" + nonce + "\n" + String(block);
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
    String material = "ALARMv2ACK\nsender\n" + String(id);
    Security::berechneHMAC(material.c_str(), material.length(), config.apiHmacToken, strlen(config.apiHmacToken), sig);
}

// --- Heartbeat (nur im BEREIT-Zustand) ---
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
    client.setTimeout(2000);              // Timeout 2s
    HTTPClient http;                      // HTTP-Client-Wrapper
    http.setTimeout(2000);                // Timeout 2s
    String serverPath = "http://" + String(config.apiServer) + "/api.php";  // API-URL
    bool neustartNachAntwort = false;

    if (http.begin(client, serverPath)) { // HTTP-Verbindung oeffnen
        logSerial("HB", "-> " + String(config.apiServer) + " | Heap: " + String(HAL::freierHeap()));  // Log
        http.addHeader("Content-Type", "application/json");  // Content-Type setzen
        if (strlen(config.apiToken) > 0) {  // Token vorhanden
            http.addHeader("Authorization", String("Bearer ") + config.apiToken);  // Bearer-Token
            http.addHeader("X-ESP-Token", config.apiToken);  // Custom-Header
        }
        StaticJsonDocument<512> doc;      // JSON-Dokument (groesser wegen Telemetrie)
        doc["source"] = DEVICE_NAME;      // Absender
        bool ackGesendet = Security::pendingDeliveryAck[0] != '\0';
        if (ackGesendet) {
            doc["delivery_ack"] = Security::pendingDeliveryAck;
            char ackSig[65];
            berechneDeliveryAckSignatur(Security::pendingDeliveryAck, ackSig);
            doc["delivery_ack_sig"] = ackSig;
        }
        doc["ip"] = HAL::wlanIp();        // IP-Adresse
        doc["status_msg"] = (aktuellerZustand == ZUSTAND_SENDEN) ? "Sende Cmd..." : "Bereit";  // Status
        doc["rssi"] = HAL::wlanRssi();    // Signalstaerke
        doc["heap"] = HAL::freierHeap();  // Freier RAM
        doc["uptime"] = millis() / 1000;  // Uptime in Sekunden
        doc["reset_reason"] = HAL::resetGrund();  // Reset-Grund
        if (ausstehendeNachricht[0] != '\0')
            doc["log"] = "UDP command pending"; // Keine signierte Payload in Server-Logs
        String body;                      // JSON-String
        serializeJson(doc, body);         // JSON serialisieren
        int code = http.POST(body);       // POST-Request senden

        if (code > 0) {                   // Antwort erhalten
            aktuellesHeartbeatIntervall = HEARTBEAT_INTERVALL;  // Normales Intervall
            hbFehlversuche = 0;           // TCP-Stack offensichtlich gesund

           if (code == 200) {             // Erfolgreiche Antwort
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
                                // Erst durable processed+pending-ACK; Reboot folgt nach geschlossener HTTP-Antwort.
                                neustartNachAntwort = true;
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
                letzterVerbindungsVersuch = HAL::zeitMs();  // Reconnect-Cooldown setzen
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

// --- Serielle Befehle ---
// Rueckgabe: true wenn Transaktion gestartet wurde
bool verarbeiteSerielleBefehle() {
    if (!HAL::seriellVerfuegbar()) return false;  // Keine Daten

    char befehl[32];                      // Buffer fuer Befehl
    HAL::seriellLesen(befehl, sizeof(befehl));  // Zeile lesen

    if (strcmp(befehl, "ALARM_ON") == 0 || strcmp(befehl, "ALARM_OFF") == 0) {  // Gueltiger Befehl
        starteUdpTransaktion(befehl);     // UDP-Transaktion starten
        return true;                      // Transaktion gestartet
    }
    return false;                         // Kein gueltiger Befehl
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
    static int stabilitaetsZaehler = 0;       // Zaehlt stabile Hauptnetz-Scans
    static uint8_t hauptnetz_fehlversuche = 0; // Zaehlt Reconnect-Fehlversuche beim Hauptnetz
    const  uint8_t MAX_HAUPTNETZ_VERSUCHE  = 2; // Max 2x Hauptnetz probieren, dann Backup
    static bool udp_auf_backup_neugestartet = false;  // UDP einmalig nach Backup-Switch neu binden

    // Fix: Non-blocking Rueckwechsel zum Hauptnetz.
    // Problem vorher: while(!verbunden, max 15s) blockierte den loop() komplett -> Receiver war
    // in dieser Zeit taub fuer UDP, ACKs gingen verloren -> Alarmsignal ankam mit 10s Verzoegerung.
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
            HAL::mdnsStarten(DEVICE_NAME);                         // mDNS neu initialisieren
            HAL::zielIpZuruecksetzen();                            // Backup-IP aus Cache entfernen
            HAL::zielIpAktualisieren(config.mdnsZiel);             // Empfaenger-IP neu aufloesen
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
            // Fix: UDP-Socket + IP-Cache einmalig nach Backup-Switch erneuern.
            // Ohne zielIpZuruecksetzen() wuerde zielIpAktualisieren() die noch gueltige Haupt-Netz-IP
            // weiterverwenden (Rate-Limit 60s) -> alle UDP-Pakete gehen ins falsche Subnetz.
            // Schlaegt mDNS-Aufloesung fehl, greift Broadcast-Fallback (255.255.255.255).
            if (!udp_auf_backup_neugestartet) {
                HAL::udpStarten();                          // UDP an neue Backup-IP binden
                HAL::zielIpZuruecksetzen();                 // Veraltete Haupt-IP aus Cache entfernen
                HAL::zielIpAktualisieren(config.mdnsZiel);  // Receiver-IP aufloesen (Fehler -> Broadcast)
                logSerial("WLAN", "Backup aktiv | " + HAL::wlanIp() + " | UDP-IP-Cache erneuert");
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
    else if (eingabe.equalsIgnoreCase("up up down down left right left right b a")) {  // Easter Egg
        HAL::telnetSchreiben("\n>> CHEAT CODE DETECTED <<");  // Easter Egg
        HAL::telnetSchreiben("   GOD MODE: [FAKE ENABLED]");  // Easter Egg
    }
    else if (eingabe == "logout") {       // Logout-Befehl
        telnetAutorisiert = false;        // Logout
        HAL::telnetSchreiben("Ausgeloggt.");  // Bestaetigung
    }
    else {                                // Falsches Passwort
        telnetFehlversuche++;             // Fehlversuche erhoehen
        if (telnetFehlversuche >= MAX_TELNET_VERSUCHE) {  // Zu viele Fehlversuche
            telnetSperreBis = HAL::zeitMs() + 300000;  // 5 Minuten Sperre
            HAL::telnetStoppen();         // Telnet-Server stoppen
            logSerial("TLNT", "GESPERRT fuer 5 Minuten (zu viele Fehlversuche)");  // Log
        } else {                          // Noch Versuche uebrig
            HAL::telnetSchreiben("Falsches PW");  // Fehler-Nachricht
        }
    }
}

// --- Hardware-Reset-Taster ---
// Rueckgabe: true wenn Reset ausgeloest wurde
bool pruefePhysischenReset() {
    static unsigned long druckStart = 0;  // Timestamp Druck-Beginn

    if (HAL::resetTasterGedrueckt()) {    // Taster gedrueckt
        if (druckStart == 0) druckStart = HAL::zeitMs();  // Timestamp merken
        // Visuelles Feedback: LED blinkt waehrend gedrueckt
        if (HAL::zeitMs() % 200 < 100) HAL::alarmLed(true);   // LED an
        else HAL::alarmLed(false);        // LED aus
        return false;                     // Noch nicht ausgeloest
    }

    if (druckStart == 0) return false;    // War nicht gedrueckt

    unsigned long dauer = HAL::zeitMs() - druckStart;  // Druck-Dauer
    HAL::alarmLed(false);                 // LED aus
    druckStart = 0;                       // Timestamp zuruecksetzen

    if (dauer > TASTER_RESET_DRUCK) return true;  // Lang genug gedrueckt
    return false;                         // Zu kurz
}

// ============================================================================
// FINITE STATE MACHINE
// ============================================================================

// FSM: INIT-Zustand
void fsmInit() {
    HAL::init();                          // Hardware initialisieren (Serial.begin hier)
    Serial.println();
    Serial.println(F("========================================"));
    Serial.println(F("  Alarm-SENDER V15.0 | ESP8266 NodeMCU  "));
    Serial.println(F("========================================"));
    logSerial("SYSTEM", "Hardware initialisiert");
    const bool flashBereit = HAL::flashInit();  // Dateisystem genau einmal pruefen
    // Verschleierte Defaults entschluesseln (einmalig beim Start)
    // Falls config.json existiert, werden diese sowieso ueberschrieben
    deobfuscate(config.apPasswort);       // AP-Passwort entschluesseln
    deobfuscate(config.telnetPasswort);   // Telnet-Passwort entschluesseln
    if (flashBereit) {
        ladeKonfiguration();              // Config aus Flash laden
        Security::ladeSequenz();          // Sequenznummer laden
        Security::ladeApiZustand();       // Delivery-Journal laden
    } else {
        // Ohne persistente Sequenz- und Delivery-Zustaende darf der Sender
        // weder Pakete senden noch Remote-Aktionen als frisch behandeln.
        configStateCorrupt = true;
        Security::sequenceStateCorrupt = true;
        Security::apiStateCorrupt = true;
        logSerial("SEC", "LittleFS nicht verfuegbar: Sicherheitsfunktionen gesperrt");
    }
    sichereErststartSecrets();            // AP/Telnet nie mit bekanntem oder leerem Default starten
    if (flashBereit && !wendeOffenesApiJournalAn())
        logSerial("SEC", "Delivery-Journal ungueltig oder Wirkung nicht wiederherstellbar");
    if (Security::sequenceStateCorrupt || Security::apiStateCorrupt || configStateCorrupt)
        logSerial("SEC", "Persistenter Sicherheitszustand defekt: Remote-Funktionen gesperrt");
    logSerial("SYSTEM", "Konfiguration und Sequenz geladen");
    aktuellerZustand = ZUSTAND_WLAN_VERBINDEN;  // Naechster Zustand
    logSerial("SYSTEM", "FSM -> WLAN_VERBINDEN");
}

// FSM: WLAN_VERBINDEN-Zustand
void fsmWlanVerbinden() {
    WiFiManager wm;                       // WiFiManager-Objekt
    wm.setSaveConfigCallback(konfigurationSpeichernCallback);  // Callback registrieren

    // WiFiManager-Parameter definieren
    WiFiManagerParameter p_api("api", "API Server IP", config.apiServer, 32);  // API-Server
    WiFiManagerParameter p_token("token", "UDP HMAC Secret", config.udpToken, 40);
    WiFiManagerParameter p_tpass("tpass", "Telnet PW", config.telnetPasswort, 20);  // Telnet-PW
    WiFiManagerParameter p_ziel("ziel", "Empfaenger mDNS", config.mdnsZiel, 32);  // Empfaenger
    WiFiManagerParameter p_bssid("bssid", "Backup SSID", config.backupSsid, 32);  // Backup-SSID
    WiFiManagerParameter p_bpass("bpass", "Backup PW", config.backupPasswort, 64);  // Backup-PW
    WiFiManagerParameter p_hssid("hssid", "Haupt SSID", config.hauptWlanName, 32);  // Haupt-SSID
    WiFiManagerParameter p_hpass("hpass", "Haupt PW", config.hauptWlanPasswort, 64);  // Haupt-PW
    WiFiManagerParameter p_appw("appw", "AP Passwort", config.apPasswort, 64);  // AP-Passwort
    WiFiManagerParameter p_apitoken("apitoken", "API Token", config.apiToken, 32);  // API-Token
    WiFiManagerParameter p_dhmac("dhmac", "Sender Delivery HMAC", config.apiHmacToken, 40);

    // Parameter zum WiFiManager hinzufuegen
    wm.addParameter(&p_api);              // API-Server
    wm.addParameter(&p_token);            // Token
    wm.addParameter(&p_tpass);            // Telnet-PW
    wm.addParameter(&p_ziel);             // Empfaenger
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

    // Hauptnetz direkt versuchen wenn konfiguriert
    if (strlen(config.hauptWlanName) > 0 && strlen(config.hauptWlanPasswort) > 0) {  // Hauptnetz konfiguriert
        logSerial("WLAN", "Versuche Hauptnetz: " + String(config.hauptWlanName));
        HAL::wlanVerbinden(config.hauptWlanName, config.hauptWlanPasswort);  // Verbinden
        int i = 0;                        // Zaehler
        while (i < 20 && !HAL::wlanVerbunden()) { delay(500); i++; Serial.print("."); }  // Warten
        Serial.println();                 // Zeilenumbruch nach Fortschrittspunkten
        if (HAL::wlanVerbunden())
            logSerial("WLAN", "Verbunden: " + HAL::wlanSsid() + " | " + HAL::wlanIp() + " | " + String(HAL::wlanRssi()) + " dBm");
    }

    // Captive Portal NUR bei Ersteinrichtung (keine gespeicherten WLAN-Credentials).
    // Mit vorhandenen Credentials kein Portal oeffnen: Reconnect laeuft automatisch
    // ueber verwalteWlanVerbindung() im BEREIT-Loop. So wird der WiFiManager-Neustart-Bug
    // vermieden (Router noch nicht erreichbar nach Stromausfall -> kein falsches Portal).
    if (!HAL::wlanVerbunden()) {
        if (strlen(config.hauptWlanName) == 0) {  // Keine Credentials -> Ersteinrichtung noetig
            logSerial("WLAN", "Ersteinrichtung: Setup-Portal wird geoeffnet...");
            Serial.print(F("Setup-AP Passwort (lokale serielle Ausgabe): "));
            Serial.println(config.apPasswort);
            bool erfolg = wm.autoConnect("Alarm-Sender-Konfig", config.apPasswort);
            if (!erfolg) logSerial("WLAN", "Setup-Portal: kein Erfolg, Offline-Start");
        } else {
            // Credentials bekannt: kein Portal, Reconnect erfolgt automatisch
            logSerial("WLAN", "Offline-Start, Reconnect automatisch via verwalteWlanVerbindung()");
        }
    }

    // Parameter aus WiFiManager in Config uebernehmen
    strlcpy(config.apiServer, p_api.getValue(), sizeof(config.apiServer));  // API-Server
    strlcpy(config.udpToken, p_token.getValue(), sizeof(config.udpToken));  // Token
    strlcpy(config.telnetPasswort, p_tpass.getValue(), sizeof(config.telnetPasswort));  // Telnet-PW
    strlcpy(config.mdnsZiel, p_ziel.getValue(), sizeof(config.mdnsZiel));  // Empfaenger
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
        logSerial("SEC", "Sender Delivery-HMAC fehlt/zu kurz: Remote-Control bleibt gesperrt");

    if (konfigurationSpeichern || HAL::wlanVerbunden())  // Config geaendert oder verbunden
        speichereKonfiguration();         // Config speichern

    // Dienste starten
    if (HAL::mdnsStarten(DEVICE_NAME))    // mDNS starten
        logSerial("SYSTEM", "mDNS gestartet: " + String(DEVICE_NAME));
#if ALARM_ENABLE_TELNET
    HAL::telnetStarten();                 // Nur im expliziten Debug-Build
#endif
    HAL::udpStarten();                    // UDP starten
    HAL::zielIpAktualisieren(config.mdnsZiel);  // Empfaenger-IP aufloesen
    logSerial("SYSTEM", ALARM_ENABLE_TELNET ? "Dienste aktiv (UDP/Telnet/mDNS)" : "Dienste aktiv (UDP/mDNS; Telnet aus)");

    if (HAL::wlanVerbunden()) {           // Verbunden
        darfLoggen = true;                // Logging erlauben
        letzterHeartbeat = 0;             // Heartbeat sofort senden
        verarbeiteHeartbeat();            // Ersten Heartbeat senden
        sendeLogAnApi("System erfolgreich gestartet!");  // Startup-Log
    }

    HAL::watchdogStarten();               // Watchdog starten
    logSerial("SYSTEM", "Watchdog aktiv - Loop beginnt");
    aktuellerZustand = ZUSTAND_BEREIT;    // Naechster Zustand
    logSerial("SYSTEM", "FSM -> BEREIT");
}

// FSM: BEREIT-Zustand
void fsmBereit() {
    // Serielle Befehle -> koennen Transaktion starten
    if (verarbeiteSerielleBefehle()) return;  // Zustand schon auf SENDEN

    // Reset-Taster pruefen
    if (pruefePhysischenReset()) {        // Reset ausgeloest
        aktuellerZustand = ZUSTAND_WERKSRESET;  // In Reset-Zustand wechseln
        return;                           // Funktion beenden
    }

    // Nicht-zeitkritische Tasks
    HAL::zielIpAktualisieren(config.mdnsZiel);  // mDNS-IP aktualisieren
    verarbeiteHeartbeat();                // Heartbeat verarbeiten
    pruefeTelnetZugang();                 // Telnet-Login pruefen
    verwalteWlanVerbindung();             // WLAN-Failover
    aktualisiereWlanLed();                // WLAN-LED aktualisieren

    if (HAL::wlanVerbunden())             // Verbunden
        HAL::mdnsUpdate();                // mDNS-Verarbeitung
}

// FSM: SENDEN-Zustand
void fsmSenden() {
    // PRIORITY MODE: Nur UDP-Transaktion + Reset-Taster
    // Kein Heartbeat, kein WLAN-Scan (wuerde ACK verzoegern)

    int result = pruefeUdpAntwort();      // ACK pruefen

    if (result == 1) {                    // ACK empfangen
        sendeProtokoll("Erfolg: Validiertes ACK erhalten!");  // Log
        HAL::alarmLed(letzterBefehlWarAlarmAn);  // LED entsprechend setzen
        ausstehendeNachricht[0] = '\0';   // Nachricht loeschen
        aktuellerZustand = ZUSTAND_BEREIT;  // Zurueck zu BEREIT
        return;                           // Funktion beenden
    }

    if (result == -1) {                   // Timeout
        sendeProtokoll("FEHLER: Timeout - Empfaenger antwortet nicht!");  // Log
        ausstehendeNachricht[0] = '\0';   // Nachricht loeschen
        aktuellerZustand = ZUSTAND_BEREIT;  // Zurueck zu BEREIT
        return;                           // Funktion beenden
    }

    // Reset-Taster auch im SENDEN-Zustand
    if (pruefePhysischenReset()) {        // Reset ausgeloest
        aktuellerZustand = ZUSTAND_WERKSRESET;  // In Reset-Zustand wechseln
        return;                           // Funktion beenden
    }

    aktualisiereWlanLed();                // WLAN-LED aktualisieren
}

// FSM: WERKSRESET-Zustand
void fsmWerksreset() {
    sendeProtokoll("!!! HARDWARE RESET (SAFE) !!!");  // Log
    HAL::watchdogStoppen();               // Watchdog aus
    HAL::alarmLed(true);                  // LED an
    delay(2000);                          // 2s warten
    HAL::alarmLed(false);                 // LED aus
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
        case ZUSTAND_SENDEN:         fsmSenden();        break;  // SENDEN-Handler
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

// Lokale Verwaltungszugänge werden pro Gerät erzeugt und vor dem ersten
// Captive-Portal-Start persistiert. UDP- und Delivery-Secrets kommen bewusst
// getrennt aus der Server-Bootstrap-Datei und werden nicht lokal erfunden.
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
