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
 *           Constant-Time Signatur-Vergleich, isdigit-Validierung, Traffic Obfuscation,
 *           DoS Rate-Limiting, Telnet Brute-Force-Schutz
 *
 * Priority-Mode: Im ALARM-Zustand werden Heartbeat, Telnet und WLAN-Scan pausiert.
 *                Nur Hardware-Steuerung und UDP-Empfang laufen (Stop-the-World).
 *
 * Hardware:   NodeMCU V2 (ESP8266)
 * Autor:      Philip Keminer
 * Version:    V13.0 (Receiver - HAL/FSM, OTA entfernt)
 * Datum:      2026-02-11
 */

// ============================================================================
// BIBLIOTHEKEN
// ============================================================================

#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>   // Fuer BearSSL HMAC-Bibliothek
#include <TelnetStream.h>
#include <Ticker.h>
#include <bearssl/bearssl.h>

// ============================================================================
// KONSTANTEN
// ============================================================================

const char* DEVICE_NAME = "alarm-receiver";

// --- Timing ---
const unsigned long ALARM_TOGGLE_INTERVALL = 200;          // LED/Summer-Wechsel: 5 Hz
const unsigned long WLAN_WIEDERHOLUNGS_INTERVALL = 20000;  // Reconnect-Pause
const unsigned long WLAN_SCAN_INTERVALL = 30000;           // Scan nach Hauptnetz
const uint8_t STABILITAETS_SCHWELLWERT = 3;                // Hauptnetz muss 3x stabil sein
const int8_t RSSI_SCHWELLWERT = -75;                       // Mindest-Signalstaerke
const unsigned long TASTER_LANG_DRUCK = 1000;              // Kurzdruck-Schwelle: <1s = Toggle
const unsigned long TASTER_RESET_DRUCK = 10000;            // Langdruck: >10s = Factory Reset
const unsigned long BLINK_INTERVALL = 500;                 // WLAN-LED Blink-Intervall
const unsigned long HEARTBEAT_INTERVALL = 2000;            // Telemetrie-Intervall
const int WATCHDOG_TIMEOUT_SEK = 30;                       // Loop-Ueberwachung
const unsigned long TELNET_TIMEOUT = 300000;               // Session-Timeout: 5 Minuten

// --- Sicherheit ---
const uint8_t UDP_MAX_PAKETE_PRO_MINUTE = 60;             // DoS Rate-Limit
const uint8_t MAX_TELNET_VERSUCHE = 3;                    // Brute-Force-Schutz
const uint8_t REPLAY_FENSTER_GROESSE = 25;                // Sliding-Window Breite
const unsigned long SEQ_PERSIST_SCHWELLE = 5;              // Flash-Write alle N Sequenzen

// --- Obfuscated Payloads (Muss mit Sender uebereinstimmen!) ---
const char* CMD_ALARM_AN  = "NICE_TRY_WIRESHARK_USER";
const char* CMD_ALARM_AUS = "ENCRYPTION_IS_YOUR_FRIEND";

// --- Pins ---
#define PIN_LED_ROT  D1   // Alarm-LED 1 (wechselt mit Gelb)
#define PIN_LED_GELB D2   // Alarm-LED 2 (wechselt mit Rot)
#define PIN_LED_WLAN D3   // WLAN-Status-LED
#define PIN_SUMMER_1 D5   // Akustischer Alarm 1
#define PIN_SUMMER_2 D6   // Akustischer Alarm 2
#define PIN_TASTER   D7   // Toggle (<1s) / Reset (>10s)

// ============================================================================
// QUELLTEXT-VERSCHLEIERUNG (Kein Klartext in der Firmware)
// ============================================================================
// Wer das hier reverse-engineered: Respekt, du hast es dir verdient.

// Diese Defaults hier sind nur Fallbacks fuer den allerersten Start.


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

void deobfuscate(char* text) {
    for (int i = 0; text[i] != '\0'; i++) {
        unsigned char idx = (unsigned char)text[i];
        if (idx < 128) {
            char decoded = (char)pgm_read_byte(&DECODER_TABLE[idx]);
            if (decoded != (char)idx) text[i] = decoded;
        }
    }
}

// ============================================================================
// DATENSTRUKTUREN
// ============================================================================

struct SystemKonfiguration {
    char udpToken[41] = "";
    char mdnsName[33] = "";
    char telnetPasswort[21] = "y!Q#u_pPx_%L9gI";
    char backupSsid[33] = "";
    char backupPasswort[65] = "";
    char hauptWlanName[33] = "";
    char hauptWlanPasswort[65] = "";
    char apPasswort[65] = "y!Q#u_pPx_%L9gI";
    char apiServer[33] = "";
    char apiToken[33] = "";    
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

    void gpioInit() {
        pinMode(PIN_LED_WLAN, OUTPUT);
        pinMode(PIN_LED_ROT, OUTPUT);
        pinMode(PIN_LED_GELB, OUTPUT);
        pinMode(PIN_SUMMER_1, OUTPUT);
        pinMode(PIN_SUMMER_2, OUTPUT);
        pinMode(PIN_TASTER, INPUT_PULLUP);

        digitalWrite(PIN_LED_WLAN, LOW);
        digitalWrite(PIN_LED_ROT, LOW);
        digitalWrite(PIN_LED_GELB, LOW);
        digitalWrite(PIN_SUMMER_1, LOW);
        digitalWrite(PIN_SUMMER_2, LOW);
    }

    void wlanLed(bool an) {
        digitalWrite(PIN_LED_WLAN, an ? HIGH : LOW);
    }

    void wlanLedToggle() {
        digitalWrite(PIN_LED_WLAN, !digitalRead(PIN_LED_WLAN));
    }

    // Alarm-Hardware: Abwechselnd Rot/Gelb + Summer im 200ms-Takt
    void alarmHardwareSetzen(bool phase) {
        digitalWrite(PIN_LED_ROT,  phase ? HIGH : LOW);
        digitalWrite(PIN_SUMMER_2, phase ? HIGH : LOW);
        digitalWrite(PIN_LED_GELB, phase ? LOW : HIGH);
        digitalWrite(PIN_SUMMER_1, phase ? HIGH : LOW);
    }

    void alarmHardwareAus() {
        digitalWrite(PIN_SUMMER_1, LOW);
        digitalWrite(PIN_SUMMER_2, LOW);
        digitalWrite(PIN_LED_ROT, LOW);
        digitalWrite(PIN_LED_GELB, LOW);
    }

    bool tasterGedrueckt() {
        return digitalRead(PIN_TASTER) == LOW;
    }

    // --- Watchdog ---

    Ticker watchdogTicker;
    volatile int watchdogZaehler = 0;
    volatile bool mussNeustarten = false;

    void ICACHE_RAM_ATTR watchdogISR() {
        watchdogZaehler++;
        if (watchdogZaehler >= WATCHDOG_TIMEOUT_SEK) mussNeustarten = true;
    }

    void watchdogStarten() {
        watchdogTicker.attach(1.0, watchdogISR);
    }

    void watchdogStoppen() {
        watchdogTicker.detach();
    }

    void watchdogFuettern() {
        watchdogZaehler = 0;
    }

    bool watchdogAusgeloest() {
        return mussNeustarten;
    }

    // --- System ---

    void init() {
        Serial.begin(9600);
        gpioInit();
    }

    void neustart() {
        delay(100);
        ESP.restart();
    }

    unsigned long zeitMs() { return millis(); }
    uint32_t freierHeap() { return ESP.getFreeHeap(); }
    String resetGrund() { return ESP.getResetReason(); }
    void cpuFreigeben() { yield(); }

    // --- Flash (LittleFS) ---

    bool flashInit() { return LittleFS.begin(); }
    void flashFormatieren() { LittleFS.format(); }

    // --- WiFi ---

    bool wlanVerbunden() { return WiFi.status() == WL_CONNECTED; }
    String wlanSsid() { return WiFi.SSID(); }
    int wlanRssi() { return WiFi.RSSI(); }
    String wlanIp() { return WiFi.localIP().toString(); }

    void wlanVerbinden(const char* ssid, const char* pw) { WiFi.begin(ssid, pw); }
    void wlanTrennen() { WiFi.disconnect(true); }

    int wlanScanStarten() { WiFi.scanNetworks(true); return 0; }
    int wlanScanErgebnis() { return WiFi.scanComplete(); }
    String wlanScanSsid(int i) { return WiFi.SSID(i); }
    int wlanScanRssi(int i) { return WiFi.RSSI(i); }
    void wlanScanLoeschen() { WiFi.scanDelete(); }

    void wlanCredentialsLoeschen() {
        WiFiManager wm;
        wm.resetSettings();
    }

    // --- UDP ---

    WiFiUDP udpSocket;
    const unsigned int LOKALER_PORT = 4210;

    // Zwischenspeicher fuer Sender-Adresse (vor strtok sichern!)
    IPAddress letzterSenderIp;
    unsigned int letzterSenderPort = 0;

    void udpStarten() { udpSocket.begin(LOKALER_PORT); }

    int udpPaketVerfuegbar() { return udpSocket.parsePacket(); }

    // Liest Paket und merkt sich Absender-Info fuer ACK
    int udpLesen(char* buf, size_t maxLen) {
        letzterSenderIp = udpSocket.remoteIP();
        letzterSenderPort = udpSocket.remotePort();
        return udpSocket.read(buf, maxLen);
    }

    void udpAntworten(const char* daten) {
        udpSocket.beginPacket(letzterSenderIp, letzterSenderPort);
        udpSocket.print(daten);
        udpSocket.endPacket();
    }

    void udpFlush() { udpSocket.flush(); }

    // --- mDNS ---

    bool mdnsStarten(const char* hostname) { return MDNS.begin(hostname); }
    void mdnsUpdate() { MDNS.update(); }

    // --- Telnet ---

    void telnetStarten() { TelnetStream.begin(); }
    bool telnetVerfuegbar() { return TelnetStream.available(); }
    String telnetLesen() { String s = TelnetStream.readStringUntil('\n'); s.trim(); return s; }
    void telnetSchreiben(const char* msg) { TelnetStream.println(msg); TelnetStream.flush(); }
    void telnetStoppen() { TelnetStream.stop(); }

} // namespace HAL

// ============================================================================
// SECURITY - Sicherheitsfunktionen (plattformunabhaengig)
// ============================================================================

namespace Security {

    // Constant-Time Vergleich gegen Timing-Seitenkanalangriffe
    bool sichererVergleich(const char* a, const char* b, size_t laenge) {
        volatile uint8_t ergebnis = 0;
        for (size_t i = 0; i < laenge; i++) {
            ergebnis |= (uint8_t)a[i] ^ (uint8_t)b[i];
        }
        return ergebnis == 0;
    }

    // Prueft ob String nur aus Dezimalziffern besteht
    bool nurZiffern(const char* str) {
        if (str == NULL || str[0] == '\0') return false;
        for (size_t i = 0; str[i] != '\0'; i++) {
            if (!isdigit((unsigned char)str[i])) return false;
        }
        return true;
    }

    // HMAC-SHA256 -> 64 Hex-Zeichen in hexOut (min. 65 Bytes)
    void berechneHMAC(const char* nachricht, size_t nachrichtLen,
                      const char* secret, size_t secretLen,
                      char* hexOut) {
        br_hmac_key_context kc;
        br_hmac_context ctx;
        br_hmac_key_init(&kc, &br_sha256_vtable, secret, secretLen);
        br_hmac_init(&ctx, &kc, 0);
        br_hmac_update(&ctx, nachricht, nachrichtLen);
        uint8_t result[32];
        br_hmac_out(&ctx, result);
        for (int i = 0; i < 32; i++)
            sprintf(hexOut + (i * 2), "%02x", result[i]);
        hexOut[64] = '\0';
    }

    // --- Sliding-Window Replay-Schutz ---

    unsigned long replayWindowBase = 0;
    uint32_t replayBitmap = 0;
    unsigned long letztePersistedSeq = 0;

    bool pruefeReplay(unsigned long seq) {
        // Fall 1: Neues Paket (vor dem Fenster)
        if (seq > replayWindowBase) {
            unsigned long shift = seq - replayWindowBase;
            if (shift >= REPLAY_FENSTER_GROESSE)
                replayBitmap = 0;
            else
                replayBitmap <<= shift;
            replayWindowBase = seq;
            replayBitmap |= 1;
            return true;
        }
        // Fall 2: Innerhalb des Fensters
        unsigned long diff = replayWindowBase - seq;
        if (diff >= REPLAY_FENSTER_GROESSE) return false; // Zu alt
        uint32_t maske = 1UL << diff;
        if (replayBitmap & maske) return false; // Bereits gesehen
        replayBitmap |= maske;
        return true;
    }

    // Flash-Wear-Schutz: Nur alle N Inkremente schreiben
    void speichereSequenz() {
        if (replayWindowBase - letztePersistedSeq < SEQ_PERSIST_SCHWELLE) return;
        File f = LittleFS.open("/seq.dat", "w");
        if (f) {
            char buf[12];
            snprintf(buf, sizeof(buf), "%lu", replayWindowBase);
            f.print(buf);
            f.close();
            letztePersistedSeq = replayWindowBase;
        }
    }

    void ladeSequenz() {
        if (!LittleFS.exists("/seq.dat")) return;
        File f = LittleFS.open("/seq.dat", "r");
        if (f) {
            char buf[12];
            size_t len = f.readBytes(buf, sizeof(buf) - 1);
            buf[len] = '\0';
            f.close();
            if (nurZiffern(buf)) {
                replayWindowBase = strtoul(buf, NULL, 10);
                letztePersistedSeq = replayWindowBase;
            }
        }
    }

} // namespace Security

// ============================================================================
// GLOBALER ZUSTAND
// ============================================================================

SystemKonfiguration config;
SystemZustand aktuellerZustand = ZUSTAND_INIT;

// Flags
bool konfigurationSpeichern = false;
bool telnetAutorisiert = false;
bool darfLoggen = false;

// Alarm
volatile bool alarmAktiv = false;
bool ledPhase = false;
unsigned long letzterToggle = 0;

// Timing
unsigned long letzterVerbindungsVersuch = 0;
unsigned long letzterScanStart = 0;
unsigned long letzterHeartbeat = 0;
unsigned long letzterTelnetInput = 0;
unsigned long letztesBlinken = 0;
unsigned long aktuellesHeartbeatIntervall = 2000;
int scanStatus = -1;

// Sicherheit
uint8_t telnetFehlversuche = 0;
unsigned long telnetSperreBis = 0;
uint8_t udpPaketZaehler = 0;
unsigned long udpZaehlerReset = 0;

// ============================================================================
// CONFIG PERSISTENZ
// ============================================================================

void konfigurationSpeichernCallback() {
    konfigurationSpeichern = true;
}

void ladeKonfiguration() {
    if (LittleFS.begin()) {
        if (LittleFS.exists("/config.json")) {
            File datei = LittleFS.open("/config.json", "r");
            if (datei) {
                DynamicJsonDocument doc(1024);
                DeserializationError fehler = deserializeJson(doc, datei);
                if (!fehler) {
                    strlcpy(config.udpToken, doc["token"] | "", sizeof(config.udpToken));
                    strlcpy(config.mdnsName, doc["name"] | "", sizeof(config.mdnsName));
                    strlcpy(config.telnetPasswort, doc["tpass"] | "", sizeof(config.telnetPasswort));
                    strlcpy(config.backupSsid, doc["bssid"] | "", sizeof(config.backupSsid));
                    strlcpy(config.backupPasswort, doc["bpass"] | "", sizeof(config.backupPasswort));
                    strlcpy(config.hauptWlanName, doc["hssid"] | "", sizeof(config.hauptWlanName));
                    strlcpy(config.hauptWlanPasswort, doc["hpass"] | "", sizeof(config.hauptWlanPasswort));
                    strlcpy(config.apPasswort, doc["appw"] | "", sizeof(config.apPasswort));
                    strlcpy(config.apiServer, doc["apiip"] | "", sizeof(config.apiServer));
                    strlcpy(config.apiToken, doc["apitoken"] | "", sizeof(config.apiToken));
                }
            }
        }
    }
}

void speichereKonfiguration() {
    DynamicJsonDocument doc(1024);
    doc["token"] = config.udpToken;
    doc["name"] = config.mdnsName;
    doc["tpass"] = config.telnetPasswort;
    doc["bssid"] = config.backupSsid;
    doc["bpass"] = config.backupPasswort;
    doc["hssid"] = config.hauptWlanName;
    doc["hpass"] = config.hauptWlanPasswort;
    doc["appw"]  = config.apPasswort;
    doc["apiip"] = config.apiServer;
    doc["apitoken"] = config.apiToken; 
    File datei = LittleFS.open("/config.json", "w");
    if (datei) {
        serializeJson(doc, datei);
        datei.close();
    }
}

// ============================================================================
// LOGGING & PROTOKOLL
// ============================================================================

void sendeLogAnApi(const char* nachricht) {
    if (!darfLoggen || !HAL::wlanVerbunden()) return;
    // Im Alarm nur Alarm/UDP/Button-Logs durchlassen (kein Heartbeat-Spam)
    if (alarmAktiv && strncmp(nachricht, "ALARM", 5) != 0
                   && strncmp(nachricht, "UDP", 3) != 0
                   && strncmp(nachricht, "Btn", 3) != 0) return;

    WiFiClient client;
    client.setTimeout(1000);
    HTTPClient http;
    http.setTimeout(1000);
    String serverPath = "http://" + String(config.apiServer) + "/api.php";
    if (http.begin(client, serverPath)) {
        http.addHeader("Content-Type", "application/json");
        if (strlen(config.apiToken) > 0) {
            http.addHeader("Authorization", String("Bearer ") + config.apiToken);
            http.addHeader("X-ESP-Token", config.apiToken);
        }
        StaticJsonDocument<256> doc;
        doc["source"] = "receiver";
        doc["log"] = nachricht;
        String body;
        serializeJson(doc, body);
        http.POST(body);
        http.end();
    }
}

void sendeProtokoll(const char* nachricht) {
    Serial.println(nachricht);
    if (HAL::wlanVerbunden() && telnetAutorisiert)
        HAL::telnetSchreiben(nachricht);
    sendeLogAnApi(nachricht);
}

// ============================================================================
// SERVICE-FUNKTIONEN
// ============================================================================

// --- UDP-Empfang und Validierung ---
// Rueckgabe: 1 = ALARM_ON, -1 = ALARM_OFF, 0 = nichts/ungueltig
int verarbeiteUdpEmpfang() {
    int paketGroesse = HAL::udpPaketVerfuegbar();
    if (!paketGroesse) return 0;

    // DoS-Schutz: Rate Limiting
    if (udpPaketZaehler >= UDP_MAX_PAKETE_PRO_MINUTE) {
        if (HAL::zeitMs() - udpZaehlerReset > 60000) {
            udpPaketZaehler = 0;
            udpZaehlerReset = HAL::zeitMs();
        } else {
            HAL::udpFlush();
            return 0;
        }
    }
    udpPaketZaehler++;

    // char-Array basiertes Parsing (kein Arduino String auf dem Hot-Path)
    char puffer[512];
    int laenge = HAL::udpLesen(puffer, sizeof(puffer) - 1);
    if (laenge <= 0) return 0;
    puffer[laenge] = '\0';

    // Whitespace trimmen
    while (laenge > 0 && (puffer[laenge-1] == '\n' || puffer[laenge-1] == '\r' || puffer[laenge-1] == ' '))
        puffer[--laenge] = '\0';

    // Arbeitskopie fuer strtok (destruktiv)
    char arbeitskopie[512];
    strlcpy(arbeitskopie, puffer, sizeof(arbeitskopie));

    // Format: "BEFEHL:SEQUENZNUMMER:HMAC_SIGNATUR"
    char* befehl = strtok(arbeitskopie, ":");
    char* seqString = strtok(NULL, ":");
    char* empfangeneSignatur = strtok(NULL, ":");

    if (befehl == NULL || seqString == NULL || empfangeneSignatur == NULL) return 0;
    if (strtok(NULL, ":") != NULL) return 0; // Keine Extra-Felder

    // isdigit-Validierung
    if (!Security::nurZiffern(seqString)) return 0;

    // HMAC berechnen und vergleichen
    char payload[256];
    int payloadLen = snprintf(payload, sizeof(payload), "%s:%s", befehl, seqString);
    if (payloadLen < 0 || (size_t)payloadLen >= sizeof(payload)) return 0;

    char berechneteSignatur[65];
    Security::berechneHMAC(payload, (size_t)payloadLen,
                           config.udpToken, strlen(config.udpToken),
                           berechneteSignatur);

    // Signaturlaenge pruefen
    size_t sigLen = strlen(empfangeneSignatur);
    if (sigLen != 64) return 0;

    // Lowercase normalisieren
    char sigLower[65];
    for (size_t i = 0; i < 64; i++)
        sigLower[i] = (char)tolower((unsigned char)empfangeneSignatur[i]);
    sigLower[64] = '\0';

    // Constant-Time Vergleich
    if (!Security::sichererVergleich(berechneteSignatur, sigLower, 64))
        return 0; // Falsche Signatur -> stillschweigend ignorieren

    // Replay-Window pruefen
    unsigned long empfangeneSeq = strtoul(seqString, NULL, 10);
    if (!Security::pruefeReplay(empfangeneSeq)) return 0;

    // Sequenz periodisch in Flash sichern
    Security::speichereSequenz();

    // ACK senden
    char ack[32];
    snprintf(ack, sizeof(ack), "ACK_SECURE:%s", seqString);

    if (strcmp(befehl, CMD_ALARM_AN) == 0) {
        if (!alarmAktiv) sendeProtokoll("ALARM ON (UDP)");
        HAL::udpAntworten(ack);
        return 1;
    }
    else if (strcmp(befehl, CMD_ALARM_AUS) == 0) {
        if (alarmAktiv) sendeProtokoll("ALARM OFF (UDP)");
        HAL::udpAntworten(ack);
        return -1;
    }

    return 0; // Unbekannter Befehl
}

// --- Alarm-Hardware aktualisieren ---
void aktualisiereAlarmHardware() {
    if (alarmAktiv) {
        if (HAL::zeitMs() - letzterToggle >= ALARM_TOGGLE_INTERVALL) {
            letzterToggle = HAL::zeitMs();
            ledPhase = !ledPhase;
            HAL::alarmHardwareSetzen(ledPhase);
        }
    } else {
        if (ledPhase) {
            HAL::alarmHardwareAus();
            ledPhase = false;
        }
    }
}

// --- Taster (Toggle + Reset) ---
// Rueckgabe: 1 = Toggle, 2 = Werksreset, 0 = nichts
int verarbeiteTaster() {
    static unsigned long druckStart = 0;

    if (HAL::tasterGedrueckt()) {
        if (druckStart == 0) druckStart = HAL::zeitMs();
        return 0;
    }

    if (druckStart == 0) return 0;

    unsigned long dauer = HAL::zeitMs() - druckStart;
    druckStart = 0;

    if (dauer > TASTER_RESET_DRUCK) return 2;        // Factory Reset
    if (dauer < TASTER_LANG_DRUCK) return 1;          // Toggle Alarm
    return 0;
}

// --- Heartbeat mit Server-Befehlen ---
void verarbeiteHeartbeat() {
    if (HAL::zeitMs() - letzterHeartbeat < aktuellesHeartbeatIntervall) return;
    if (!HAL::wlanVerbunden()) { letzterHeartbeat = HAL::zeitMs(); return; }

    WiFiClient client;
    client.setTimeout(1000);
    HTTPClient http;
    http.setTimeout(1000);
    String serverPath = "http://" + String(config.apiServer) + "/api.php";

    if (http.begin(client, serverPath)) {
        http.addHeader("Content-Type", "application/json");
        if (strlen(config.apiToken) > 0) {
            http.addHeader("Authorization", String("Bearer ") + config.apiToken);
            http.addHeader("X-ESP-Token", config.apiToken);
        }
        StaticJsonDocument<384> doc;
        doc["source"] = "receiver";
        doc["ip"] = HAL::wlanIp();
        doc["status_msg"] = alarmAktiv ? "ALARM" : "Bereit";
        doc["alarm_state"] = alarmAktiv;
        doc["rssi"] = HAL::wlanRssi();
        doc["heap"] = HAL::freierHeap();
        doc["uptime"] = millis() / 1000;              // NEU
        doc["reset_reason"] = ESP.getResetReason(); 

        String body;
        serializeJson(doc, body);
        int code = http.POST(body);

        if (code > 0) {
            aktuellesHeartbeatIntervall = HEARTBEAT_INTERVALL;

            if (code == 200) {
                String payload = http.getString();
                DynamicJsonDocument antwort(1024);
                deserializeJson(antwort, payload);

                if (!antwort.isNull()) {
                    if (antwort.containsKey("logging_active"))
                        darfLoggen = antwort["logging_active"];

                    // Remote-Konfigurationsupdate
                    if (antwort.containsKey("new_config")) {
                        JsonObject newConf = antwort["new_config"];
                        bool neustartNoetig = false;
                        if (newConf.containsKey("mssid")) {
                            strlcpy(config.hauptWlanName, newConf["mssid"] | "", sizeof(config.hauptWlanName));
                            neustartNoetig = true;
                        }
                        if (newConf.containsKey("mpass")) {
                            strlcpy(config.hauptWlanPasswort, newConf["mpass"] | "", sizeof(config.hauptWlanPasswort));
                            neustartNoetig = true;
                        }
                        if (newConf.containsKey("bssid")) {
                           strlcpy(config.backupSsid, newConf["bssid"] | "", sizeof(config.backupSsid));
                            neustartNoetig = true;
                        }
                        if (newConf.containsKey("bpass")) {
                         strlcpy(config.backupPasswort, newConf["bpass"] | "", sizeof(config.backupPasswort));
                            neustartNoetig = true;
                        }
                        if (newConf.containsKey("apiip")) {
                            strlcpy(config.apiServer, newConf["apiip"] | "", sizeof(config.apiServer));
                            neustartNoetig = true;
                        }
                        if (newConf.containsKey("tpass")) {
                            strlcpy(config.telnetPasswort, newConf["tpass"] | "", sizeof(config.telnetPasswort));
                            neustartNoetig = true;
                        }
                        speichereKonfiguration();
                        if (neustartNoetig) {
                            delay(500);
                            HAL::neustart();
                        }
                    }

                    // Remote-Befehle
                    if (antwort.containsKey("command")) {
                        String befehl = antwort["command"];
                        if (befehl == "REBOOT") { delay(500); HAL::neustart(); }
                        else if (befehl == "RESET") {
                            HAL::flashFormatieren();
                            HAL::wlanCredentialsLoeschen();
                            HAL::neustart();
                        }
                        else if (befehl == "ALARM_ON") { alarmAktiv = true; }
                        else if (befehl == "ALARM_OFF") { alarmAktiv = false; }
                    }
                }
            }
        } else {
            aktuellesHeartbeatIntervall = 60000;
        }
        http.end();
    } else {
        aktuellesHeartbeatIntervall = 60000;
    }
    letzterHeartbeat = HAL::zeitMs();
}

// --- WLAN-LED ---
void aktualisiereWlanLed() {
    if (HAL::wlanVerbunden()) {
        HAL::wlanLed(true);
    } else {
        if (HAL::zeitMs() - letztesBlinken >= BLINK_INTERVALL) {
            HAL::wlanLedToggle();
            letztesBlinken = HAL::zeitMs();
        }
    }
}

// --- WLAN Failover ---
void verwalteWlanVerbindung() {
    static int stabilitaetsZaehler = 0;

    if (HAL::wlanVerbunden()) {
        String ssid = HAL::wlanSsid();
        if (ssid == String(config.backupSsid) && strlen(config.backupSsid) > 0) {
            if (HAL::zeitMs() - letzterScanStart > WLAN_SCAN_INTERVALL && scanStatus == -1) {
                letzterScanStart = HAL::zeitMs();
                HAL::wlanScanStarten();
                scanStatus = 0;
            }
            if (scanStatus == 0) {
                int n = HAL::wlanScanErgebnis();
                if (n >= 0) {
                    bool gefunden = false;
                    for (int i = 0; i < n; i++) {
                        if (HAL::wlanScanSsid(i) == String(config.hauptWlanName) &&
                            String(config.hauptWlanName).length() > 0 &&
                            HAL::wlanScanRssi(i) > RSSI_SCHWELLWERT) {
                            gefunden = true;
                            break;
                        }
                    }
                    if (gefunden) {
                        stabilitaetsZaehler++;
                        if (stabilitaetsZaehler >= STABILITAETS_SCHWELLWERT) {
                            HAL::watchdogStoppen();
                            for (int k = 0; k < 10; k++) { HAL::wlanLedToggle(); delay(50); }
                            if (strlen(config.hauptWlanPasswort) > 0)
                                HAL::wlanVerbinden(config.hauptWlanName, config.hauptWlanPasswort);
                            else
                                HAL::neustart();
                        }
                    } else {
                        stabilitaetsZaehler = 0;
                    }
                    HAL::wlanScanLoeschen();
                    scanStatus = -1;
                }
            }
        } else {
            stabilitaetsZaehler = 0;
        }
    } else {
        if (HAL::zeitMs() - letzterVerbindungsVersuch > WLAN_WIEDERHOLUNGS_INTERVALL) {
            if (strlen(config.backupSsid) > 0) {
                HAL::wlanTrennen();
                unsigned long start = HAL::zeitMs();
                while (HAL::zeitMs() - start < 500) { HAL::watchdogFuettern(); HAL::cpuFreigeben(); }
                HAL::wlanVerbinden(config.backupSsid, config.backupPasswort);
                letzterVerbindungsVersuch = HAL::zeitMs();
                letzterScanStart = HAL::zeitMs();
                stabilitaetsZaehler = 0;
            } else {
                letzterVerbindungsVersuch = HAL::zeitMs();
            }
        }
    }
}

// --- Telnet ---
void pruefeTelnetZugang() {
    if (HAL::zeitMs() < telnetSperreBis) return;

    if (!HAL::telnetVerfuegbar()) return;

    String eingabe = HAL::telnetLesen();
    if (eingabe.length() == 0) return;

    if (eingabe == String(config.telnetPasswort)) {
        telnetAutorisiert = true;
        telnetFehlversuche = 0;
        HAL::telnetSchreiben("LOGIN OK");
    }
    else if (eingabe.equalsIgnoreCase("up up down down left right left right b a")) {
        HAL::telnetSchreiben("\n>> CHEAT CODE DETECTED <<");
        HAL::telnetSchreiben("   GOD MODE: [ACTIVATED]");
        HAL::telnetSchreiben("   UNLIMITED AMMO: [TRUE]");
    }
    else {
        telnetFehlversuche++;
        if (telnetFehlversuche >= MAX_TELNET_VERSUCHE) {
            telnetSperreBis = HAL::zeitMs() + 300000;
            HAL::telnetStoppen();
        } else {
            HAL::telnetSchreiben("Wrong PW");
        }
    }
}

// ============================================================================
// FINITE STATE MACHINE
// ============================================================================

void fsmInit() {
    HAL::init();
    HAL::flashInit();
    // Verschleierte Defaults entschluesseln (einmalig beim Start)
    // Falls config.json existiert, werden diese sowieso ueberschrieben
    deobfuscate(config.apPasswort);
    deobfuscate(config.telnetPasswort);   
    ladeKonfiguration();
    Security::ladeSequenz();
    aktuellerZustand = ZUSTAND_WLAN_VERBINDEN;
}

void fsmWlanVerbinden() {
    WiFiManager wm;
    wm.setSaveConfigCallback(konfigurationSpeichernCallback);

    WiFiManagerParameter p_tpass("tpass", "Telnet PW", config.telnetPasswort, 20);
    WiFiManagerParameter p_token("token", "HMAC Secret", config.udpToken, 40);
    WiFiManagerParameter p_name("name", "mDNS Name", config.mdnsName, 32);
    WiFiManagerParameter p_api("apiip", "API Server IP", config.apiServer, 32);
    WiFiManagerParameter p_bssid("bssid", "Backup SSID", config.backupSsid, 32);
    WiFiManagerParameter p_bpass("bpass", "Backup PW", config.backupPasswort, 64);
    WiFiManagerParameter p_hssid("hssid", "Haupt SSID", config.hauptWlanName, 32);
    WiFiManagerParameter p_hpass("hpass", "Haupt PW", config.hauptWlanPasswort, 64);
    WiFiManagerParameter p_appw("appw", "AP Passwort", config.apPasswort, 64);
    WiFiManagerParameter p_apitoken("apitoken", "API Token", config.apiToken, 32);

    wm.addParameter(&p_tpass);
    wm.addParameter(&p_token);
    wm.addParameter(&p_name);
    wm.addParameter(&p_api);
    wm.addParameter(&p_bssid);
    wm.addParameter(&p_bpass);
    wm.addParameter(&p_hssid);
    wm.addParameter(&p_hpass);
    wm.addParameter(&p_appw);
    wm.addParameter(&p_apitoken);

    wm.setClass("invert");
    wm.setConfigPortalTimeout(180);
    wm.setConnectTimeout(30);

    // Hauptnetz direkt versuchen
    if (strlen(config.hauptWlanName) > 0)
        HAL::wlanVerbinden(config.hauptWlanName, config.hauptWlanPasswort);

    if (!HAL::wlanVerbunden())
        wm.autoConnect("Alarm-Empfaenger-SETUP");

    // Parameter uebernehmen
    strlcpy(config.udpToken, p_token.getValue(), sizeof(config.udpToken));
    strlcpy(config.telnetPasswort, p_tpass.getValue(), sizeof(config.telnetPasswort));
    strlcpy(config.mdnsName, p_name.getValue(), sizeof(config.mdnsName));
    strlcpy(config.apiServer, p_api.getValue(), sizeof(config.apiServer));
    strlcpy(config.backupSsid, p_bssid.getValue(), sizeof(config.backupSsid));
    strlcpy(config.backupPasswort, p_bpass.getValue(), sizeof(config.backupPasswort));
    strlcpy(config.hauptWlanName, p_hssid.getValue(), sizeof(config.hauptWlanName));
    strlcpy(config.hauptWlanPasswort, p_hpass.getValue(), sizeof(config.hauptWlanPasswort));
    strlcpy(config.apPasswort, p_appw.getValue(), sizeof(config.apPasswort));
    strlcpy(config.apiToken, p_apitoken.getValue(), sizeof(config.apiToken));

    if (konfigurationSpeichern || HAL::wlanVerbunden())
        speichereKonfiguration();

    // Dienste starten
    if (HAL::mdnsStarten(config.mdnsName))
        Serial.println("mDNS aktiv");
    HAL::telnetStarten();
    HAL::udpStarten();

    HAL::watchdogStarten();
    aktuellerZustand = ZUSTAND_BEREIT;
}

void fsmBereit() {
    // Zeitkritisch: UDP-Empfang
    int udpResult = verarbeiteUdpEmpfang();
    if (udpResult == 1) { alarmAktiv = true; aktuellerZustand = ZUSTAND_ALARM; return; }
    if (udpResult == -1) { alarmAktiv = false; }

    // Taster
    int taste = verarbeiteTaster();
    if (taste == 2) { aktuellerZustand = ZUSTAND_WERKSRESET; return; }
    if (taste == 1) {
        alarmAktiv = !alarmAktiv;
        sendeProtokoll("Btn Toggle");
        if (alarmAktiv) { aktuellerZustand = ZUSTAND_ALARM; return; }
    }

    // Alarm-Hardware aktualisieren (fuer den Fall: OFF-Zustand aufräumen)
    aktualisiereAlarmHardware();

    // Nicht-zeitkritische Netzwerk-Tasks
    verarbeiteHeartbeat();
    pruefeTelnetZugang();
    verwalteWlanVerbindung();
    aktualisiereWlanLed();

    if (HAL::wlanVerbunden())
        HAL::mdnsUpdate();
}

void fsmAlarm() {
    // PRIORITY MODE: Hardware-Steuerung hat Vorrang
    // Heartbeat laeuft weiter (fuer Web-Dashboard-Steuerung),
    // aber Telnet und WLAN-Scan bleiben pausiert.

    aktualisiereAlarmHardware();

    int udpResult = verarbeiteUdpEmpfang();
    if (udpResult == -1) {
        alarmAktiv = false;
        aktuellerZustand = ZUSTAND_BEREIT;
        return;
    }

    // Heartbeat: Ermoeglicht ALARM_OFF vom Web-Dashboard
    // Tradeoff: HTTP-Request kann ~1s blockieren (kurzes LED-Stottern)
    verarbeiteHeartbeat();

    // Alarm durch Server-Befehl deaktiviert?
    if (!alarmAktiv) {
        aktuellerZustand = ZUSTAND_BEREIT;
        return;
    }

    int taste = verarbeiteTaster();
    if (taste == 2) { aktuellerZustand = ZUSTAND_WERKSRESET; return; }
    if (taste == 1) {
        alarmAktiv = false;
        sendeProtokoll("Btn Toggle");
        aktuellerZustand = ZUSTAND_BEREIT;
        return;
    }

    // WLAN-LED auch im Alarm aktualisieren
    aktualisiereWlanLed();
}

void fsmWerksreset() {
    sendeProtokoll("RESET!");
    HAL::watchdogStoppen();
    HAL::flashFormatieren();
    HAL::wlanCredentialsLoeschen();
    HAL::neustart();
}

// Zentraler FSM-Dispatcher
void fsmUpdate() {
    switch (aktuellerZustand) {
        case ZUSTAND_INIT:           fsmInit();          break;
        case ZUSTAND_WLAN_VERBINDEN: fsmWlanVerbinden(); break;
        case ZUSTAND_BEREIT:         fsmBereit();        break;
        case ZUSTAND_ALARM:          fsmAlarm();         break;
        case ZUSTAND_WERKSRESET:     fsmWerksreset();    break;
    }
}

// ============================================================================
// ARDUINO ENTRY POINTS
// ============================================================================

void setup() {
    aktuellerZustand = ZUSTAND_INIT;
    fsmUpdate(); // INIT -> WLAN_VERBINDEN
    fsmUpdate(); // WLAN_VERBINDEN -> BEREIT
}

void loop() {
    HAL::watchdogFuettern();
    if (HAL::watchdogAusgeloest()) {
        Serial.println("WATCHDOG RESET!");
        HAL::neustart();
    }
    fsmUpdate();
    HAL::cpuFreigeben();
}
