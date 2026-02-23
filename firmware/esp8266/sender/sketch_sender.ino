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
 * Version:    V14.0 (Sender - HAL/FSM, OTA entfernt)
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
#include <ESP8266mDNS.h>             // mDNS-Namensaufloesung (*.local)
#include <ESP8266HTTPClient.h>       // HTTP-Client fuer API-Calls
#include <WiFiClient.h>              // TCP-Client
#include <WiFiClientSecure.h>        // Fuer BearSSL HMAC-Bibliothek
#include <TelnetStream.h>            // Debug-Konsole ueber Telnet
#include <Ticker.h>                  // Timer-Interrupts
#include <bearssl/bearssl.h>         // Kryptografie-Bibliothek

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

// --- Obfuscated Payloads (Muss mit Empfaenger uebereinstimmen!) ---
const char* CMD_ALARM_AN  = "NICE_TRY_WIRESHARK_USER";     // Verschleierter ALARM_ON Befehl
const char* CMD_ALARM_AUS = "ENCRYPTION_IS_YOUR_FRIEND";   // Verschleierter ALARM_OFF Befehl

// --- Pins ---
#define PIN_RESET_TASTER D3            // Hardware-Reset-Taster
#define PIN_LED_ALARM LED_BUILTIN      // Alarm-LED (invertiert: LOW = an)
#define PIN_LED_WLAN D5                // WLAN-Status-LED

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
    char mdnsZiel[33] = "";             // Empfaenger-Hostname (ohne .local)
    char apiServer[33] = "";            // API-Server IP-Adresse
    char telnetPasswort[21] = "y!Q#u_pPx_%L9gI";  // Telnet-Login (verschleiert)
    char backupSsid[33] = "";           // Fallback-WLAN
    char backupPasswort[65] = "";       // Fallback-WLAN-Passwort
    char hauptWlanName[33] = "";        // Primaeres WLAN
    char hauptWlanPasswort[65] = "";    // Primaeres WLAN-Passwort
    char apPasswort[65] = "y!Q#u_pPx_%L9gI";  // Access-Point-Passwort (verschleiert)
    char apiToken[33] = "";             // Bearer-Token fuer API
};

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

    // --- mDNS ---

    bool mdnsStarten(const char* hostname) { return MDNS.begin(hostname); }  // mDNS-Responder
    void mdnsUpdate() { MDNS.update(); }  // mDNS-Verarbeitung

    // --- Telnet ---

    void telnetStarten() { TelnetStream.begin(); }  // Telnet-Server starten
    bool telnetVerfuegbar() { return TelnetStream.available(); }  // Daten verfuegbar
    String telnetLesen() { String s = TelnetStream.readStringUntil('\n'); s.trim(); return s; }  // Zeile lesen
    void telnetSchreiben(const char* msg) { TelnetStream.println(msg); TelnetStream.flush(); }  // Zeile senden
    void telnetStoppen() { TelnetStream.stop(); }  // Telnet-Server stoppen

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

    // --- Sequenznummer (LittleFS-persistent, ueberlebt Reboot) ---

    unsigned long sequenceNumber = 0;  // Aktuelle Sequenznummer

    // Speichert Sequenznummer im Flash
    void speichereSequenz() {
        File f = LittleFS.open("/seq.dat", "w");  // Datei zum Schreiben oeffnen
        if (f) {                                  // Erfolgreich geoeffnet
            char buf[12];                         // Buffer fuer Zahl
            snprintf(buf, sizeof(buf), "%lu", sequenceNumber);  // In String konvertieren
            f.print(buf);                         // In Datei schreiben
            f.close();                            // Datei schliessen
        }
    }

    // Laedt Sequenznummer aus Flash
    void ladeSequenz() {
        if (!LittleFS.exists("/seq.dat")) return;  // Datei existiert nicht
        File f = LittleFS.open("/seq.dat", "r");   // Datei zum Lesen oeffnen
        if (f) {                                   // Erfolgreich geoeffnet
            char buf[12];                          // Buffer fuer Zahl
            size_t len = f.readBytes(buf, sizeof(buf) - 1);  // Daten lesen
            buf[len] = '\0';                       // Null-Terminierung
            f.close();                             // Datei schliessen
            if (nurZiffern(buf))                   // Nur Ziffern
                sequenceNumber = strtoul(buf, NULL, 10);  // String in Zahl konvertieren
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
int scanStatus = -1;                          // Async-Scan-Status

// Telnet Sicherheit
uint8_t telnetFehlversuche = 0;       // Login-Fehlversuche
unsigned long telnetSperreBis = 0;    // Timestamp Sperrende

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
                    strlcpy(config.mdnsZiel, doc["ziel"] | "", sizeof(config.mdnsZiel));   // Ziel lesen
                    strlcpy(config.telnetPasswort, doc["tpass"] | "", sizeof(config.telnetPasswort));  // Telnet-PW lesen
                    strlcpy(config.backupSsid, doc["bssid"] | "", sizeof(config.backupSsid));  // Backup-SSID
                    strlcpy(config.backupPasswort, doc["bpass"] | "", sizeof(config.backupPasswort));  // Backup-PW
                    strlcpy(config.hauptWlanName, doc["hssid"] | "", sizeof(config.hauptWlanName));  // Haupt-SSID
                    strlcpy(config.hauptWlanPasswort, doc["hpass"] | "", sizeof(config.hauptWlanPasswort));  // Haupt-PW
                    strlcpy(config.apPasswort, doc["appw"] | "", sizeof(config.apPasswort));  // AP-Passwort
                    strlcpy(config.apiServer, doc["api"] | "", sizeof(config.apiServer));  // API-Server
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
    doc["ziel"] = config.mdnsZiel;    // Ziel schreiben
    doc["tpass"] = config.telnetPasswort;  // Telnet-PW schreiben
    doc["bssid"] = config.backupSsid;      // Backup-SSID
    doc["bpass"] = config.backupPasswort;  // Backup-PW
    doc["hssid"] = config.hauptWlanName;   // Haupt-SSID
    doc["hpass"] = config.hauptWlanPasswort;  // Haupt-PW
    doc["api"] = config.apiServer;         // API-Server
    doc["appw"]  = config.apPasswort;      // AP-Passwort
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
    Serial.println(nachricht);            // Serial-Konsole
    if (HAL::wlanVerbunden() && telnetAutorisiert)  // Telnet wenn verbunden und autorisiert
        HAL::telnetSchreiben(nachricht);  // Telnet-Ausgabe
    sendeLogAnApi(nachricht);             // API-Logging
}

// ============================================================================
// SERVICE-FUNKTIONEN
// ============================================================================

// --- Signiertes UDP-Paket erstellen und senden ---
void starteUdpTransaktion(const char* klarBefehl) {
    Security::sequenceNumber++;           // Sequenznummer erhoehen
    Security::speichereSequenz();         // Im Flash speichern

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

        // Erwartetes ACK: "ACK_SECURE:SEQUENZNUMMER"
        char erwartet[32];                // Buffer fuer erwartetes ACK
        int erwLen = snprintf(erwartet, sizeof(erwartet), "ACK_SECURE:%lu", Security::sequenceNumber);  // ACK bauen
        if (erwLen < 0 || (size_t)erwLen >= sizeof(erwartet)) return 0;  // Overflow

        if ((size_t)laenge == (size_t)erwLen &&  // Laenge stimmt
            Security::sichererVergleich(puffer, erwartet, (size_t)erwLen)) {  // Constant-Time-Vergleich
            return 1;                     // ACK korrekt
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

// --- Heartbeat (nur im BEREIT-Zustand) ---
void verarbeiteHeartbeat() {
    if (HAL::zeitMs() - letzterHeartbeat < aktuellesHeartbeatIntervall) return;  // Intervall noch nicht abgelaufen
    if (!HAL::wlanVerbunden()) { letzterHeartbeat = HAL::zeitMs(); return; }  // Nicht verbunden

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
        StaticJsonDocument<512> doc;      // JSON-Dokument (groesser wegen Telemetrie)
        doc["source"] = DEVICE_NAME;      // Absender
        doc["ip"] = HAL::wlanIp();        // IP-Adresse
        doc["status_msg"] = (aktuellerZustand == ZUSTAND_SENDEN) ? "Sende Cmd..." : "Bereit";  // Status
        doc["rssi"] = HAL::wlanRssi();    // Signalstaerke
        doc["heap"] = HAL::freierHeap();  // Freier RAM
        doc["uptime"] = millis() / 1000;  // Uptime in Sekunden
        doc["reset_reason"] = HAL::resetGrund();  // Reset-Grund
        if (ausstehendeNachricht[0] != '\0')  // Nachricht ausstehend
            doc["log"] = String("Pending: ") + ausstehendeNachricht;  // In Log aufnehmen
        String body;                      // JSON-String
        serializeJson(doc, body);         // JSON serialisieren
        int code = http.POST(body);       // POST-Request senden

        if (code > 0) {                   // Antwort erhalten
            aktuellesHeartbeatIntervall = HEARTBEAT_INTERVALL;  // Normales Intervall

           if (code == 200) {             // Erfolgreiche Antwort
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
                        if (newConf.containsKey("apiip")) {  // API-Server
                            strlcpy(config.apiServer, newConf["apiip"] | "", sizeof(config.apiServer));  // Kopieren
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
    HAL::init();                          // Hardware initialisieren
    HAL::flashInit();                     // Dateisystem mounten
    // Verschleierte Defaults entschluesseln (einmalig beim Start)
    // Falls config.json existiert, werden diese sowieso ueberschrieben
    deobfuscate(config.apPasswort);       // AP-Passwort entschluesseln
    deobfuscate(config.telnetPasswort);   // Telnet-Passwort entschluesseln
    ladeKonfiguration();                  // Config aus Flash laden
    Security::ladeSequenz();              // Sequenznummer laden
    aktuellerZustand = ZUSTAND_WLAN_VERBINDEN;  // Naechster Zustand
}

// FSM: WLAN_VERBINDEN-Zustand
void fsmWlanVerbinden() {
    WiFiManager wm;                       // WiFiManager-Objekt
    wm.setSaveConfigCallback(konfigurationSpeichernCallback);  // Callback registrieren

    // WiFiManager-Parameter definieren
    WiFiManagerParameter p_api("api", "API Server IP", config.apiServer, 32);  // API-Server
    WiFiManagerParameter p_token("token", "HMAC Secret", config.udpToken, 40);  // Token
    WiFiManagerParameter p_tpass("tpass", "Telnet PW", config.telnetPasswort, 20);  // Telnet-PW
    WiFiManagerParameter p_ziel("ziel", "Empfaenger mDNS", config.mdnsZiel, 32);  // Empfaenger
    WiFiManagerParameter p_bssid("bssid", "Backup SSID", config.backupSsid, 32);  // Backup-SSID
    WiFiManagerParameter p_bpass("bpass", "Backup PW", config.backupPasswort, 64);  // Backup-PW
    WiFiManagerParameter p_hssid("hssid", "Haupt SSID", config.hauptWlanName, 32);  // Haupt-SSID
    WiFiManagerParameter p_hpass("hpass", "Haupt PW", config.hauptWlanPasswort, 64);  // Haupt-PW
    WiFiManagerParameter p_appw("appw", "AP Passwort", config.apPasswort, 64);  // AP-Passwort
    WiFiManagerParameter p_apitoken("apitoken", "API Token", config.apiToken, 32);  // API-Token

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

    wm.setClass("invert");                // Dunkles Theme
    wm.setConfigPortalTimeout(180);       // Portal-Timeout 3min
    wm.setConnectTimeout(30);             // Verbindungs-Timeout 30s

    Serial.println("Starte WiFiManager...");  // Info

    // Hauptnetz direkt versuchen wenn konfiguriert
    if (strlen(config.hauptWlanName) > 0 && strlen(config.hauptWlanPasswort) > 0) {  // Hauptnetz konfiguriert
        HAL::wlanVerbinden(config.hauptWlanName, config.hauptWlanPasswort);  // Verbinden
        int i = 0;                        // Zaehler
        while (i < 20 && !HAL::wlanVerbunden()) { delay(500); i++; Serial.print("."); }  // Warten
    }

    // Wenn nicht verbunden: Captive Portal starten
    if (!HAL::wlanVerbunden()) {          // Nicht verbunden
        bool erfolg;                      // Erfolgs-Flag
        if (strlen(config.apPasswort) >= 8)  // AP-Passwort lang genug
            erfolg = wm.autoConnect("Alarm-Sender-Konfig", config.apPasswort);  // Portal mit Passwort
        else                              // AP-Passwort zu kurz
            erfolg = wm.autoConnect("Alarm-Sender-SETUP-OPEN");  // Portal ohne Passwort
        if (!erfolg) Serial.println("Offline Start...");  // Fehler
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

    if (konfigurationSpeichern || HAL::wlanVerbunden())  // Config geaendert oder verbunden
        speichereKonfiguration();         // Config speichern

    // Dienste starten
    if (HAL::mdnsStarten(DEVICE_NAME))    // mDNS starten
        Serial.println("mDNS gestartet"); // Info
    HAL::telnetStarten();                 // Telnet starten
    HAL::udpStarten();                    // UDP starten
    HAL::zielIpAktualisieren(config.mdnsZiel);  // Empfaenger-IP aufloesen

    if (HAL::wlanVerbunden()) {           // Verbunden
        darfLoggen = true;                // Logging erlauben
        letzterHeartbeat = 0;             // Heartbeat sofort senden
        verarbeiteHeartbeat();            // Ersten Heartbeat senden
        sendeLogAnApi("System erfolgreich gestartet!");  // Startup-Log
    }

    HAL::watchdogStarten();               // Watchdog starten
    Serial.println("Watchdog aktiv. Loop beginnt.");  // Info
    aktuellerZustand = ZUSTAND_BEREIT;    // Naechster Zustand
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