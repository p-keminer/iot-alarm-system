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
 *           Constant-Time ACK-Vergleich, Traffic Obfuscation,
 *           Telnet Brute-Force-Schutz
 *
 * Hardware:   NodeMCU V2 (ESP8266)
 * Autor:      Philip Keminer
 * Version:    V12.0 (Sender - HAL/FSM, OTA entfernt)
 * Datum:      2026-02-10
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

const char* DEVICE_NAME = "sender";

// --- Timing ---
const unsigned long WLAN_WIEDERHOLUNGS_INTERVALL = 20000;
const unsigned long WLAN_SCAN_INTERVALL = 30000;
const uint8_t STABILITAETS_SCHWELLWERT = 3;
const int8_t RSSI_SCHWELLWERT = -75;
const unsigned long TASTER_RESET_DRUCK = 10000;
const unsigned long BLINK_INTERVALL = 500;
const int WATCHDOG_TIMEOUT_SEK = 30;
const unsigned long SENDE_WIEDERHOLUNGS_INTERVALL = 1000;
const int MAX_SENDE_VERSUCHE = 10;
const unsigned long HEARTBEAT_INTERVALL = 2000;
const unsigned long TELNET_TIMEOUT = 300000;
const unsigned long IP_UPDATE_INTERVALL = 60000;

// --- Sicherheit ---
const uint8_t MAX_TELNET_VERSUCHE = 3;

// --- Obfuscated Payloads (Muss mit Empfaenger uebereinstimmen!) ---
const char* CMD_ALARM_AN  = "NICE_TRY_WIRESHARK_USER";
const char* CMD_ALARM_AUS = "ENCRYPTION_IS_YOUR_FRIEND";

// --- Pins ---
#define PIN_RESET_TASTER D3
#define PIN_LED_ALARM LED_BUILTIN  // Invertiert: LOW = an, HIGH = aus
#define PIN_LED_WLAN D5

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
    char mdnsZiel[33] = "";
    char apiServer[33] = "";
    char telnetPasswort[21] = "y!Q#u_pPx_%L9gI";
    char backupSsid[33] = "";
    char backupPasswort[65] = "";
    char hauptWlanName[33] = "";
    char hauptWlanPasswort[65] = "";
    char apPasswort[65] = "y!Q#u_pPx_%L9gI";
    char apiToken[33] = "";
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

    void gpioInit() {
        pinMode(PIN_LED_ALARM, OUTPUT);
        digitalWrite(PIN_LED_ALARM, HIGH); // Invertiert: HIGH = aus
        pinMode(PIN_LED_WLAN, OUTPUT);
        digitalWrite(PIN_LED_WLAN, LOW);
        pinMode(PIN_RESET_TASTER, INPUT_PULLUP);
    }

    // Alarm-LED (invertiert: LOW = an, HIGH = aus)
    void alarmLed(bool an) {
        digitalWrite(PIN_LED_ALARM, an ? LOW : HIGH);
    }

    void wlanLed(bool an) {
        digitalWrite(PIN_LED_WLAN, an ? HIGH : LOW);
    }

    void wlanLedToggle() {
        digitalWrite(PIN_LED_WLAN, !digitalRead(PIN_LED_WLAN));
    }

    bool resetTasterGedrueckt() {
        return digitalRead(PIN_RESET_TASTER) == LOW;
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

    // Rate-Limited: Max alle 100ms (wie Original-Sender)
    void watchdogFuettern() {
        static unsigned long letzterHappen = 0;
        if (millis() - letzterHappen > 100) {
            watchdogZaehler = 0;
            letzterHappen = millis();
        }
    }

    bool watchdogAusgeloest() {
        return mussNeustarten;
    }

    // --- System ---

    void init() {
        delay(1000); // Warten bis Serial stabil
        Serial.begin(9600);
        Serial.setTimeout(1000);
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
    const unsigned int LOKALER_PORT = 4211;
    const unsigned int ZIEL_PORT = 4210;
    IPAddress zielIp;
    unsigned long letzteIpAktualisierung = 0;

    void udpStarten() { udpSocket.begin(LOKALER_PORT); }

    int udpPaketVerfuegbar() { return udpSocket.parsePacket(); }

    int udpLesen(char* buf, size_t maxLen) { return udpSocket.read(buf, maxLen); }

    void udpSenden(const char* daten) {
        udpSocket.beginPacket(zielIp, ZIEL_PORT);
        udpSocket.print(daten);
        udpSocket.endPacket();
    }

    // mDNS-Aufloesung der Empfaenger-IP
    void zielIpAktualisieren(const char* zielHostname) {
        if (zeitMs() - letzteIpAktualisierung < IP_UPDATE_INTERVALL &&
            zielIp.toString() != "0.0.0.0") return;
        WiFi.hostByName((String(zielHostname) + ".local").c_str(), zielIp);
        letzteIpAktualisierung = zeitMs();
        if (zielIp.toString() == "0.0.0.0")
            zielIp = IPAddress(255, 255, 255, 255); // Broadcast-Fallback
    }

    bool zielIpGueltig() { return zielIp.toString() != "0.0.0.0"; }

    // --- mDNS ---

    bool mdnsStarten(const char* hostname) { return MDNS.begin(hostname); }
    void mdnsUpdate() { MDNS.update(); }

    // --- Telnet ---

    void telnetStarten() { TelnetStream.begin(); }
    bool telnetVerfuegbar() { return TelnetStream.available(); }
    String telnetLesen() { String s = TelnetStream.readStringUntil('\n'); s.trim(); return s; }
    void telnetSchreiben(const char* msg) { TelnetStream.println(msg); TelnetStream.flush(); }
    void telnetStoppen() { TelnetStream.stop(); }

    // --- Seriell ---

    bool seriellVerfuegbar() { return Serial.available(); }

    size_t seriellLesen(char* buf, size_t maxLen) {
        size_t len = Serial.readBytesUntil('\n', buf, maxLen - 1);
        buf[len] = '\0';
        while (len > 0 && (buf[len-1] == '\r' || buf[len-1] == ' '))
            buf[--len] = '\0';
        return len;
    }

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

    // --- Sequenznummer (LittleFS-persistent, ueberlebt Reboot) ---

    unsigned long sequenceNumber = 0;

    void speichereSequenz() {
        File f = LittleFS.open("/seq.dat", "w");
        if (f) {
            char buf[12];
            snprintf(buf, sizeof(buf), "%lu", sequenceNumber);
            f.print(buf);
            f.close();
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
            if (nurZiffern(buf))
                sequenceNumber = strtoul(buf, NULL, 10);
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

// UDP-Transaktion (SENDEN-Zustand)
char ausstehendeNachricht[256] = "";
int wiederholungsZaehler = 0;
unsigned long letzterSendeZeitpunkt = 0;
bool letzterBefehlWarAlarmAn = false; // Fuer LED-Feedback nach ACK

// Timing
unsigned long letzterVerbindungsVersuch = 0;
unsigned long letzterScanStart = 0;
unsigned long letzterHeartbeat = 0;
unsigned long letzterTelnetInput = 0;
unsigned long letztesBlinken = 0;
unsigned long aktuellesHeartbeatIntervall = 2000;
int scanStatus = -1;

// Telnet Sicherheit
uint8_t telnetFehlversuche = 0;
unsigned long telnetSperreBis = 0;

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
                    strlcpy(config.mdnsZiel, doc["ziel"] | "", sizeof(config.mdnsZiel));
                    strlcpy(config.telnetPasswort, doc["tpass"] | "", sizeof(config.telnetPasswort));
                    strlcpy(config.backupSsid, doc["bssid"] | "", sizeof(config.backupSsid));
                    strlcpy(config.backupPasswort, doc["bpass"] | "", sizeof(config.backupPasswort));
                    strlcpy(config.hauptWlanName, doc["hssid"] | "", sizeof(config.hauptWlanName));
                    strlcpy(config.hauptWlanPasswort, doc["hpass"] | "", sizeof(config.hauptWlanPasswort));
                    strlcpy(config.apPasswort, doc["appw"] | "", sizeof(config.apPasswort));
                    strlcpy(config.apiServer, doc["api"] | "", sizeof(config.apiServer));
                    strlcpy(config.apiToken, doc["apitoken"] | "", sizeof(config.apiToken));
                }
            }
        }
    }
}

void speichereKonfiguration() {
    DynamicJsonDocument doc(1024);
    doc["token"] = config.udpToken;
    doc["ziel"] = config.mdnsZiel;
    doc["tpass"] = config.telnetPasswort;
    doc["bssid"] = config.backupSsid;
    doc["bpass"] = config.backupPasswort;
    doc["hssid"] = config.hauptWlanName;
    doc["hpass"] = config.hauptWlanPasswort;
    doc["api"] = config.apiServer;
    doc["appw"]  = config.apPasswort;
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

    WiFiClient client;
    client.setTimeout(2000);
    HTTPClient http;
    http.setTimeout(2000);
    String serverPath = "http://" + String(config.apiServer) + "/api.php";
    if (http.begin(client, serverPath)) {
        http.addHeader("Content-Type", "application/json");
        if (strlen(config.apiToken) > 0) {
            http.addHeader("Authorization", String("Bearer ") + config.apiToken);
            http.addHeader("X-ESP-Token", config.apiToken);
        }
        StaticJsonDocument<256> doc;
        doc["source"] = DEVICE_NAME;
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

// --- Signiertes UDP-Paket erstellen und senden ---
void starteUdpTransaktion(const char* klarBefehl) {
    Security::sequenceNumber++;
    Security::speichereSequenz();

    const char* echterBefehl = (strcmp(klarBefehl, "ALARM_ON") == 0) ? CMD_ALARM_AN : CMD_ALARM_AUS;
    letzterBefehlWarAlarmAn = (strcmp(klarBefehl, "ALARM_ON") == 0);

    // Payload: "OBFUSCATED_CMD:SEQ"
    char payload[128];
    int pLen = snprintf(payload, sizeof(payload), "%s:%lu", echterBefehl, Security::sequenceNumber);
    if (pLen < 0 || (size_t)pLen >= sizeof(payload)) return;

    // HMAC berechnen
    char signatur[65];
    Security::berechneHMAC(payload, (size_t)pLen, config.udpToken, strlen(config.udpToken), signatur);

    // Komplett: "OBFUSCATED_CMD:SEQ:HMAC"
    snprintf(ausstehendeNachricht, sizeof(ausstehendeNachricht), "%s:%s", payload, signatur);

    wiederholungsZaehler = 0;
    letzterSendeZeitpunkt = HAL::zeitMs();

    HAL::zielIpAktualisieren(config.mdnsZiel);
    HAL::udpSenden(ausstehendeNachricht);

    aktuellerZustand = ZUSTAND_SENDEN;
}

// --- UDP-ACK pruefen (SENDEN-Zustand) ---
// Rueckgabe: 1 = ACK empfangen, 0 = noch wartend, -1 = Timeout
int pruefeUdpAntwort() {
    int paketGroesse = HAL::udpPaketVerfuegbar();

    if (paketGroesse) {
        char puffer[255];
        int laenge = HAL::udpLesen(puffer, sizeof(puffer) - 1);
        if (laenge <= 0) return 0;
        puffer[laenge] = '\0';

        // Whitespace trimmen
        while (laenge > 0 && (puffer[laenge-1] == '\n' || puffer[laenge-1] == '\r' || puffer[laenge-1] == ' '))
            puffer[--laenge] = '\0';

        // Erwartetes ACK: "ACK_SECURE:SEQUENZNUMMER"
        char erwartet[32];
        int erwLen = snprintf(erwartet, sizeof(erwartet), "ACK_SECURE:%lu", Security::sequenceNumber);
        if (erwLen < 0 || (size_t)erwLen >= sizeof(erwartet)) return 0;

        if ((size_t)laenge == (size_t)erwLen &&
            Security::sichererVergleich(puffer, erwartet, (size_t)erwLen)) {
            return 1;
        }
    }

    // Retry-Logik
    if (wiederholungsZaehler < MAX_SENDE_VERSUCHE &&
        HAL::zeitMs() - letzterSendeZeitpunkt >= SENDE_WIEDERHOLUNGS_INTERVALL) {
        wiederholungsZaehler++;
        char msg[48];
        snprintf(msg, sizeof(msg), "Wiederholung %d", wiederholungsZaehler);
        sendeProtokoll(msg);

        if (HAL::zielIpGueltig())
            HAL::udpSenden(ausstehendeNachricht);
        letzterSendeZeitpunkt = HAL::zeitMs();
    }

    if (wiederholungsZaehler >= MAX_SENDE_VERSUCHE)
        return -1;

    return 0;
}

// --- Heartbeat (nur im BEREIT-Zustand) ---
void verarbeiteHeartbeat() {
    if (HAL::zeitMs() - letzterHeartbeat < aktuellesHeartbeatIntervall) return;
    if (!HAL::wlanVerbunden()) { letzterHeartbeat = HAL::zeitMs(); return; }

    WiFiClient client;
    client.setTimeout(2000);
    HTTPClient http;
    http.setTimeout(2000);
    String serverPath = "http://" + String(config.apiServer) + "/api.php";

    if (http.begin(client, serverPath)) {
        http.addHeader("Content-Type", "application/json");
        if (strlen(config.apiToken) > 0) {
            http.addHeader("Authorization", String("Bearer ") + config.apiToken);
            http.addHeader("X-ESP-Token", config.apiToken);
        }
        StaticJsonDocument<512> doc;
        doc["source"] = DEVICE_NAME;
        doc["ip"] = HAL::wlanIp();
        doc["status_msg"] = (aktuellerZustand == ZUSTAND_SENDEN) ? "Sende Cmd..." : "Bereit";
        doc["rssi"] = HAL::wlanRssi();
        doc["heap"] = HAL::freierHeap();
        doc["uptime"] = millis() / 1000;   
        doc["reset_reason"] = HAL::resetGrund();
        if (ausstehendeNachricht[0] != '\0')
            doc["log"] = String("Pending: ") + ausstehendeNachricht;
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

// --- Serielle Befehle ---
// Rueckgabe: true wenn Transaktion gestartet wurde
bool verarbeiteSerielleBefehle() {
    if (!HAL::seriellVerfuegbar()) return false;

    char befehl[32];
    HAL::seriellLesen(befehl, sizeof(befehl));

    if (strcmp(befehl, "ALARM_ON") == 0 || strcmp(befehl, "ALARM_OFF") == 0) {
        starteUdpTransaktion(befehl);
        return true;
    }
    return false;
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

    // Auto-Logout nach 5 Minuten Inaktivitaet
    if (telnetAutorisiert && (HAL::zeitMs() - letzterTelnetInput > TELNET_TIMEOUT)) {
        telnetAutorisiert = false;
        HAL::telnetSchreiben("\n--- AUTO LOGOUT ---");
    }

    if (!HAL::telnetVerfuegbar()) return;
    letzterTelnetInput = HAL::zeitMs();

    String eingabe = HAL::telnetLesen();
    if (eingabe.length() == 0) return;

    if (eingabe == String(config.telnetPasswort)) {
        telnetAutorisiert = true;
        telnetFehlversuche = 0;
        HAL::telnetSchreiben("LOGIN OK");
    }
    else if (eingabe.equalsIgnoreCase("up up down down left right left right b a")) {
        HAL::telnetSchreiben("\n>> CHEAT CODE DETECTED <<");
        HAL::telnetSchreiben("   GOD MODE: [FAKE ENABLED]");
    }
    else if (eingabe == "logout") {
        telnetAutorisiert = false;
        HAL::telnetSchreiben("Ausgeloggt.");
    }
    else {
        telnetFehlversuche++;
        if (telnetFehlversuche >= MAX_TELNET_VERSUCHE) {
            telnetSperreBis = HAL::zeitMs() + 300000;
            HAL::telnetStoppen();
        } else {
            HAL::telnetSchreiben("Falsches PW");
        }
    }
}

// --- Hardware-Reset-Taster ---
// Rueckgabe: true wenn Reset ausgeloest wurde
bool pruefePhysischenReset() {
    static unsigned long druckStart = 0;

    if (HAL::resetTasterGedrueckt()) {
        if (druckStart == 0) druckStart = HAL::zeitMs();
        // Visuelles Feedback: LED blinkt waehrend gedrueckt
        if (HAL::zeitMs() % 200 < 100) HAL::alarmLed(true);
        else HAL::alarmLed(false);
        return false;
    }

    if (druckStart == 0) return false;

    unsigned long dauer = HAL::zeitMs() - druckStart;
    HAL::alarmLed(false);
    druckStart = 0;

    if (dauer > TASTER_RESET_DRUCK) return true;
    return false;
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

    WiFiManagerParameter p_api("api", "API Server IP", config.apiServer, 32);
    WiFiManagerParameter p_token("token", "HMAC Secret", config.udpToken, 40);
    WiFiManagerParameter p_tpass("tpass", "Telnet PW", config.telnetPasswort, 20);
    WiFiManagerParameter p_ziel("ziel", "Empfaenger mDNS", config.mdnsZiel, 32);
    WiFiManagerParameter p_bssid("bssid", "Backup SSID", config.backupSsid, 32);
    WiFiManagerParameter p_bpass("bpass", "Backup PW", config.backupPasswort, 64);
    WiFiManagerParameter p_hssid("hssid", "Haupt SSID", config.hauptWlanName, 32);
    WiFiManagerParameter p_hpass("hpass", "Haupt PW", config.hauptWlanPasswort, 64);
    WiFiManagerParameter p_appw("appw", "AP Passwort", config.apPasswort, 64);
    WiFiManagerParameter p_apitoken("apitoken", "API Token", config.apiToken, 32);

    wm.addParameter(&p_api);
    wm.addParameter(&p_token);
    wm.addParameter(&p_tpass);
    wm.addParameter(&p_ziel);
    wm.addParameter(&p_bssid);
    wm.addParameter(&p_bpass);
    wm.addParameter(&p_hssid);
    wm.addParameter(&p_hpass);
    wm.addParameter(&p_appw);
    wm.addParameter(&p_apitoken); 

    wm.setClass("invert");
    wm.setConfigPortalTimeout(180);
    wm.setConnectTimeout(30);

    Serial.println("Starte WiFiManager...");

    // Hauptnetz direkt versuchen wenn konfiguriert
    if (strlen(config.hauptWlanName) > 0 && strlen(config.hauptWlanPasswort) > 0) {
        HAL::wlanVerbinden(config.hauptWlanName, config.hauptWlanPasswort);
        int i = 0;
        while (i < 20 && !HAL::wlanVerbunden()) { delay(500); i++; Serial.print("."); }
    }

    // Wenn nicht verbunden: Captive Portal starten
    if (!HAL::wlanVerbunden()) {
        bool erfolg;
        if (strlen(config.apPasswort) >= 8)
            erfolg = wm.autoConnect("Alarm-Sender-Konfig", config.apPasswort);
        else
            erfolg = wm.autoConnect("Alarm-Sender-SETUP-OPEN");
        if (!erfolg) Serial.println("Offline Start...");
    }

    // Parameter uebernehmen
    strlcpy(config.apiServer, p_api.getValue(), sizeof(config.apiServer));
    strlcpy(config.udpToken, p_token.getValue(), sizeof(config.udpToken));
    strlcpy(config.telnetPasswort, p_tpass.getValue(), sizeof(config.telnetPasswort));
    strlcpy(config.mdnsZiel, p_ziel.getValue(), sizeof(config.mdnsZiel));
    strlcpy(config.backupSsid, p_bssid.getValue(), sizeof(config.backupSsid));
    strlcpy(config.backupPasswort, p_bpass.getValue(), sizeof(config.backupPasswort));
    strlcpy(config.hauptWlanName, p_hssid.getValue(), sizeof(config.hauptWlanName));
    strlcpy(config.hauptWlanPasswort, p_hpass.getValue(), sizeof(config.hauptWlanPasswort));
    strlcpy(config.apPasswort, p_appw.getValue(), sizeof(config.apPasswort));
    strlcpy(config.apiToken, p_apitoken.getValue(), sizeof(config.apiToken));

    if (konfigurationSpeichern || HAL::wlanVerbunden())
        speichereKonfiguration();

    // Dienste starten
    if (HAL::mdnsStarten(DEVICE_NAME))
        Serial.println("mDNS gestartet");
    HAL::telnetStarten();
    HAL::udpStarten();
    HAL::zielIpAktualisieren(config.mdnsZiel);

    if (HAL::wlanVerbunden()) {
        darfLoggen = true;
        letzterHeartbeat = 0;
        verarbeiteHeartbeat();
        sendeLogAnApi("System erfolgreich gestartet!");
    }

    HAL::watchdogStarten();
    Serial.println("Watchdog aktiv. Loop beginnt.");
    aktuellerZustand = ZUSTAND_BEREIT;
}

void fsmBereit() {
    // Serielle Befehle -> koennen Transaktion starten
    if (verarbeiteSerielleBefehle()) return; // Zustand schon auf SENDEN

    // Reset-Taster
    if (pruefePhysischenReset()) {
        aktuellerZustand = ZUSTAND_WERKSRESET;
        return;
    }

    // Nicht-zeitkritische Tasks
    HAL::zielIpAktualisieren(config.mdnsZiel);
    verarbeiteHeartbeat();
    pruefeTelnetZugang();
    verwalteWlanVerbindung();
    aktualisiereWlanLed();

    if (HAL::wlanVerbunden())
        HAL::mdnsUpdate();
}

void fsmSenden() {
    // PRIORITY MODE: Nur UDP-Transaktion + Reset-Taster
    // Kein Heartbeat, kein WLAN-Scan (wuerde ACK verzoegern)

    int result = pruefeUdpAntwort();

    if (result == 1) {
        // ACK empfangen
        sendeProtokoll("Erfolg: Validiertes ACK erhalten!");
        HAL::alarmLed(letzterBefehlWarAlarmAn);
        ausstehendeNachricht[0] = '\0';
        aktuellerZustand = ZUSTAND_BEREIT;
        return;
    }

    if (result == -1) {
        // Timeout
        sendeProtokoll("FEHLER: Timeout - Empfaenger antwortet nicht!");
        ausstehendeNachricht[0] = '\0';
        aktuellerZustand = ZUSTAND_BEREIT;
        return;
    }

    // Reset-Taster auch im SENDEN-Zustand
    if (pruefePhysischenReset()) {
        aktuellerZustand = ZUSTAND_WERKSRESET;
        return;
    }

    aktualisiereWlanLed();
}

void fsmWerksreset() {
    sendeProtokoll("!!! HARDWARE RESET (SAFE) !!!");
    HAL::watchdogStoppen();
    HAL::alarmLed(true);
    delay(2000);
    HAL::alarmLed(false);
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
        case ZUSTAND_SENDEN:         fsmSenden();        break;
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
