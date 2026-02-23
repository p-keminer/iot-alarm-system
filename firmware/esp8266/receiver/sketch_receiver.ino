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
 * Security: HMAC-SHA256, Sliding-Window Replay-Schutz (25 Pakete, LittleFS-persistent),
 *           Constant-Time Signatur-Vergleich, isdigit-Validierung,
 *           Binary String Obfuscation (Flash), DoS Rate-Limiting
 *
 * Resilienz: Flash Wear-Leveling (LittleFS), Emergency QoS (Log-Filterung im Alarm),
 *            Remote-Override via Heartbeat-Kanal, Zero-Allocation UDP-Parsing
 *
 * Priority-Mode: Im ALARM-Zustand werden Heartbeat und WLAN-Scan pausiert.
 *                Nur Hardware-Steuerung und UDP-Empfang laufen (Stop-the-World).
 *
 * Hardware:   NodeMCU V2 (ESP8266)
 * Autor:      Philip Keminer
 * Version:    V14.0 (Receiver - HAL/FSM, OTA entfernt)
 * Datum:      2026-02-23
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
#include <ESP8266mDNS.h>             // mDNS-Responder (*.local)
#include <ESP8266HTTPClient.h>       // HTTP-Client fuer API-Calls
#include <WiFiClient.h>              // TCP-Client
#include <WiFiClientSecure.h>        // Fuer BearSSL HMAC-Bibliothek
#include <TelnetStream.h>            // Debug-Konsole ueber Telnet
#include <Ticker.h>                  // Timer-Interrupts
#include <bearssl/bearssl.h>         // Kryptografie-Bibliothek

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
const uint8_t REPLAY_FENSTER_GROESSE = 25;                // Sliding-Window Breite
const unsigned long SEQ_PERSIST_SCHWELLE = 5;              // Flash-Write alle N Sequenzen

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
// Wer das hier reverse-engineered: Respekt, du hast es dir verdient.

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
    char mdnsName[33] = "";             // Eigener mDNS-Hostname
    char telnetPasswort[21] = "y!Q#I_pPx_%L9gI";  // Telnet-Login (verschleiert)
    char backupSsid[33] = "";           // Fallback-WLAN
    char backupPasswort[65] = "";       // Fallback-WLAN-Passwort
    char hauptWlanName[33] = "";        // Primaeres WLAN
    char hauptWlanPasswort[65] = "";    // Primaeres WLAN-Passwort
    char apPasswort[65] = "y!Q#I_pPx_%L9gI";  // Access-Point-Passwort (verschleiert)
    char apiServer[33] = "";            // API-Server IP-Adresse
    char apiToken[33] = "";             // Bearer-Token fuer API
};

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

    // Startet UDP-Socket
    void udpStarten() { udpSocket.begin(LOKALER_PORT); }

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

    // Verwirft empfangenes Paket
    void udpFlush() { udpSocket.flush(); }

    // --- mDNS ---

    bool mdnsStarten(const char* hostname) { return MDNS.begin(hostname); }  // mDNS-Responder
    void mdnsUpdate() { MDNS.update(); }  // mDNS-Verarbeitung

    // --- Telnet ---

    void telnetStarten() { TelnetStream.begin(); }  // Telnet-Server starten
    bool telnetVerfuegbar() { return TelnetStream.available(); }  // Daten verfuegbar
    String telnetLesen() { String s = TelnetStream.readStringUntil('\n'); s.trim(); return s; }  // Zeile lesen
    void telnetSchreiben(const char* msg) { TelnetStream.println(msg); TelnetStream.flush(); }  // Zeile senden
    void telnetStoppen() { TelnetStream.stop(); }  // Telnet-Server stoppen

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

    // HMAC-SHA256 -> 64 Hex-Zeichen in hexOut (min. 65 Bytes)
    void berechneHMAC(const char* nachricht, size_t nachrichtLen,
                      const char* secret, size_t secretLen,
                      char* hexOut) {
        br_hmac_key_context kc;   // HMAC-Schluessel-Kontext
        br_hmac_context ctx;       // HMAC-Hash-Kontext
        br_hmac_key_init(&kc, &br_sha256_vtable, secret, secretLen);  // Schluessel initialisieren
        br_hmac_init(&ctx, &kc, 0);                  // HMAC-Kontext initialisieren
        br_hmac_update(&ctx, nachricht, nachrichtLen);  // Nachricht hashen
        uint8_t result[32];                          // 32 Bytes = 256 Bit
        br_hmac_out(&ctx, result);                   // Ergebnis extrahieren
        for (int i = 0; i < 32; i++)                 // In Hex konvertieren
            sprintf(hexOut + (i * 2), "%02x", result[i]);  // 2 Hex-Zeichen pro Byte
        hexOut[64] = '\0';                           // Null-Terminierung
    }

    // --- Sliding-Window Replay-Schutz ---

    unsigned long replayWindowBase = 0;  // Hoechste akzeptierte Sequenznummer
    uint32_t replayBitmap = 0;           // Bitmap fuer letzte 25 Sequenzen
    unsigned long letztePersistedSeq = 0;  // Letzte im Flash gespeicherte Sequenz

    // Prueft Sequenznummer gegen Replay-Angriffe
    bool pruefeReplay(unsigned long seq) {
        // Fall 1: Neues Paket (vor dem Fenster)
        if (seq > replayWindowBase) {     // Sequenz hoeher als bisheriges Maximum
            unsigned long shift = seq - replayWindowBase;  // Um wieviel verschieben
            if (shift >= REPLAY_FENSTER_GROESSE)  // Grosse Luecke
                replayBitmap = 0;         // Bitmap komplett zuruecksetzen
            else                          // Kleine Luecke
                replayBitmap <<= shift;   // Bitmap verschieben
            replayWindowBase = seq;       // Neue Basis
            replayBitmap |= 1;            // Aktuelles Bit setzen
            return true;                  // Akzeptieren
        }
        // Fall 2: Innerhalb des Fensters
        unsigned long diff = replayWindowBase - seq;  // Abstand zur Basis
        if (diff >= REPLAY_FENSTER_GROESSE) return false;  // Zu alt
        uint32_t maske = 1UL << diff;     // Bit-Maske fuer diese Sequenz
        if (replayBitmap & maske) return false;  // Bereits gesehen
        replayBitmap |= maske;            // Bit setzen
        return true;                      // Akzeptieren
    }

    // Flash-Wear-Schutz: Nur alle N Inkremente schreiben
    void speichereSequenz() {
        if (replayWindowBase - letztePersistedSeq < SEQ_PERSIST_SCHWELLE) return;  // Noch nicht noetig
        File f = LittleFS.open("/seq.dat", "w");  // Datei zum Schreiben oeffnen
        if (f) {                          // Erfolgreich geoeffnet
            char buf[12];                 // Buffer fuer Zahl
            snprintf(buf, sizeof(buf), "%lu", replayWindowBase);  // In String konvertieren
            f.print(buf);                 // In Datei schreiben
            f.close();                    // Datei schliessen
            letztePersistedSeq = replayWindowBase;  // Timestamp merken
        }
    }

    // Laedt Sequenznummer aus Flash
    void ladeSequenz() {
        if (!LittleFS.exists("/seq.dat")) return;  // Datei existiert nicht
        File f = LittleFS.open("/seq.dat", "r");   // Datei zum Lesen oeffnen
        if (f) {                          // Erfolgreich geoeffnet
            char buf[12];                 // Buffer fuer Zahl
            size_t len = f.readBytes(buf, sizeof(buf) - 1);  // Daten lesen
            buf[len] = '\0';              // Null-Terminierung
            f.close();                    // Datei schliessen
            if (nurZiffern(buf)) {        // Nur Ziffern
                replayWindowBase = strtoul(buf, NULL, 10);  // String in Zahl konvertieren
                letztePersistedSeq = replayWindowBase;  // Timestamp setzen
            }
        }
    }

} // namespace Security

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
int scanStatus = -1;                          // Async-Scan-Status

// Sicherheit
uint8_t telnetFehlversuche = 0;       // Login-Fehlversuche
unsigned long telnetSperreBis = 0;    // Timestamp Sperrende
uint8_t udpPaketZaehler = 0;          // UDP-Pakete pro Minute (DoS-Schutz)
unsigned long udpZaehlerReset = 0;    // Timestamp letzter Zaehler-Reset

// ============================================================================
// CONFIG PERSISTENZ
// ============================================================================

// Callback von WiFiManager (wird bei Config-Aenderung aufgerufen)
void konfigurationSpeichernCallback() {
    konfigurationSpeichern = true;  // Flag setzen
}

// Laedt Konfiguration aus config.json
void ladeKonfiguration() {
    if (LittleFS.begin()) {                         // Dateisystem mounten
        if (LittleFS.exists("/config.json")) {      // Config existiert
            File datei = LittleFS.open("/config.json", "r");  // Datei oeffnen
            if (datei) {                            // Erfolgreich geoeffnet
                DynamicJsonDocument doc(1024);      // JSON-Dokument
                DeserializationError fehler = deserializeJson(doc, datei);  // JSON parsen
                if (!fehler) {                      // Kein Parse-Fehler
                    strlcpy(config.udpToken, doc["token"] | "", sizeof(config.udpToken));  // Token lesen
                    strlcpy(config.mdnsName, doc["name"] | "", sizeof(config.mdnsName));   // mDNS-Name
                    strlcpy(config.telnetPasswort, doc["tpass"] | "", sizeof(config.telnetPasswort));  // Telnet-PW
                    strlcpy(config.backupSsid, doc["bssid"] | "", sizeof(config.backupSsid));  // Backup-SSID
                    strlcpy(config.backupPasswort, doc["bpass"] | "", sizeof(config.backupPasswort));  // Backup-PW
                    strlcpy(config.hauptWlanName, doc["hssid"] | "", sizeof(config.hauptWlanName));  // Haupt-SSID
                    strlcpy(config.hauptWlanPasswort, doc["hpass"] | "", sizeof(config.hauptWlanPasswort));  // Haupt-PW
                    strlcpy(config.apPasswort, doc["appw"] | "", sizeof(config.apPasswort));  // AP-Passwort
                    strlcpy(config.apiServer, doc["apiip"] | "", sizeof(config.apiServer));  // API-Server
                    strlcpy(config.apiToken, doc["apitoken"] | "", sizeof(config.apiToken));  // API-Token
                }
            }
        }
    }
}

// Speichert Konfiguration in config.json
void speichereKonfiguration() {
    DynamicJsonDocument doc(1024);    // JSON-Dokument
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
    File datei = LittleFS.open("/config.json", "w");  // Datei zum Schreiben oeffnen
    if (datei) {                          // Erfolgreich geoeffnet
        serializeJson(doc, datei);        // JSON in Datei schreiben
        datei.close();                    // Datei schliessen
    }
}

// ============================================================================
// LOGGING & PROTOKOLL
// ============================================================================

// Sendet Log-Nachricht an API-Server
void sendeLogAnApi(const char* nachricht) {
    if (!darfLoggen || !HAL::wlanVerbunden()) return;  // Nicht erlaubt oder nicht verbunden
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
    Serial.println(nachricht);            // Serial-Konsole
    if (HAL::wlanVerbunden() && telnetAutorisiert)  // Telnet wenn verbunden und autorisiert
        HAL::telnetSchreiben(nachricht);  // Telnet-Ausgabe
    sendeLogAnApi(nachricht);             // API-Logging
}

// ============================================================================
// SERVICE-FUNKTIONEN
// ============================================================================

// --- UDP-Empfang und Validierung ---
// Rueckgabe: 1 = ALARM_ON, -1 = ALARM_OFF, 0 = nichts/ungueltig
int verarbeiteUdpEmpfang() {
    int paketGroesse = HAL::udpPaketVerfuegbar();  // Paket empfangen?
    if (!paketGroesse) return 0;          // Kein Paket

    // DoS-Schutz: Rate Limiting
    if (udpPaketZaehler >= UDP_MAX_PAKETE_PRO_MINUTE) {  // Limit erreicht
        if (HAL::zeitMs() - udpZaehlerReset > 60000) {  // Minute abgelaufen
            udpPaketZaehler = 0;          // Zaehler zuruecksetzen
            udpZaehlerReset = HAL::zeitMs();  // Timestamp merken
        } else {                          // Noch gesperrt
            HAL::udpFlush();              // Paket verwerfen
            return 0;                     // Ignorieren
        }
    }
    udpPaketZaehler++;                    // Zaehler erhoehen

    // char-Array basiertes Parsing (kein Arduino String auf dem Hot-Path)
    char puffer[512];                     // Buffer fuer Paket
    int laenge = HAL::udpLesen(puffer, sizeof(puffer) - 1);  // Paket lesen
    if (laenge <= 0) return 0;            // Lese-Fehler
    puffer[laenge] = '\0';                // Null-Terminierung

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
    size_t sigLen = strlen(empfangeneSignatur);  // Laenge der Signatur
    if (sigLen != 64) return 0;           // Falsche Laenge

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
    if (!Security::pruefeReplay(empfangeneSeq)) return 0;  // Replay erkannt

    // Sequenz periodisch in Flash sichern
    Security::speichereSequenz();         // Flash-Write (rate-limited)

    // ACK senden
    char ack[32];                         // ACK-Buffer
    snprintf(ack, sizeof(ack), "ACK_SECURE:%s", seqString);  // ACK bauen

    if (strcmp(befehl, CMD_ALARM_AN) == 0) {  // ALARM_ON-Befehl
        if (!alarmAktiv) sendeProtokoll("ALARM ON (UDP)");  // Log nur bei Zustandsaenderung
        HAL::udpAntworten(ack);           // ACK senden
        return 1;                         // ALARM_ON
    }
    else if (strcmp(befehl, CMD_ALARM_AUS) == 0) {  // ALARM_OFF-Befehl
        if (alarmAktiv) sendeProtokoll("ALARM OFF (UDP)");  // Log nur bei Zustandsaenderung
        HAL::udpAntworten(ack);           // ACK senden
        return -1;                        // ALARM_OFF
    }

    return 0;                             // Unbekannter Befehl
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

// --- Heartbeat mit Server-Befehlen ---
void verarbeiteHeartbeat() {
    if (HAL::zeitMs() - letzterHeartbeat < aktuellesHeartbeatIntervall) return;  // Intervall noch nicht abgelaufen
    if (!HAL::wlanVerbunden()) { letzterHeartbeat = HAL::zeitMs(); return; }  // Nicht verbunden

    WiFiClient client;                    // HTTP-Client
    client.setTimeout(1000);              // Timeout 1s
    HTTPClient http;                      // HTTP-Client-Wrapper
    http.setTimeout(1000);                // Timeout 1s
    String serverPath = "http://" + String(config.apiServer) + "/api.php";  // API-URL

    if (http.begin(client, serverPath)) { // HTTP-Verbindung oeffnen
        http.addHeader("Content-Type", "application/json");  // Content-Type setzen
        if (strlen(config.apiToken) > 0) {  // Token vorhanden
            http.addHeader("Authorization", String("Bearer ") + config.apiToken);  // Bearer-Token
            http.addHeader("X-ESP-Token", config.apiToken);  // Custom-Header
        }
        StaticJsonDocument<384> doc;      // JSON-Dokument
        doc["source"] = "receiver";       // Absender
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

            if (code == 200) {            // Erfolgreiche Antwort
                String payload = http.getString();  // Response-Body
                DynamicJsonDocument antwort(1024);  // JSON-Dokument
                deserializeJson(antwort, payload);  // JSON parsen

                if (!antwort.isNull()) {  // JSON gueltig
                    if (antwort.containsKey("logging_active"))  // Logging-Flag
                        darfLoggen = antwort["logging_active"];  // Flag setzen

                    // Remote-Konfigurationsupdate
                    if (antwort.containsKey("new_config")) {  // Config-Update vom Server
                        JsonObject newConf = antwort["new_config"];  // Config-Objekt
                        bool neustartNoetig = false;  // Neustart-Flag
                        if (newConf.containsKey("mssid")) {  // Haupt-SSID
                            strlcpy(config.hauptWlanName, newConf["mssid"] | "", sizeof(config.hauptWlanName));  // Kopieren
                            neustartNoetig = true;    // Neustart markieren
                        }
                        if (newConf.containsKey("mpass")) {  // Haupt-PW
                            strlcpy(config.hauptWlanPasswort, newConf["mpass"] | "", sizeof(config.hauptWlanPasswort));  // Kopieren
                            neustartNoetig = true;    // Neustart markieren
                        }
                        if (newConf.containsKey("bssid")) {  // Backup-SSID
                           strlcpy(config.backupSsid, newConf["bssid"] | "", sizeof(config.backupSsid));  // Kopieren
                            neustartNoetig = true;    // Neustart markieren
                        }
                        if (newConf.containsKey("bpass")) {  // Backup-PW
                         strlcpy(config.backupPasswort, newConf["bpass"] | "", sizeof(config.backupPasswort));  // Kopieren
                            neustartNoetig = true;    // Neustart markieren
                        }
                        
                        if (newConf.containsKey("tpass")) {  // Telnet-PW
                            strlcpy(config.telnetPasswort, newConf["tpass"] | "", sizeof(config.telnetPasswort));  // Kopieren
                            neustartNoetig = true;    // Neustart markieren
                        }
                        speichereKonfiguration();     // Config speichern
                        if (neustartNoetig) {         // Neustart noetig
                            delay(500);               // Kurz warten
                            HAL::neustart();          // Neustart durchfuehren
                        }
                    }

                    // Remote-Befehle
                    if (antwort.containsKey("command")) {  // Server-Befehl
                        String befehl = antwort["command"];  // Befehl auslesen
                        if (befehl == "REBOOT") { delay(500); HAL::neustart(); }  // Neustart
                        else if (befehl == "RESET") {  // Factory-Reset
                            HAL::flashFormatieren();  // Flash loeschen
                            HAL::wlanCredentialsLoeschen();  // WLAN-Credentials loeschen
                            HAL::neustart();          // Neustart
                        }
                        else if (befehl == "ALARM_ON") { alarmAktiv = true; }  // Alarm an
                        else if (befehl == "ALARM_OFF") { alarmAktiv = false; }  // Alarm aus
                    }
                }
            }
        } else {                          // Fehler oder Timeout
            aktuellesHeartbeatIntervall = 60000;  // Langsamer retries (1 Minute)
        }
        http.end();                       // Verbindung schliessen
    } else {                              // Verbindung fehlgeschlagen
        aktuellesHeartbeatIntervall = 60000;  // Langsamer retries
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
    static int stabilitaetsZaehler = 0;   // Zaehlt stabile Hauptnetz-Scans

    if (HAL::wlanVerbunden()) {           // Verbunden
        String ssid = HAL::wlanSsid();    // Aktuelle SSID
        if (ssid == String(config.backupSsid) && strlen(config.backupSsid) > 0) {  // Im Backup-Netz
            if (HAL::zeitMs() - letzterScanStart > WLAN_SCAN_INTERVALL && scanStatus == -1) {  // Scan starten
                letzterScanStart = HAL::zeitMs();  // Timestamp merken
                HAL::wlanScanStarten();   // Async-Scan starten
                scanStatus = 0;           // Status auf "laufend"
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
                        if (stabilitaetsZaehler >= STABILITAETS_SCHWELLWERT) {  // 3x stabil erkannt
                            HAL::watchdogStoppen();  // Watchdog aus (Reconnect dauert)
                            for (int k = 0; k < 10; k++) { HAL::wlanLedToggle(); delay(50); }  // Visuelles Feedback
                            if (strlen(config.hauptWlanPasswort) > 0)  // Passwort vorhanden
                                HAL::wlanVerbinden(config.hauptWlanName, config.hauptWlanPasswort);  // Verbinden
                            else          // Kein Passwort
                                HAL::neustart();  // Neustart (WiFiManager)
                        }
                    } else {              // Hauptnetz nicht gefunden
                        stabilitaetsZaehler = 0;  // Zaehler zuruecksetzen
                    }
                    HAL::wlanScanLoeschen();  // Scan-Ergebnisse freigeben
                    scanStatus = -1;      // Status zuruecksetzen
                }
            }
        } else {                          // Im Hauptnetz oder kein Backup
            stabilitaetsZaehler = 0;      // Zaehler zuruecksetzen
        }
    } else {                              // Nicht verbunden
        if (HAL::zeitMs() - letzterVerbindungsVersuch > WLAN_WIEDERHOLUNGS_INTERVALL) {  // Reconnect-Intervall
            if (strlen(config.backupSsid) > 0) {  // Backup konfiguriert
                HAL::wlanTrennen();       // Alte Verbindung trennen
                unsigned long start = HAL::zeitMs();  // Timestamp
                while (HAL::zeitMs() - start < 500) { HAL::watchdogFuettern(); HAL::cpuFreigeben(); }  // 500ms warten
                HAL::wlanVerbinden(config.backupSsid, config.backupPasswort);  // Verbinden mit Backup
                letzterVerbindungsVersuch = HAL::zeitMs();  // Timestamp aktualisieren
                letzterScanStart = HAL::zeitMs();  // Scan-Timestamp zuruecksetzen
                stabilitaetsZaehler = 0;  // Stabilitaets-Zaehler zuruecksetzen
            } else {                      // Kein Backup
                letzterVerbindungsVersuch = HAL::zeitMs();  // Timestamp aktualisieren
            }
        }
    }
}

// --- Telnet ---
void pruefeTelnetZugang() {
    if (HAL::zeitMs() < telnetSperreBis) return;  // Noch gesperrt

    if (!HAL::telnetVerfuegbar()) return;  // Keine Eingabe

    String eingabe = HAL::telnetLesen();  // Zeile lesen
    if (eingabe.length() == 0) return;    // Leer

    if (eingabe == String(config.telnetPasswort)) {  // Passwort korrekt
        telnetAutorisiert = true;         // Login erfolgreich
        telnetFehlversuche = 0;           // Fehlversuche zuruecksetzen
        HAL::telnetSchreiben("LOGIN OK"); // Bestaetigung
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
    HAL::init();                          // Hardware initialisieren
    HAL::flashInit();                     // Dateisystem mounten
    // Verschleierte Defaults entschluesseln (einmalig beim Start)
    // Falls config.json existiert, werden diese sowieso ueberschrieben
    deobfuscate(config.apPasswort);       // AP-Passwort entschluesseln
    deobfuscate(config.telnetPasswort);   // Telnet-Passwort entschluesseln
    ladeKonfiguration();                  // Config aus Flash laden
    Security::ladeSequenz();              // Replay-Window laden
    aktuellerZustand = ZUSTAND_WLAN_VERBINDEN;  // Naechster Zustand
}

// FSM: WLAN_VERBINDEN-Zustand
void fsmWlanVerbinden() {
    WiFiManager wm;                       // WiFiManager-Objekt
    wm.setSaveConfigCallback(konfigurationSpeichernCallback);  // Callback registrieren

    // WiFiManager-Parameter definieren
    WiFiManagerParameter p_tpass("tpass", "Telnet PW", config.telnetPasswort, 20);  // Telnet-PW
    WiFiManagerParameter p_token("token", "HMAC Secret", config.udpToken, 40);  // Token
    WiFiManagerParameter p_name("name", "mDNS Name", config.mdnsName, 32);  // mDNS-Name
    WiFiManagerParameter p_api("apiip", "API Server IP", config.apiServer, 32);  // API-Server
    WiFiManagerParameter p_bssid("bssid", "Backup SSID", config.backupSsid, 32);  // Backup-SSID
    WiFiManagerParameter p_bpass("bpass", "Backup PW", config.backupPasswort, 64);  // Backup-PW
    WiFiManagerParameter p_hssid("hssid", "Haupt SSID", config.hauptWlanName, 32);  // Haupt-SSID
    WiFiManagerParameter p_hpass("hpass", "Haupt PW", config.hauptWlanPasswort, 64);  // Haupt-PW
    WiFiManagerParameter p_appw("appw", "AP Passwort", config.apPasswort, 64);  // AP-Passwort
    WiFiManagerParameter p_apitoken("apitoken", "API Token", config.apiToken, 32);  // API-Token

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

    wm.setClass("invert");                // Dunkles Theme
    wm.setConfigPortalTimeout(180);       // Portal-Timeout 3min
    wm.setConnectTimeout(30);             // Verbindungs-Timeout 30s

    // Hauptnetz direkt versuchen
    if (strlen(config.hauptWlanName) > 0)  // Hauptnetz konfiguriert
        HAL::wlanVerbinden(config.hauptWlanName, config.hauptWlanPasswort);  // Verbinden

    if (!HAL::wlanVerbunden())            // Nicht verbunden
        wm.autoConnect("Alarm-Empfaenger-SETUP");  // Captive Portal starten

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

    if (konfigurationSpeichern || HAL::wlanVerbunden())  // Config geaendert oder verbunden
        speichereKonfiguration();         // Config speichern

    // Dienste starten
    if (HAL::mdnsStarten(config.mdnsName))  // mDNS starten
        Serial.println("mDNS aktiv");     // Info
    HAL::telnetStarten();                 // Telnet starten
    HAL::udpStarten();                    // UDP starten

    HAL::watchdogStarten();               // Watchdog starten
    aktuellerZustand = ZUSTAND_BEREIT;    // Naechster Zustand
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
        alarmAktiv = !alarmAktiv;         // Status umschalten
        sendeProtokoll("Btn Toggle");     // Log
        if (alarmAktiv) { aktuellerZustand = ZUSTAND_ALARM; return; }  // In ALARM wechseln
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
        alarmAktiv = false;               // Alarm aus
        sendeProtokoll("Btn Toggle");     // Log
        aktuellerZustand = ZUSTAND_BEREIT;  // Zurueck zu BEREIT
        return;                           // Funktion beenden
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