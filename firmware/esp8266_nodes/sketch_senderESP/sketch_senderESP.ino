/*
 * PROJEKT: ESP8266 UDP Alarm-System (Sender)
 * ------------------------------------------
 * Beschreibung: Sendet Steuerbefehle (ALARM_ON/ALARM_OFF) per UDP an den Empfänger.
 * Das System ist auf Ausfallsicherheit ausgelegt (Failover-WLAN) und wartet
 * auf Bestätigungen (ACK), bevor es den Status als "erfolgreich" anzeigt.
 * * Features:
 * - Sicherheit:      Watchdog, Token-Authentifizierung
 * - Feedback:        Status-LED am Sender schaltet erst, wenn Empfänger das ACK sendet
 * - Wartung:         OTA-Updates, Telnet-Debugging, Config-Persistenz (LittleFS)
 * * Hardware:        NodeMCU V2 Amica (ESP8266 12E)
 * Autor:    Philip Keminer
 * Version:  First Final German Clean Code
 * Datum:    2026-01-29
 */

#include <FS.h>             // Zugriff auf den Flash-Speicher
#include <LittleFS.h>       // Dateisystem für Konfiguration
#include <ArduinoJson.h>    // Bibliothek zum Lesen/Schreiben von JSON
#include <ESP8266WiFi.h>    // Basis-WLAN-Funktionen
#include <WiFiManager.h>    // Erstellt Hotspot zur Einrichtung (Captive Portal)
#include <WiFiUdp.h>        // UDP Netzwerkprotokoll
#include <ESP8266mDNS.h>    // Lokale Namensauflösung (findet "alarm-empfaenger.local")
#include <ArduinoOTA.h>     // Updates über WLAN (Over-The-Air)
#include <TelnetStream.h>   // Debugging über Netzwerk
#include <Ticker.h>         // Hardware-Timer für Watchdog

// --- SYSTEM KONSTANTEN (Timing) ---
const unsigned long WLAN_WIEDERHOLUNGS_INTERVALL = 20000; // 20s warten vor Reconnect-Versuch
const unsigned long WLAN_SCAN_INTERVALL = 30000;          // Alle 30s nach Hauptnetz scannen (im Backup-Modus)
const uint8_t STABILITAETS_SCHWELLWERT = 3;               // Netz muss 3x stabil sein vor Wechsel
const int8_t RSSI_SCHWELLWERT = -75;                      // Min. Signalstärke für Hauptnetz
const unsigned long TASTER_RESET_DRUCK = 10000;           // 10s Drücken für Factory Reset
const unsigned long STATUS_DRUCK_INTERVALL = 5000;        // Alle 5s Status ausgeben
const unsigned long BLINK_INTERVALL = 500;                // LED Blink-Geschwindigkeit bei Suche
const int WATCHDOG_TIMEOUT_SEK = 10;                      // Hard-Reset nach 10s Hänger

// --- SENDER SPEZIFISCH ---
const unsigned long SENDE_WIEDERHOLUNGS_INTERVALL = 1000; // Wenn kein ACK kommt: Nach 1s neu senden
const int MAX_SENDE_VERSUCHE = 10;                        // Nach 10 Versuchen aufgeben (Fehler)
const unsigned long IP_UPDATE_INTERVALL = 60000;          // IP-Adresse nur alle 60s neu auflösen (Performance!)

// --- SICHERHEIT ---
const uint8_t MAX_TELNET_VERSUCHE = 3; // Max. Login-Versuche vor Sperre

// --- HARDWARE PIN DEFINITIONEN ---
#define PIN_RESET_TASTER D3   // Flash-Button am NodeMCU
#define PIN_LED_ALARM LED_BUILTIN // Interne LED (Active Low: LOW = AN)
#define PIN_LED_WLAN D5       // Externe Status-LED

// --- KONFIGURATIONS-STRUCT ---
// Speichert alle Einstellungen zentral an einem Ort
struct SystemKonfiguration {
    char udpToken[41] = "";             // Sicherheits-Token (muss gleich Empfänger sein)
    char mdnsZiel[33] = "alarm-empfaenger"; // Netzwerkname des Empfängers
    char telnetPasswort[21] = "";       // Passwort für Telnet/OTA
    char backupSsid[33] = "";           // Backup WLAN Name
    char backupPasswort[65] = "";       // Backup WLAN Passwort
    char hauptWlanName[33] = "";        // Automatisch gelerntes Hauptnetz
    char apPasswort[65] = "12345678";   // AP Passwort (Default)
};

SystemKonfiguration config; // Globale Instanz der Konfiguration

// --- STATUS VARIABLEN ---
bool konfigurationSpeichern = false;    // Flag: Soll Config gespeichert werden?
bool telnetAutorisiert = false;         // Ist User eingeloggt?
unsigned long letzterVerbindungsVersuch = 0; 
unsigned long letzterScanStart = 0;
int scanStatus = -1;                    // Status des asynchronen Scans (-1 = inaktiv)

// --- SENDER LOGIK VARIABLEN ---
String ausstehendeNachricht = "";       // Der Befehl, der gerade gesendet wird (z.B. "ALARM_ON")
bool wartetAufBestatigung = false;      // Warten wir auf ein ACK?
unsigned long letzterSendeZeitpunkt = 0;// Wann ging das letzte Paket raus?
int wiederholungsZaehler = 0;           // Wie oft haben wir es schon versucht?

// --- NETZWERK ZUSTAND ---
IPAddress zielIpAdresse;                // IP des Empfängers (gecached)
const unsigned int zielPort = 4210;     // Port des Empfängers
const unsigned int lokalerPort = 4211;  // Eigener Port
WiFiUDP udp;                            // UDP Instanz
unsigned long letztesBlinken = 0;
unsigned long letzteIpAktualisierung = 0; // Zeitstempel für mDNS Cache

// --- SYSTEM STATE & WATCHDOG ---
uint8_t telnetFehlversuche = 0;       
unsigned long telnetSperreBis = 0;      // Zeitstempel für Login-Sperre
Ticker watchdogTicker;                  // Timer für Hardware-Watchdog
volatile int watchdogZaehler = 0;       // Zähler (volatile für ISR Zugriff)

// --- WATCHDOG ISR ---
// ICACHE_RAM_ATTR ist zwingend nötig, damit der Code im RAM liegt und nicht crasht
void ICACHE_RAM_ATTR watchdogInterrupt() {
    watchdogZaehler++;
    // Wenn Hauptschleife den Zähler nicht zurücksetzt -> Hardware Reset
    if (watchdogZaehler >= WATCHDOG_TIMEOUT_SEK) {
        ESP.restart();
    }
}

// --- LOGGING FUNKTION ---
// Sendet Nachrichten an USB (Serial) und Netzwerk (Telnet) gleichzeitig
void sendeProtokoll(const String &nachricht) {
  Serial.println(nachricht); 
  // Nur senden, wenn WLAN da ist UND User eingeloggt ist (Sicherheit)
  if (WiFi.status() == WL_CONNECTED && telnetAutorisiert) {
    TelnetStream.println(nachricht);
    TelnetStream.flush();
  }
}

// --- CONFIG HANDLING ---
// Callback für WiFiManager
void konfigurationSpeichernCallback() { konfigurationSpeichern = true; }

// Lädt Einstellungen aus dem Flash-Speicher
void ladeKonfiguration() {
  if (LittleFS.begin()) {
    if (LittleFS.exists("/config.json")) {
      File datei = LittleFS.open("/config.json", "r");
      if (datei) {
        StaticJsonDocument<512> doc; // Puffer im Stack
        DeserializationError fehler = deserializeJson(doc, datei);
        if (!fehler) {
           // Sicheres Kopieren mit Längenbegrenzung (strlcpy)
           if(doc["token"]) strlcpy(config.udpToken, doc["token"], sizeof(config.udpToken));
           if(doc["ziel"])  strlcpy(config.mdnsZiel, doc["ziel"], sizeof(config.mdnsZiel));
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

// Speichert Einstellungen in Flash-Speicher
void speichereKonfiguration() {
  StaticJsonDocument<512> doc;
  // Struct in JSON mappen
  doc["token"] = config.udpToken; doc["ziel"] = config.mdnsZiel; doc["tpass"] = config.telnetPasswort;
  doc["bssid"] = config.backupSsid; doc["bpass"] = config.backupPasswort; doc["hssid"] = config.hauptWlanName; 
  doc["appw"]  = config.apPasswort;
  File datei = LittleFS.open("/config.json", "w");
  if (datei) { serializeJson(doc, datei); datei.close(); }
}

// Startet OTA (Over-The-Air Update) Dienst
void starteOtaDienst() {
  ArduinoOTA.setPort(8266); 
  ArduinoOTA.setHostname("alarm-sender"); 
  ArduinoOTA.setPassword(config.telnetPasswort); // Gleiches PW wie Telnet

  ArduinoOTA.onStart([]() { sendeProtokoll(">> OTA Start"); });
  ArduinoOTA.onEnd([]() { sendeProtokoll("\n>> OTA Ende"); });
  ArduinoOTA.onProgress([](unsigned int fortschritt, unsigned int gesamt) {
      static int letzterProzent = 0;
      int prozent = (fortschritt / (gesamt / 100));
      // Log-Ausgabe reduzieren (nur alle 10%)
      if (prozent >= letzterProzent + 10 || prozent == 100) {
          letzterProzent = prozent;
          sendeProtokoll(">> Upload: " + String(prozent) + "%");
      }
  });
  ArduinoOTA.onError([](ota_error_t fehler) { sendeProtokoll("!! OTA Fehler: " + String(fehler)); });
  ArduinoOTA.begin();
}

// Steuert die WLAN-Status LED (Blinken bei Suche, An bei Verbindung)
void aktualisiereWlanLed(){
    if (WiFi.status() != WL_CONNECTED) {
        if (millis() - letztesBlinken >= BLINK_INTERVALL) { digitalWrite(PIN_LED_WLAN, !digitalRead(PIN_LED_WLAN)); letztesBlinken = millis(); }
    } else { digitalWrite(PIN_LED_WLAN, HIGH); }
}

// --- NETZWERK & FAILOVER LOGIK ---
// Handhabt Verbindung, Backup-WLAN und Rückwechsel zum Hauptnetz
void verwalteWlanVerbindung() {
    static int stabilitaetsZaehler = 0; 
    
    // Fall 1: Verbunden
    if (WiFi.status() == WL_CONNECTED) {
        String aktuelleSsid = WiFi.SSID();
        // Sind wir im Backup-Netz? Wenn ja, suche Hauptnetz
        if (aktuelleSsid == String(config.backupSsid) && strlen(config.backupSsid) > 0) {
            // Asynchroner Scan starten (blockiert nicht)
            if (millis() - letzterScanStart > WLAN_SCAN_INTERVALL && scanStatus == -1) { 
                letzterScanStart = millis(); WiFi.scanNetworks(true); scanStatus = 0; 
            }
            // Scan-Ergebnisse verarbeiten
            if (scanStatus == 0) {
                int n = WiFi.scanComplete();
                if (n >= 0) {
                    bool hauptNetzGefunden = false;
                    for (int i = 0; i < n; i++) {
                        // Prüfen ob Hauptnetz da ist UND Signal stark genug
                        if (WiFi.SSID(i) == String(config.hauptWlanName) && String(config.hauptWlanName).length() > 0) {
                          if (WiFi.RSSI(i) > RSSI_SCHWELLWERT) { hauptNetzGefunden = true; break; }
                        }
                    }
                    if (hauptNetzGefunden) {
                        stabilitaetsZaehler++;
                        // Hysterese: Erst wechseln wenn 3x stabil gefunden
                        if (stabilitaetsZaehler >= STABILITAETS_SCHWELLWERT) { 
                             watchdogTicker.detach(); // WD aus für Restart
                             // Visuelles Feedback
                             for(int k=0; k<10; k++) { digitalWrite(PIN_LED_WLAN, !digitalRead(PIN_LED_WLAN)); delay(50); }
                             ESP.restart(); // Sauberer Neustart ins Hauptnetz
                        }
                    } else { stabilitaetsZaehler = 0; }
                    WiFi.scanDelete(); scanStatus = -1;  
                }
            }
        } else { stabilitaetsZaehler = 0; }
    } else {
        // Fall 2: Verbindung verloren
        if (millis() - letzterVerbindungsVersuch > WLAN_WIEDERHOLUNGS_INTERVALL) { 
            if (strlen(config.backupSsid) > 0) {
                WiFi.persistent(false); WiFi.mode(WIFI_STA); 
                
                WiFi.disconnect(true); 
                // Warten (Non-Blocking mit Watchdog Feed)
                unsigned long start = millis();
                while(millis() - start < 500) { watchdogZaehler = 0; yield(); }

                WiFi.begin(config.backupSsid, config.backupPasswort);
                letzterVerbindungsVersuch = millis(); letzterScanStart = millis(); stabilitaetsZaehler = 0;
            } else { letzterVerbindungsVersuch = millis(); }
        }
    }
}

// --- ZIEL-IP RESOLVING ---
// Löst Namen in IP auf (mDNS) mit Caching für Performance
void aktualisiereZielIp() {
    // Wenn IP erst kürzlich gesucht wurde -> Abbruch (Cache nutzen)
    if (millis() - letzteIpAktualisierung < IP_UPDATE_INTERVALL && zielIpAdresse.toString() != "0.0.0.0") {
        return; 
    }
    
    // Suche nach "alarm-empfaenger.local"
    WiFi.hostByName((String(config.mdnsZiel) + ".local").c_str(), zielIpAdresse);
    letzteIpAktualisierung = millis();
    
    // Wenn nicht gefunden: Broadcast als Fallback
    if (zielIpAdresse.toString() == "0.0.0.0") {
        Serial.println("WARN: mDNS Fehler -> Nutze Broadcast");
        zielIpAdresse = IPAddress(255, 255, 255, 255);
    } else {
        Serial.println("INFO: Ziel IP: " + zielIpAdresse.toString());
    }
}

// --- KOMMUNIKATION (UDP) ---
// Verarbeitet Antworten (ACKs) vom Empfänger
void verarbeiteUdpAntworten() {
    int paketGroesse = udp.parsePacket();
    if (paketGroesse) {                                                      
        char puffer[255]; 
        int laenge = udp.read(puffer, sizeof(puffer) - 1); 
        if (laenge > 0) puffer[laenge] = 0;
        
        String roheNachricht = String(puffer); roheNachricht.trim();
        
        // Prüfen: Warten wir auf ACK? Passt das ACK zum Befehl?
        if (wartetAufBestatigung && roheNachricht == "ACK_" + ausstehendeNachricht) {            
            sendeProtokoll("Erfolg: Bestaetigung erhalten!");
            
            // WICHTIG: LED erst JETZT umschalten (Synchronisation)
            if (ausstehendeNachricht.endsWith("ALARM_ON")) {
                digitalWrite(PIN_LED_ALARM, LOW); // AN
            } else if (ausstehendeNachricht.endsWith("ALARM_OFF")) {
                digitalWrite(PIN_LED_ALARM, HIGH); // AUS
            }

            // Sendevorgang erfolgreich beendet
            wartetAufBestatigung = false; ausstehendeNachricht = ""; wiederholungsZaehler = 0;                                                        
        }
    }

    // Retry Logik: Wenn keine Antwort, nochmal senden
    if (wartetAufBestatigung && wiederholungsZaehler < MAX_SENDE_VERSUCHE && millis() - letzterSendeZeitpunkt >= SENDE_WIEDERHOLUNGS_INTERVALL) { 
        watchdogZaehler = 0; // Watchdog füttern (Netzwerk kann dauern)
        wiederholungsZaehler++;                                                                           
        sendeProtokoll("Wiederholung " + String(wiederholungsZaehler));                  
        if (zielIpAdresse.toString() != "0.0.0.0") {                                             
            udp.beginPacket(zielIpAdresse, zielPort); 
            udp.print(ausstehendeNachricht); 
            udp.endPacket();
        }
        letzterSendeZeitpunkt = millis();
    }
    // Timeout erreicht: Fehler
     if (wartetAufBestatigung && wiederholungsZaehler >= MAX_SENDE_VERSUCHE) {
        sendeProtokoll("FEHLER: Timeout - Keine Antwort!"); 
        // Wir ändern den LED Status NICHT, da wir nicht wissen, ob der Empfänger geschaltet hat
        wartetAufBestatigung = false; ausstehendeNachricht = ""; wiederholungsZaehler = 0;                                                        
    }
}

// --- SERIELLE BEFEHLE (USB) ---
void verarbeiteSerielleBefehle(){
    if (Serial.available()) {                  
        String befehl = Serial.readStringUntil('\n'); befehl.trim();                                                          
      if (befehl == "ALARM_ON") {   
            ausstehendeNachricht = String(config.udpToken) + ":ALARM_ON";
            // Sendevorgang starten
            wartetAufBestatigung = true; wiederholungsZaehler = 0; letzterSendeZeitpunkt = millis();                                    
            
            aktualisiereZielIp(); 
            udp.beginPacket(zielIpAdresse, zielPort); 
            udp.print(ausstehendeNachricht); 
            udp.endPacket();
            
      } else if (befehl == "ALARM_OFF") {  
            ausstehendeNachricht = String(config.udpToken) + ":ALARM_OFF";
            wartetAufBestatigung = true; wiederholungsZaehler = 0; letzterSendeZeitpunkt = millis();
            
            aktualisiereZielIp(); 
            udp.beginPacket(zielIpAdresse, zielPort); 
            udp.print(ausstehendeNachricht); 
            udp.endPacket();
      }
    }
}

// --- TELNET SECURITY ---
void pruefeTelnetZugang() {
  if (millis() < telnetSperreBis) {
    if (TelnetStream.available()) {
      TelnetStream.readStringUntil('\n'); 
      TelnetStream.println("GESPERRT.");
    }
    return;
  }
  if (TelnetStream.available()) {
    String eingabe = TelnetStream.readStringUntil('\n');
    eingabe.trim(); 
    if (eingabe.length() == 0) return; 

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
        if (telnetFehlversuche >= MAX_TELNET_VERSUCHE) {
            telnetSperreBis = millis() + 300000; 
            TelnetStream.println("!!! SPERRE !!!");
            TelnetStream.stop();
        } else {
            TelnetStream.println("Falsches PW");
        }
    }
  }
}

// --- HARDWARE RESET ---
void pruefePhysischenReset() {
    static unsigned long druckStart = 0;
    // Taster gedrückt?
    if (digitalRead(PIN_RESET_TASTER) == LOW) {
        if (druckStart == 0) druckStart = millis();
        
        // Blink-Feedback (schnelles Flackern)
        if (millis() % 200 < 100) digitalWrite(PIN_LED_ALARM, LOW); else digitalWrite(PIN_LED_ALARM, HIGH);
        
        // Wenn 10s gehalten -> Reset
        if (millis() - druckStart > TASTER_RESET_DRUCK) {
             sendeProtokoll("!!! HARDWARE RESET !!!");
             
             watchdogTicker.detach(); 
             
             // Dauerlicht als Bestätigung
             digitalWrite(PIN_LED_ALARM, LOW); 
             unsigned long start = millis();
             while(millis() - start < 2000) { yield(); }
             digitalWrite(PIN_LED_ALARM, HIGH);

             LittleFS.format(); // Config löschen
             WiFiManager wm; wm.resetSettings(); // WLAN löschen
             ESP.restart();
        }
    } else {
        druckStart = 0;
    }
}

// --- SYSTEM STATUS ---
void zeigeSystemStatus() {
    static unsigned long letzterDruck = 0;
    if (millis() - letzterDruck > STATUS_DRUCK_INTERVALL) {
       letzterDruck = millis();
       Serial.print("IP: "); Serial.print(WiFi.localIP()); 
       Serial.print(" | RAM: "); Serial.println(ESP.getFreeHeap());
       if (ESP.getFreeHeap() < 10000) sendeProtokoll("WARNUNG: Wenig RAM!");
    }
}

// --- CONFIG HELPER ---
void speichereAktuellesWlan() {
    if (WiFi.status() == WL_CONNECTED) {
        String aktuelleVerbindung = WiFi.SSID();
        // Nur Hauptnetz speichern, nicht das Backup oder leere Netze
        if (aktuelleVerbindung != String(config.backupSsid) && aktuelleVerbindung.length() > 0) {
            if (String(config.hauptWlanName) != aktuelleVerbindung) {
                strcpy(config.hauptWlanName, aktuelleVerbindung.c_str()); 
                speichereKonfiguration(); 
            }
        }
    }
}

// --- SETUP (Initialisierung) ---
void setup() {
    delay(1000); Serial.begin(9600); 
    Serial.setTimeout(1000); // Wichtig für Serial.readStringUntil
    
    // Pins
    pinMode(PIN_LED_ALARM, OUTPUT); digitalWrite(PIN_LED_ALARM, HIGH); 
    pinMode(PIN_LED_WLAN, OUTPUT); digitalWrite(PIN_LED_WLAN, LOW);
    pinMode(PIN_RESET_TASTER, INPUT_PULLUP); 

    ladeKonfiguration();

    WiFiManager wm;
    wm.setSaveConfigCallback(konfigurationSpeichernCallback);
    
    // Custom Parameter für das WLAN-Portal
    WiFiManagerParameter custom_token("token", "UDP Token", config.udpToken, 40);
    WiFiManagerParameter custom_ziel("ziel", "mDNS Ziel", config.mdnsZiel, 32);
    WiFiManagerParameter custom_telnet_pass("tpass", "Telnet PW", config.telnetPasswort, 20);
    WiFiManagerParameter custom_backup_ssid("bssid", "Backup SSID", config.backupSsid, 32);
    WiFiManagerParameter custom_backup_pass("bpass", "Backup PW", config.backupPasswort, 64);
    WiFiManagerParameter custom_ap_pass("appw", "AP PW (min 8)", config.apPasswort, 64);

    wm.addParameter(&custom_token); wm.addParameter(&custom_ziel); wm.addParameter(&custom_telnet_pass);
    wm.addParameter(&custom_backup_ssid); wm.addParameter(&custom_backup_pass);
    wm.addParameter(&custom_ap_pass); 

    wm.setConfigPortalTimeout(60); 
    wm.setConnectTimeout(20);      

    Serial.println("Verbinde...");

    bool erfolg;
    // Logik: Wenn Passwort gesetzt -> Sicherer AP, sonst Offener AP
    if (strlen(config.apPasswort) >= 8) {
        erfolg = wm.autoConnect("Alarm-Sender-Konfig", config.apPasswort);
    } else {
        erfolg = wm.autoConnect("Alarm-Sender-SETUP-OPEN");
    }

    if (!erfolg) { Serial.println("Offline -> Backup Logik."); }

    speichereAktuellesWlan(); // Funktion ausgelagert

    // Werte aus Portal übernehmen
    strlcpy(config.udpToken, custom_token.getValue(), sizeof(config.udpToken));
    strlcpy(config.mdnsZiel, custom_ziel.getValue(), sizeof(config.mdnsZiel));
    strlcpy(config.telnetPasswort, custom_telnet_pass.getValue(), sizeof(config.telnetPasswort));
    strlcpy(config.backupSsid, custom_backup_ssid.getValue(), sizeof(config.backupSsid));
    strlcpy(config.backupPasswort, custom_backup_pass.getValue(), sizeof(config.backupPasswort));
    strlcpy(config.apPasswort, custom_ap_pass.getValue(), sizeof(config.apPasswort));

    if (konfigurationSpeichern) speichereKonfiguration(); 

    if (MDNS.begin("alarm-sender")) Serial.println("mDNS aktiv");
    
    TelnetStream.begin(); 
    udp.begin(lokalerPort); 
    starteOtaDienst();
    
    aktualisiereZielIp(); 
    watchdogTicker.attach(1.0, watchdogInterrupt); // Watchdog aktivieren
}

// --- HAUPTSCHLEIFE ---
void loop() {
    watchdogZaehler = 0; // Heartbeat: Ich lebe noch
    aktualisiereZielIp(); // IP-Cache Check (Non-Blocking)

    zeigeSystemStatus();        
    pruefeTelnetZugang();
    verwalteWlanVerbindung();
    aktualisiereWlanLed();
    verarbeiteSerielleBefehle();
    verarbeiteUdpAntworten(); 
    pruefePhysischenReset(); 
    
    if (WiFi.status() == WL_CONNECTED) { ArduinoOTA.handle(); MDNS.update(); }
}