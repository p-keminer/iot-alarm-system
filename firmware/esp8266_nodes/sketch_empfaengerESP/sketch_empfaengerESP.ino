/*
 * PROJEKT: ESP8266 UDP Alarm-System (Empfänger)
 * ---------------------------------------------
 * Beschreibung: Empfängt UDP-Pakete, steuert Alarm-Peripherie (LEDs/Summer).
 * Beinhaltet Failover-WLAN, OTA-Updates und Watchdog-Schutz.
 * * Features:
 * - Sicherheit: Token-Auth, Brute-Force-Schutz, DoS-Rate-Limiting
 * - Stabilität: Software-Watchdog, Non-Blocking WiFi-Reconnect
 * - Wartung:    OTA-Updates, Telnet-Debugging, Config-Persistenz (Struct)
 * * Hardware:   NodeMCU V2 Amica (ESP8266 12E) 
 * Autor:    Philip Keminer
 * Version:  First Final German Clean Code
 * Datum:    2026-01-29
 */

#include <FS.h>             // Flash-Speicher Zugriff
#include <LittleFS.h>       // Dateisystem (Nachfolger von SPIFFS)
#include <ArduinoJson.h>    // JSON Parser/Serializer
#include <ESP8266WiFi.h>    // WLAN Core
#include <WiFiManager.h>    // Captive Portal für Initialeinrichtung
#include <WiFiUdp.h>        // UDP Netzwerkprotokoll
#include <ESP8266mDNS.h>    // Lokale Namensauflösung (.local)
#include <ArduinoOTA.h>     // Over-The-Air Updates
#include <TelnetStream.h>   // Remote Logging via Netzwerk
#include <Ticker.h>         // Hardware-Timer Interrupts

// --- KONSTANTEN ---
const unsigned long ALARM_TOGGLE_INTERVALL = 200; // Blink-Geschwindigkeit bei Alarm
const unsigned long WLAN_WIEDERHOLUNGS_INTERVALL = 20000; // 20s warten bei Verbindungsverlust
const unsigned long WLAN_SCAN_INTERVALL = 30000;          // 30s Intervall für Hintergrund-Scans
const uint8_t STABILITAETS_SCHWELLWERT = 3;               // Benötigte positive Scans vor Netzwechsel
const int8_t RSSI_SCHWELLWERT = -75;                      // Min. Signalstärke für Hauptnetz (dBm)
const unsigned long TASTER_LANG_DRUCK = 1000;             // 1s für Manuell-Modus
const unsigned long TASTER_RESET_DRUCK = 10000;           // 10s für Factory Reset
const unsigned long STATUS_DRUCK_INTERVALL = 5000;        // 5s Log-Intervall
const unsigned long BLINK_INTERVALL = 500;                // 500ms LED-Blinken

// --- SICHERHEIT ---
const uint8_t UDP_MAX_PAKETE_PRO_MINUTE = 30; // Rate-Limit (DoS-Schutz)
const uint8_t MAX_TELNET_VERSUCHE = 3;        // Max. Login-Versuche (Brute-Force)
const int WATCHDOG_TIMEOUT_SEK = 10;          // Zeit bis Hardware-Reset

// --- PIN MAPPING ---
#define PIN_LED_ROT D1
#define PIN_LED_GELB D2
#define PIN_LED_WLAN D3
#define PIN_SUMMER_1 D5
#define PIN_SUMMER_2 D6
#define PIN_TASTER D7

// --- KONFIGURATIONS-STRUCT ---
// Bündelt alle persistierten Einstellungen
struct SystemKonfiguration {
    char udpToken[41] = "";             // Authentifizierungs-Token
    char mdnsName[33] = "alarm-empfaenger";
    char telnetPasswort[21] = "";       // Telnet Passwort
    char backupSsid[33] = "";           // Fallback WLAN
    char backupPasswort[65] = "";
    char hauptWlanName[33] = "";        // Automatisch gelerntes Hauptnetz
    char apPasswort[65] = "12345678";   // AP Passwort (Default)
};

SystemKonfiguration config; // Globale Instanz

// --- STATUS VARIABLEN ---
bool konfigurationSpeichern = false;    // Flag für FS-Write
bool telnetAutorisiert = false;         // Login-Status
unsigned long letzterVerbindungsVersuch = 0;
unsigned long letzterScanStart = 0;
int scanStatus = -1;                    // Async Scan Status (-1: Idle)

// --- SYSTEM STATE ---
uint8_t telnetFehlversuche = 0;
unsigned long telnetSperreBis = 0;      // Zeitstempel für Login-Sperre
uint8_t udpPaketZaehler = 0;            // Zähler für Rate-Limit
unsigned long udpZaehlerReset = 0;      // Zeitfenster Rate-Limit
Ticker watchdogTicker;                  // Timer Instanz
volatile int watchdogZaehler = 0;       // Zähler (volatile für ISR)

WiFiUDP udp;
const unsigned int lokalerPort = 4210;

bool alarmAktiv = false;
bool ledStatus = false;                 // Toggle-Status für Blink-Animation
unsigned long letzterToggle = 0;
unsigned long letztesBlinken = 0;

// --- WATCHDOG ISR ---
// ICACHE_RAM_ATTR zwingt Code in RAM (wichtig für Interrupts)
void ICACHE_RAM_ATTR watchdogInterrupt() {
    watchdogZaehler++;
    if (watchdogZaehler >= WATCHDOG_TIMEOUT_SEK) {
        ESP.restart(); // Hard-Reset bei Hänger
    }
}

// --- LOGGING ---
void sendeProtokoll(const String &nachricht) {
  Serial.println(nachricht); // USB immer
  // Telnet nur wenn WLAN da + User eingeloggt
  if (WiFi.status() == WL_CONNECTED && telnetAutorisiert) {
    TelnetStream.println(nachricht);
    TelnetStream.flush();
  }
}

// --- CONFIG HANDLING ---
void konfigurationSpeichernCallback() { konfigurationSpeichern = true; }

void ladeKonfiguration() {
  if (LittleFS.begin()) { // FS Mounten
    if (LittleFS.exists("/config.json")) {
      File datei = LittleFS.open("/config.json", "r");
      if (datei) {
        StaticJsonDocument<512> doc; // Buffer im Stack
        DeserializationError fehler = deserializeJson(doc, datei);
        if (!fehler) {
          // Sicheres Kopieren mit Puffer-Limit (strlcpy)
          if(doc["token"]) strlcpy(config.udpToken, doc["token"], sizeof(config.udpToken));
          if(doc["name"])  strlcpy(config.mdnsName, doc["name"], sizeof(config.mdnsName));
          if(doc["tpass"]) strlcpy(config.telnetPasswort, doc["tpass"], sizeof(config.telnetPasswort));
          if(doc["bssid"]) strlcpy(config.backupSsid, doc["bssid"], sizeof(config.backupSsid));
          if(doc["bpass"]) strlcpy(config.backupPasswort, doc["bpass"], sizeof(config.backupPasswort));
          if(doc["hssid"]) strlcpy(config.hauptWlanName, doc["hssid"], sizeof(config.hauptWlanName)); 
          if(doc["appw"])  strlcpy(config.apPasswort, doc["appw"], sizeof(config.apPasswort));
        }
      }
    }
  }
}

void speichereKonfiguration() {
  StaticJsonDocument<512> doc;
  // Struct in JSON mappen
  doc["token"] = config.udpToken; doc["name"]  = config.mdnsName; doc["tpass"] = config.telnetPasswort;
  doc["bssid"] = config.backupSsid; doc["bpass"] = config.backupPasswort; doc["hssid"] = config.hauptWlanName; 
  doc["appw"]  = config.apPasswort;
  
  File datei = LittleFS.open("/config.json", "w");
  if (datei) { serializeJson(doc, datei); datei.close(); }
}

void starteOtaDienst() {
  ArduinoOTA.setPort(8266); 
  ArduinoOTA.setHostname(config.mdnsName); 
  ArduinoOTA.setPassword(config.telnetPasswort); // Sicherheit

  ArduinoOTA.onStart([]() { sendeProtokoll(">> OTA Start"); });
  ArduinoOTA.onEnd([]() { sendeProtokoll("\n>> OTA Ende"); });
  ArduinoOTA.onProgress([](unsigned int fortschritt, unsigned int gesamt) {
    // Fortschrittsanzeige (Reduzierte Log-Frequenz)
    static int letzterProzent = 0;
    int prozent = (fortschritt / (gesamt / 100));
    if (prozent >= letzterProzent + 10 || prozent == 100) {
        letzterProzent = prozent;
        sendeProtokoll(">> Upload: " + String(prozent) + "%");
    }
  });
  ArduinoOTA.onError([](ota_error_t fehler) { sendeProtokoll("!! OTA Fehler: " + String(fehler)); });
  ArduinoOTA.begin();
}

void aktualisiereWlanLed() {
    if (WiFi.status() == WL_CONNECTED) digitalWrite(PIN_LED_WLAN, HIGH); // Dauerlicht
    else { // Blinken bei Suche
        if (millis() - letztesBlinken >= BLINK_INTERVALL) { 
            digitalWrite(PIN_LED_WLAN, !digitalRead(PIN_LED_WLAN)); 
            letztesBlinken = millis(); 
        }
    }
}

// --- NETZWERK MANAGEMENT ---
void verwalteWlanVerbindung() {
    static int stabilitaetsZaehler = 0; 
    
    // Status: Verbunden
    if (WiFi.status() == WL_CONNECTED) {
        String aktuelleSsid = WiFi.SSID();
        // Prüfen: Sind wir im Backup-Netz?
        if (aktuelleSsid == String(config.backupSsid) && strlen(config.backupSsid) > 0) {
            // Scan Intervall prüfen + Status checken
            if (millis() - letzterScanStart > WLAN_SCAN_INTERVALL && scanStatus == -1) { 
                letzterScanStart = millis(); WiFi.scanNetworks(true); scanStatus = 0; // Async Scan starten
            }
            // Scan Ergebnisse verarbeiten
            if (scanStatus == 0) {
                int n = WiFi.scanComplete();
                if (n >= 0) {
                    bool hauptNetzGefunden = false;
                    for (int i = 0; i < n; i++) {
                        // Match Name & RSSI > Threshold
                        if (WiFi.SSID(i) == String(config.hauptWlanName) && String(config.hauptWlanName).length() > 0) {
                          if (WiFi.RSSI(i) > RSSI_SCHWELLWERT) { hauptNetzGefunden = true; break; }
                        }
                    }
                    if (hauptNetzGefunden) {
                        stabilitaetsZaehler++;
                        // Hysterese: 3x stabil -> Wechseln
                        if (stabilitaetsZaehler >= STABILITAETS_SCHWELLWERT) {
                            watchdogTicker.detach(); // WD aus, da Restart folgt
                            // Visuelles Feedback (blockierend ok hier)
                            for(int k=0; k<10; k++) { 
                                digitalWrite(PIN_LED_WLAN, !digitalRead(PIN_LED_WLAN)); 
                                delay(50); 
                            }
                            ESP.restart(); // Clean Reset zum Wechseln
                        }
                    } else { stabilitaetsZaehler = 0; }
                    WiFi.scanDelete(); scanStatus = -1; // Cleanup
                }
            }
        } else { stabilitaetsZaehler = 0; }
    } else {
        // Status: Verbindung verloren
        if (millis() - letzterVerbindungsVersuch > WLAN_WIEDERHOLUNGS_INTERVALL) { 
            if (strlen(config.backupSsid) > 0) {
                WiFi.persistent(false); WiFi.mode(WIFI_STA); 
                
                WiFi.disconnect(true); 
                // Non-Blocking Wait für sauberes Disconnect
                unsigned long start = millis();
                while(millis() - start < 500) { watchdogZaehler = 0; yield(); } 

                WiFi.begin(config.backupSsid, config.backupPasswort);
                letzterVerbindungsVersuch = millis(); letzterScanStart = millis(); stabilitaetsZaehler = 0;
            } else { letzterVerbindungsVersuch = millis(); }
        }
    }
}

// --- UDP KOMMUNIKATION ---
void verarbeiteUdpEmpfang() {
    // Rate Limiter Reset (Minuten-Takt)
    if (millis() - udpZaehlerReset > 60000) {
        udpPaketZaehler = 0;
        udpZaehlerReset = millis();
    }

    int paketGroesse = udp.parsePacket();
    if (paketGroesse) {
        // DoS Schutz Check
        if (udpPaketZaehler >= UDP_MAX_PAKETE_PRO_MINUTE) {
            sendeProtokoll("WARNUNG: UDP Rate Limit - Paket verworfen");
            udp.flush(); 
            return;
        }
        udpPaketZaehler++;

        char puffer[255]; 
        int laenge = udp.read(puffer, sizeof(puffer) - 1); // Overflow Schutz
        if (laenge > 0) puffer[laenge] = 0; // Null-Terminator
        
        String roheNachricht = String(puffer); 
        roheNachricht.trim();
        IPAddress senderIp = udp.remoteIP();

        // Format Parsen "TOKEN:BEFEHL"
        int trennerIndex = roheNachricht.indexOf(':');
        if (trennerIndex == -1) return; 

        String empfangenerToken = roheNachricht.substring(0, trennerIndex);
        String befehl = roheNachricht.substring(trennerIndex + 1);

        // Authentifizierung
        if (empfangenerToken != String(config.udpToken)) {
            sendeProtokoll("SECURITY: Falscher Token von " + senderIp.toString());
            return;
        }

        // Befehlsverarbeitung & ACK
        if (befehl == "ALARM_ON") {
            alarmAktiv = true;
            sendeProtokoll("ALARM AN. Sende ACK...");
            udp.beginPacket(senderIp, 4211); udp.print("ACK_" + roheNachricht); udp.endPacket();
        } else if (befehl == "ALARM_OFF") {
            alarmAktiv = false;
            sendeProtokoll("ALARM AUS. Sende ACK...");
            udp.beginPacket(senderIp, 4211); udp.print("ACK_" + roheNachricht); udp.endPacket();
        }
    }
}

// --- HARDWARE STEUERUNG ---
void aktualisiereAlarm() {
    if(alarmAktiv){
        // Wechselblinker & Sirene
        if (millis() - letzterToggle >= ALARM_TOGGLE_INTERVALL) {
            ledStatus = !ledStatus;
            digitalWrite(PIN_LED_ROT, ledStatus ? HIGH : LOW); digitalWrite(PIN_SUMMER_2, ledStatus ? HIGH : LOW);
            digitalWrite(PIN_LED_GELB, !ledStatus ? HIGH : LOW); digitalWrite(PIN_SUMMER_1, ledStatus ? HIGH : LOW);
            letzterToggle = millis();
        }
    } else {
        // Alles aus
        digitalWrite(PIN_SUMMER_1, LOW); digitalWrite(PIN_SUMMER_2, LOW);
        digitalWrite(PIN_LED_ROT, LOW); digitalWrite(PIN_LED_GELB, LOW);
    }
}

void pruefeTelnetZugang() {
  // Lockout Check
  if (millis() < telnetSperreBis) {
    if (TelnetStream.available()) {
      TelnetStream.readStringUntil('\n'); // Input verwerfen
      TelnetStream.println("GESPERRT. Warten...");
    }
    return;
  }

  if (TelnetStream.available()) {
    String eingabe = TelnetStream.readStringUntil('\n');
    eingabe.trim(); 
    if (eingabe.length() == 0) return; // Leere Inputs ignorieren

    Serial.print("Login: "); Serial.println(eingabe);

    if (eingabe == String(config.telnetPasswort)) {
        telnetAutorisiert = true;
        telnetFehlversuche = 0; 
        TelnetStream.println("LOGIN OK");
    } 
    else if (eingabe == "logout") {
        telnetAutorisiert = false;
        TelnetStream.println("Ausgeloggt.");
    } 
    else {
        telnetFehlversuche++;
        // Brute Force Schutz
        if (telnetFehlversuche >= MAX_TELNET_VERSUCHE) {
            telnetSperreBis = millis() + 300000; // 5 Min Sperre
            TelnetStream.println("!!! 5 MIN SPERRE !!!");
            TelnetStream.stop();
        } else {
            TelnetStream.println("Falsches PW");
        }
    }
  }
}

void ueberwacheTaster() {
    static unsigned long druckStart = 0;   
    bool aktuellerStatus = digitalRead(PIN_TASTER);

    if (aktuellerStatus == LOW) { // Gedrückt
        if (druckStart == 0) druckStart = millis();
        unsigned long dauer = millis() - druckStart;

        // Feedback LED-Flackern
        if (dauer > 500) { 
             if (millis() % 100 < 50) { digitalWrite(PIN_LED_ROT, HIGH); digitalWrite(PIN_LED_GELB, HIGH); } 
             else { digitalWrite(PIN_LED_ROT, LOW); digitalWrite(PIN_LED_GELB, LOW); }
        }

        // Factory Reset (10s)
        if (dauer > TASTER_RESET_DRUCK) { 
             sendeProtokoll("!!! RESET !!!");
             
             watchdogTicker.detach(); // WD Aus für Formatierung

             // Akustisches Signal
             unsigned long beepStart = millis();
             digitalWrite(PIN_SUMMER_1, HIGH);
             while(millis() - beepStart < 2000) { yield(); }
             digitalWrite(PIN_SUMMER_1, LOW);

             // FS Löschen & Reboot
             LittleFS.format(); 
             WiFiManager wm; wm.resetSettings(); 
             ESP.restart();
        }

    } else { // Losgelassen
        if (druckStart != 0) { 
            unsigned long dauer = millis() - druckStart;
            digitalWrite(PIN_LED_ROT, LOW); digitalWrite(PIN_LED_GELB, LOW);
            
            // Kurzer Druck: Toggle
            if (dauer < TASTER_LANG_DRUCK) {
                 alarmAktiv = !alarmAktiv;
                 sendeProtokoll("Taster: Alarm Toggle");
            }
            druckStart = 0;
        }
    }
}

void zeigeSystemStatus() {
    static unsigned long letzterDruck = 0;
    if (millis() - letzterDruck > STATUS_DRUCK_INTERVALL) {
       letzterDruck = millis();
       Serial.print("IP: "); Serial.print(WiFi.localIP()); 
       Serial.print(" | RAM: "); Serial.println(ESP.getFreeHeap());
       if (ESP.getFreeHeap() < 10000) sendeProtokoll("WARNUNG: Wenig RAM!");
    }
}

void speichereAktuellesWlan() {
    if (WiFi.status() == WL_CONNECTED) {
        String aktuelleVerbindung = WiFi.SSID();
        // Nur speichern wenn nicht Backup & neu
        if (aktuelleVerbindung != String(config.backupSsid) && aktuelleVerbindung.length() > 0) {
            if (String(config.hauptWlanName) != aktuelleVerbindung) {
                strcpy(config.hauptWlanName, aktuelleVerbindung.c_str()); 
                speichereKonfiguration(); 
            }
        }
    }
}

// --- SETUP ---
void setup() {
    Serial.begin(9600);
    // Pin Modes
    pinMode(PIN_LED_WLAN, OUTPUT); pinMode(PIN_LED_ROT, OUTPUT); pinMode(PIN_LED_GELB, OUTPUT);
    pinMode(PIN_SUMMER_1, OUTPUT); pinMode(PIN_SUMMER_2, OUTPUT); pinMode(PIN_TASTER, INPUT_PULLUP);
    
    // Initiale Zustände
    digitalWrite(PIN_LED_WLAN, LOW); digitalWrite(PIN_LED_ROT, LOW); digitalWrite(PIN_LED_GELB, LOW);
    digitalWrite(PIN_SUMMER_1, LOW); digitalWrite(PIN_SUMMER_2, LOW);

    ladeKonfiguration(); 
    
    WiFiManager wm;
    wm.setSaveConfigCallback(konfigurationSpeichernCallback);
    
    // Custom Parameter definieren
    WiFiManagerParameter custom_telnet_pass("tpass", "Telnet PW", config.telnetPasswort, 20);
    WiFiManagerParameter custom_token("token", "UDP Token", config.udpToken, 40);
    WiFiManagerParameter custom_name("name", "mDNS Name", config.mdnsName, 32);
    WiFiManagerParameter custom_backup_ssid("bssid", "Backup SSID", config.backupSsid, 32);
    WiFiManagerParameter custom_backup_pass("bpass", "Backup PW", config.backupPasswort, 64);
    WiFiManagerParameter custom_ap_pass("appw", "AP PW (min 8)", config.apPasswort, 64);

    wm.addParameter(&custom_telnet_pass); wm.addParameter(&custom_token); wm.addParameter(&custom_name);
    wm.addParameter(&custom_backup_ssid); wm.addParameter(&custom_backup_pass);
    wm.addParameter(&custom_ap_pass);

    wm.setConfigPortalTimeout(60); wm.setConnectTimeout(20);

    // AP starten (Sicher vs Offen)
    bool erfolg;
    if (strlen(config.apPasswort) >= 8) erfolg = wm.autoConnect("Alarm-Konfig", config.apPasswort);
    else erfolg = wm.autoConnect("Alarm-SETUP-OPEN");

    if (!erfolg) Serial.println("Offline -> Backup Modus");

    speichereAktuellesWlan(); // Hauptnetz merken

    // Parameter speichern wenn geändert
    strlcpy(config.telnetPasswort, custom_telnet_pass.getValue(), sizeof(config.telnetPasswort));
    strlcpy(config.backupSsid, custom_backup_ssid.getValue(), sizeof(config.backupSsid));
    strlcpy(config.backupPasswort, custom_backup_pass.getValue(), sizeof(config.backupPasswort));
    strlcpy(config.udpToken, custom_token.getValue(), sizeof(config.udpToken));
    strlcpy(config.mdnsName, custom_name.getValue(), sizeof(config.mdnsName));
    strlcpy(config.apPasswort, custom_ap_pass.getValue(), sizeof(config.apPasswort));

    if (konfigurationSpeichern) speichereKonfiguration();

    if(MDNS.begin(config.mdnsName)) Serial.println("mDNS aktiv");
    
    TelnetStream.begin();
    udp.begin(lokalerPort);
    starteOtaDienst(); 
    
    watchdogTicker.attach(1.0, watchdogInterrupt); // Watchdog aktivieren
    letzterScanStart = millis(); 
}

// --- HAUPTSCHLEIFE ---
void loop() {
    watchdogZaehler = 0; // Heartbeat: System lebt

    zeigeSystemStatus();        
    pruefeTelnetZugang();      
    verwalteWlanVerbindung();
    aktualisiereWlanLed();      
    verarbeiteUdpEmpfang();          
    ueberwacheTaster();        
    aktualisiereAlarm();        
    
    if (WiFi.status() == WL_CONNECTED) { ArduinoOTA.handle(); MDNS.update(); }
}