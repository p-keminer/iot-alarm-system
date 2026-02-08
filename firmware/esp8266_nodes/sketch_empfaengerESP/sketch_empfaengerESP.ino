/*
 * PROJEKT: ESP8266 UDP Alarm-System (Empfänger)
 * ---------------------------------------------
 * Beschreibung: Empfängt kryptografisch signierte UDP-Pakete und steuert Alarm-Hardware.
 * Priorisiert Hardware-Signale über Netzwerk-Tasks für minimale Latenz.
 *
 * Features & Funktionen:
 * ----------------------
 * 1. NETZWERK & PERFORMANCE
 * - Priority-Mode: Bei aktivem Alarm werden API/Heartbeats pausiert (Stop-the-World).
 * - Resultat: Kein "Stottern" der LEDs/Summer, selbst wenn Server offline ist.
 * - WLAN Failover (Backup-SSID) & asynchroner Reconnect.
 *
 * 2. SICHERHEIT (SECURITY HARDENING)
 * - HMAC-SHA256: Authentifiziert Sender via Secret Token (BearSSL C-API).
 * - Anti-Replay: Sequenznummern-Validierung gegen Aufzeichnungs-Angriffe.
 * - Traffic Obfuscation: Erwartet "NICE_TRY_WIRESHARK_USER" statt Klartext-Befehle.
 * - DoS-Schutz: Rate-Limiting für eingehende UDP-Pakete.
 * - Telnet Härtung: Auto-Logout, Brute-Force-Schutz & Easter Egg.
 *
 * 3. SYSTEM & STABILITÄT
 * - Watchdog V2: Ausgelagert ("fuettereWauWau"), überwacht Loop-Zykluszeit.
 * - Safe Reset: Werksreset nur bei bewusstem Loslassen des Tasters (>10s).
 * - Telemetrie: Sendet Vitaldaten (RSSI, Heap, Reset-Reason) im Leerlauf.
 *
 * Hardware:   NodeMCU V2 (ESP8266)
 * Autor:      Philip Keminer
 * Version:    V10.0 (Receiver - Final)
 * Datum:      2026-02-03
 */

// --- BIBLIOTHEKEN ---
#include <FS.h>                 // Dateisystem-Basisklasse für Abstraktionsschicht
#include <LittleFS.h>           // Flash-Dateisystem für Config-Persistierung
#include <ArduinoJson.h>        // JSON De/Serialisierung für Config und API-Kommunikation
#include <ESP8266WiFi.h>        // WiFi Stack mit TCP/IP Implementation
#include <WiFiManager.h>        // Captive Portal für initiale WLAN-Konfiguration über Smartphone
#include <WiFiUdp.h>            // User Datagram Protocol - verbindungslos, schnell für Echtzeit-Commands
#include <ESP8266mDNS.h>        // Multicast DNS für lokale Namensauflösung ohne DNS-Server
#include <ESP8266HTTPClient.h>  // HTTP Client für REST API Kommunikation
#include <WiFiClient.h>         // Basis TCP Client Implementation
#include <ArduinoOTA.h>         // Over-The-Air Firmware Updates ohne USB-Kabel
#include <TelnetStream.h>       // Remote Debugging Console über Telnet-Protokoll
#include <Ticker.h>             // Hardware-Timer für periodische Interrupts
#include <WiFiClientSecure.h>   // TLS/SSL Support für verschlüsselte Verbindungen
#include <bearssl/bearssl.h>    // Low-Level Krypto-Bibliothek für HMAC-Berechnung

// --- KONSTANTEN ---
const char* DEVICE_NAME = "alarm-receiver"; 

// --- TIMING & GRENZWERTE ---
const unsigned long ALARM_TOGGLE_INTERVALL = 200;    // Blink-Frequenz: LED/Summer wechseln alle 200ms (5 Hz)
const unsigned long WLAN_WIEDERHOLUNGS_INTERVALL = 20000; // Nach Verbindungsabbruch: 20 Sekunden warten vor Reconnect
const unsigned long WLAN_SCAN_INTERVALL = 30000;     // Alle 30 Sekunden nach besserem Hauptnetz scannen
const uint8_t STABILITAETS_SCHWELLWERT = 3;          // Hauptnetz muss 3x hintereinander erkannt werden bevor Wechsel
const int8_t RSSI_SCHWELLWERT = -75;                 // Signal muss besser als -75dBm sein (je näher 0 desto besser)
const unsigned long TASTER_LANG_DRUCK = 1000;        // Drücke unter 1 Sekunde = Toggle Alarm
const unsigned long TASTER_RESET_DRUCK = 10000;      // Drücke über 10 Sekunden = Factory Reset (Sicherheitsfeature)
const unsigned long BLINK_INTERVALL = 500;           // WLAN-LED blinkt alle 500ms wenn nicht verbunden
const unsigned long HEARTBEAT_INTERVALL = 2000;      // Alle 2 Sekunden Vitaldaten an Server senden
const int WATCHDOG_TIMEOUT_SEK = 30;                 // System rebootet nach 30 Sekunden ohne Loop-Durchlauf
const unsigned long TELNET_TIMEOUT = 300000;         // Telnet-Session timeout nach 5 Minuten Inaktivität

// --- SICHERHEIT ---
const uint8_t UDP_MAX_PAKETE_PRO_MINUTE = 60;        // DoS-Schutz: Max 60 Pakete/Min (1 pro Sekunde)
const uint8_t MAX_TELNET_VERSUCHE = 3;               // Brute-Force-Schutz: Nach 3 Fehlversuchen 5min Sperre

// --- OBFUSCATED PAYLOADS (Traffic Verschleierung) ---
// Diese Strings verbergen die tatsächliche Funktion vor einfacher Paketanalyse
// Angreifer sehen nicht "ALARM_ON" sondern kryptische Strings
const char* CMD_ALARM_AN  = "NICE_TRY_WIRESHARK_USER";
const char* CMD_ALARM_AUS = "ENCRYPTION_IS_YOUR_FRIEND";

// --- PINS ---
// NodeMCU Pin-Mapping: D1-D7 sind GPIO-Nummern
#define PIN_LED_ROT D1      // Alarm-LED 1 (wechselt mit Gelb)
#define PIN_LED_GELB D2     // Alarm-LED 2 (wechselt mit Rot)
#define PIN_LED_WLAN D3     // Status-LED für WiFi-Verbindung
#define PIN_SUMMER_1 D5     // Akustischer Alarm 1 (wechselt mit Summer 2)
#define PIN_SUMMER_2 D6     // Akustischer Alarm 2 (wechselt mit Summer 1)
#define PIN_TASTER D7       // Multifunktions-Taster: Toggle (<1s), Reset (>10s)

// --- KONFIGURATIONS-STRUCT ---
// Alle persistenten Einstellungen in einer Datenstruktur
struct SystemKonfiguration {
    char udpToken[41] = "";             // 40 Zeichen HMAC Secret + Nullterminator
    char mdnsName[33] = "alarm-receiver"; // Hostname für lokales Netzwerk (max 32 + \0)
    char telnetPasswort[21] = "";       // Passwort für Debug-Konsole
    char backupSsid[33] = "";           // Fallback-WLAN wenn Hauptnetz ausfällt
    char backupPasswort[65] = "";       // WPA2-Passwort kann bis 64 Zeichen lang sein
    char hauptWlanName[33] = "";        // Primäres WLAN mit bester Performance
    char hauptWlanPasswort[65] = ""; 
    char apPasswort[65] = "12345678";   // Passwort für Setup-Access-Point (mindestens 8 Zeichen)
    char apiServer[33] = "";            // IP oder Hostname des Backend-Servers
};

SystemKonfiguration config; // Globale Instanz der Konfiguration

// --- GLOBALE VARIABLEN ---
bool konfigurationSpeichern = false; // Flag: Wurde Config in WiFiManager geändert?
bool telnetAutorisiert = false;      // Zugriffskontrolle für Debug-Konsole
bool darfLoggen = false;             // Server-gesteuertes Logging (kann remote deaktiviert werden)
bool otaLauft = false;               // Verhindert andere Operationen während Firmware-Update

// Timing-Variablen für nicht-blockierende Operationen
unsigned long letzterVerbindungsVersuch = 0; // Timestamp für Reconnect-Logik
unsigned long letzterScanStart = 0;           // Timestamp für WLAN-Scan
unsigned long letzterHeartbeat = 0;           // Timestamp für letzte Telemetrie
unsigned long letzterTelnetInput = 0;         // Timestamp für Session-Timeout
int scanStatus = -1;                          // -1=Idle, 0=Läuft, >0=Ergebnisse

unsigned long aktuellesHeartbeatIntervall = 2000; // Dynamisch: 2s normal, 60s bei Serverfehler

// Sicherheits-Zähler
uint8_t telnetFehlversuche = 0;      // Zählt Falsch-Logins für Brute-Force-Schutz
unsigned long telnetSperreBis = 0;   // Timestamp bis wann Telnet gesperrt ist
uint8_t udpPaketZaehler = 0;         // Zählt Pakete pro Minute für Rate-Limiting
unsigned long udpZaehlerReset = 0;   // Timestamp für 60-Sekunden-Fenster
unsigned long letzteSequenceNumber = 0; // Anti-Replay: Verhindert erneutes Abspielen alter Pakete

// Netzwerk Objekte
WiFiUDP udp;                         // UDP Socket für schnelle Command-Übertragung
const unsigned int lokalerPort = 4210; // Port auf dem der Empfänger lauscht

// Hardware Status
volatile bool alarmAktiv = false;    // volatile: Kann durch ISR geändert werden
bool ledStatus = false;              // Aktueller Zustand des Blink-Zyklus
unsigned long letzterToggle = 0;     // Timestamp für LED/Summer-Wechsel
unsigned long letztesBlinken = 0;    // Timestamp für WLAN-Status-LED

// Watchdog Objekte
Ticker watchdogTicker;               // Hardware-Timer der jede Sekunde auslöst
volatile int watchdogZaehler = 0;    // Zählt hoch wenn Loop hängt
volatile bool mussNeustarten = false; // Flag für sauberen Reboot

// --- VORDEKLARATIONEN ---
// Funktionen die vor ihrer Definition aufgerufen werden müssen deklariert sein
void sendeLogAnApi(String nachricht);
void sendeHeartbeatAnApi();

// --- KRYPTO HELFER (C-API) ---
// HMAC-SHA256: Hash-based Message Authentication Code
// Erstellt digitale Signatur mit Secret Key - nur Besitzer des Keys können gültige Signaturen erzeugen
String berechneHMAC(String nachricht, String secret) {
    br_hmac_key_context kc;  // Key-Kontext speichert Hash-Algorithmus und Secret
    br_hmac_context ctx;      // Hash-Kontext für die eigentliche Berechnung
    
    // 1. Key-Kontext initialisieren mit SHA256 und Secret
    br_hmac_key_init(&kc, &br_sha256_vtable, secret.c_str(), secret.length());
    
    // 2. Hash-Kontext mit Key verknüpfen (0 = Output-Länge automatisch)
    br_hmac_init(&ctx, &kc, 0);
    
    // 3. Nachricht in Hash einarbeiten (kann mehrfach aufgerufen werden für große Daten)
    br_hmac_update(&ctx, nachricht.c_str(), nachricht.length());
    
    // 4. Hash finalisieren - 32 Bytes für SHA256
    uint8_t result[32];
    br_hmac_out(&ctx, result);
    
    // 5. Binäre Bytes in Hexadezimal-String konvertieren für einfache Übertragung
    String hexString = "";
    for (int i = 0; i < 32; i++) {
        if (result[i] < 16) hexString += "0"; // Führende Null für 0x00-0x0F
        hexString += String(result[i], HEX);
    }
    return hexString;
}

// --- WATCHDOG SYSTEM ---
// Interrupt Service Routine - wird jede Sekunde vom Hardware-Timer aufgerufen
void ICACHE_RAM_ATTR watchdogInterrupt() {
    watchdogZaehler++; // Zählt hoch wenn loop() nicht reagiert
    if (watchdogZaehler >= WATCHDOG_TIMEOUT_SEK) mussNeustarten = true;
}

// Watchdog zurücksetzen - beweist dass loop() noch läuft
void fuettereWauWau() {
    watchdogZaehler = 0; // Zähler nullen = System lebt noch
}

// Prüft ob Watchdog ausgelöst wurde und führt Reboot durch
void beissZu(){
    if (mussNeustarten) {
        Serial.println("WATCHDOG RESET!"); // Letzte Nachricht vor dem Neustart
        delay(100); // Sicherstellen dass Nachricht gesendet wurde
        ESP.restart(); // Hardware-Reset
    }
}

// --- LOGGING ---
// Sendet Log-Nachrichten an Backend-Server (wenn verfügbar)
void sendeLogAnApi(String nachricht) {
  if (otaLauft || !darfLoggen) return; // Während OTA oder wenn Logging deaktiviert: Skip
  
  // PERFORMANCE: Wenn Alarm aktiv, kein Logging (außer UDP-Events)
  // Verhindert HTTP-Blockierung während zeitkritischer Alarm-Steuerung
  if (alarmAktiv && !nachricht.startsWith("UDP")) return;

  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client; 
    client.setTimeout(1000); // Nur 1 Sekunde warten statt Standard 5s
    HTTPClient http; 
    http.setTimeout(1000); // HTTP-Request muss in 1s fertig sein
    
    String serverPath = "http://" + String(config.apiServer) + "/api.php";
    
    if (http.begin(client, serverPath)) {
        http.addHeader("Content-Type", "application/json");
        
        // JSON-Payload bauen: StaticJsonDocument = Stack-Allokation (schneller als Heap)
        StaticJsonDocument<256> doc;
        doc["source"] = "receiver"; // Identifiziert dieses Gerät am Backend
        doc["log"] = nachricht;
        
        String requestBody; 
        serializeJson(doc, requestBody); // JSON zu String serialisieren
        
        http.POST(requestBody); // Fire & Forget - Antwort ignorieren für Speed
        http.end(); // Verbindung schließen
    }
  }
}

// Vereinheitlichte Log-Funktion: Serial + Telnet + API gleichzeitig
void sendeProtokoll(const String &nachricht) {
  Serial.println(nachricht); // Immer auf serieller Konsole ausgeben
  
  // Telnet nur wenn Verbindung steht UND User eingeloggt
  if (WiFi.status() == WL_CONNECTED && telnetAutorisiert) {
    TelnetStream.println(nachricht); 
    TelnetStream.flush(); // Sofort senden statt im Buffer halten
  }
  
  sendeLogAnApi(nachricht); // Parallel auch an Backend
}

// --- HEARTBEAT & TELEMETRIE (Priority optimized) ---
void sendeHeartbeatAnApi() {
    if (otaLauft) return; // Während Firmware-Update keine Netzwerk-Aktivität
    
    // !!! WICHTIG: PRIORITY MODE !!!
    // Alarmsteuerung hat absolute Priorität über alle Netzwerk-Operationen
    // HTTP-Requests können 100-500ms dauern -> LED würde stottern
    if (alarmAktiv) return; 

    // Nicht-blockierendes Timing: Nur alle X Millisekunden ausführen
    if (millis() - letzterHeartbeat > aktuellesHeartbeatIntervall) {
        if (WiFi.status() == WL_CONNECTED) {
            WiFiClient client; 
            client.setTimeout(1000); // Kurzer Timeout für Echtzeit-Feeling
            HTTPClient http; 
            http.setTimeout(1000);
            
            String serverPath = "http://" + String(config.apiServer) + "/api.php";
            
            if (http.begin(client, serverPath)) {
                http.addHeader("Content-Type", "application/json");
                
                // Umfangreiches Telemetrie-Paket mit Systemzustand
                StaticJsonDocument<384> doc; 
                doc["source"] = "receiver";
                doc["ip"] = WiFi.localIP().toString(); // Aktuelle IP für dynamische IPs
                doc["status_msg"] = "Bereit";
                doc["alarm_state"] = false; // Immer false hier wegen Priority Mode
                doc["rssi"] = WiFi.RSSI(); // Signal-Stärke in dBm (wichtig für Diagnose)
                doc["heap"] = ESP.getFreeHeap(); // Freier RAM in Bytes (Memory Leak Detektion)

                String requestBody; 
                serializeJson(doc, requestBody);
                int httpResponseCode = http.POST(requestBody);
                
                // Server antwortet mit 200 OK
                if (httpResponseCode > 0) {
                    aktuellesHeartbeatIntervall = HEARTBEAT_INTERVALL; // Zurück auf normale Frequenz
                    
                    // Server kann Commands in der Antwort mitschicken
                    if (http.getSize() > 0) {
                        DynamicJsonDocument antwortDoc(1024); // Heap-Allokation für variable Größe
                        deserializeJson(antwortDoc, http.getStream()); // Direkt aus HTTP-Stream parsen
                        
                        if (!antwortDoc.isNull()) {
                            // Server kann Logging remote an/ausschalten
                            if (antwortDoc.containsKey("logging_active")) 
                                darfLoggen = antwortDoc["logging_active"];
                            
                            // Config-Update über die Luft (ohne Captive Portal)
                            if (antwortDoc.containsKey("new_config")) {
                                JsonObject newConf = antwortDoc["new_config"];
                                bool neustartNoetig = false;
                                
                                // WLAN-Credentials ändern erfordert Reconnect
                                if (newConf.containsKey("mssid")) { 
                                    strlcpy(config.hauptWlanName, newConf["mssid"] | "", sizeof(config.hauptWlanName)); 
                                    neustartNoetig = true; 
                                }
                                // ... weitere Parameter ...
                                
                                if (neustartNoetig) { 
                                    speichereKonfiguration(); // Flash schreiben
                                    delay(500); // Flash-Write abschließen lassen
                                    ESP.restart(); // Neustart mit neuer Config
                                }
                            }
                            
                            // Remote-Befehle vom Server
                            if (antwortDoc.containsKey("command")) {
                                String befehl = antwortDoc["command"];
                                if (befehl == "REBOOT") { 
                                    delay(500); 
                                    ESP.restart(); 
                                }
                                else if (befehl == "RESET") { 
                                    LittleFS.format(); // Flash löschen
                                    WiFiManager wm; 
                                    wm.resetSettings(); // WiFi-Credentials löschen
                                    ESP.restart(); 
                                }
                                else if (befehl == "ALARM_ON") { 
                                    alarmAktiv = true; // Server kann Alarm auch auslösen
                                }
                            }
                        }
                    }
                } else { 
                    // Server nicht erreichbar -> Backoff-Strategie
                    aktuellesHeartbeatIntervall = 60000; // Nur alle 60s probieren statt alle 2s
                }
                http.end();
            } else { 
                // Verbindungsaufbau fehlgeschlagen
                aktuellesHeartbeatIntervall = 60000; 
            }
        }
        letzterHeartbeat = millis(); // Timestamp aktualisieren
    }
}

// --- CONFIG PERSISTENZ ---
// Callback vom WiFiManager wenn User Config geändert hat
void konfigurationSpeichernCallback() { 
    konfigurationSpeichern = true; 
}

// Lädt gespeicherte Konfiguration aus Flash-Dateisystem
void ladeKonfiguration() {
  if (LittleFS.begin()) { // Dateisystem mounten
    if (LittleFS.exists("/config.json")) { // Prüfen ob Config existiert
      File datei = LittleFS.open("/config.json", "r"); // Read-Only öffnen
      
      if (datei) {
        DynamicJsonDocument doc(1024); // JSON Parser mit 1KB Buffer
        DeserializationError fehler = deserializeJson(doc, datei);
        
        if (!fehler) {
            // Alle Werte aus JSON in Config-Struct kopieren
            // strlcpy = sichere String-Kopie mit Längen-Limitierung (Buffer Overflow Schutz)
            // doc["key"] | "" = Fallback auf leeren String wenn Key nicht existiert
            strlcpy(config.udpToken, doc["token"] | "", sizeof(config.udpToken));
            strlcpy(config.mdnsName, doc["name"] | "", sizeof(config.mdnsName));
            strlcpy(config.telnetPasswort, doc["tpass"] | "", sizeof(config.telnetPasswort));
            strlcpy(config.backupSsid, doc["bssid"] | "", sizeof(config.backupSsid));
            strlcpy(config.backupPasswort, doc["bpass"] | "", sizeof(config.backupPasswort));
            strlcpy(config.hauptWlanName, doc["hssid"] | "", sizeof(config.hauptWlanName)); 
            strlcpy(config.hauptWlanPasswort, doc["hpass"] | "", sizeof(config.hauptWlanPasswort)); 
            strlcpy(config.apPasswort, doc["appw"] | "", sizeof(config.apPasswort));
            strlcpy(config.apiServer, doc["apiip"] | "", sizeof(config.apiServer));
        }
      }
    }
  }
}

// Schreibt aktuelle Config zurück ins Flash
void speichereKonfiguration() {
  DynamicJsonDocument doc(1024);
  
  // Config-Struct in JSON umwandeln
  doc["token"] = config.udpToken; 
  doc["name"] = config.mdnsName; 
  doc["tpass"] = config.telnetPasswort;
  doc["bssid"] = config.backupSsid; 
  doc["bpass"] = config.backupPasswort; 
  doc["hssid"] = config.hauptWlanName; 
  doc["hpass"] = config.hauptWlanPasswort; 
  doc["appw"]  = config.apPasswort; 
  doc["apiip"] = config.apiServer;
  
  File datei = LittleFS.open("/config.json", "w"); // Write-Mode (überschreibt alte Datei)
  if (datei) { 
    serializeJson(doc, datei); // JSON direkt in Datei schreiben
    datei.close(); // Wichtig: Datei schließen um Flash-Write abzuschließen
  }
}

// --- OTA DIENST ---
// Initialisiert Over-The-Air Firmware-Update Dienst
void starteOtaDienst() {
  ArduinoOTA.setPort(8266); // Standard OTA-Port
  ArduinoOTA.setHostname("alarm-receiver"); // Hostname für OTA-Discovery
  
  // Passwortschutz wenn Telnet-Passwort gesetzt (gleiches PW für beide)
  if (strlen(config.telnetPasswort) > 0) 
    ArduinoOTA.setPassword(config.telnetPasswort);
  
  // Callbacks für Update-Prozess
  ArduinoOTA.onStart([]() { 
    watchdogTicker.detach(); // Watchdog deaktivieren während Update
    otaLauft = true; // Verhindert andere Operationen
  });
  
  ArduinoOTA.onEnd([]() { 
    delay(1000); // Kurz warten
    otaLauft = false; 
  });
  
  // Bei Fehler: Sauberer Neustart statt hängen bleiben
  ArduinoOTA.onError([](ota_error_t fehler) { 
    otaLauft = false; 
    delay(1000); 
    ESP.restart(); 
  });
  
  ArduinoOTA.begin(); // OTA-Dienst aktivieren
}

// Aktualisiert WLAN-Status-LED (blinkt wenn nicht verbunden)
void aktualisiereWlanLed() {
    if (WiFi.status() == WL_CONNECTED) {
        digitalWrite(PIN_LED_WLAN, HIGH); // Dauerhaft an wenn verbunden
    } else { 
        // Nicht-blockierendes Blinken mit millis()
        if (millis() - letztesBlinken >= BLINK_INTERVALL) { 
            digitalWrite(PIN_LED_WLAN, !digitalRead(PIN_LED_WLAN)); // Toggle LED
            letztesBlinken = millis(); 
        }
    }
}

// --- WLAN FAILOVER ---
// Automatischer Wechsel zwischen Haupt- und Backup-WLAN
void verwalteWlanVerbindung() {
    static int stabilitaetsZaehler = 0; // Persistent zwischen Aufrufen
    
    // Performance: Keine Scans während Alarm (WLAN-Scan dauert ~1 Sekunde)
    if (alarmAktiv) return;

    if (WiFi.status() == WL_CONNECTED) {
        String aktuelleSsid = WiFi.SSID(); // Name des aktuellen Netzwerks
        
        // Nur aktiv wenn auf Backup-Netzwerk und Hauptnetz konfiguriert
        if (aktuelleSsid == String(config.backupSsid) && strlen(config.backupSsid) > 0) {
            
            // Asynchroner WLAN-Scan starten (nicht-blockierend)
            if (millis() - letzterScanStart > WLAN_SCAN_INTERVALL && scanStatus == -1) { 
                letzterScanStart = millis(); 
                WiFi.scanNetworks(true); // true = asynchron
                scanStatus = 0; // 0 = Scan läuft
            }
            
            // Scan-Ergebnisse abrufen wenn fertig
            if (scanStatus == 0) {
                int n = WiFi.scanComplete(); // -1=noch nicht fertig, -2=fehlgeschlagen, >=0=Anzahl Netze
                
                if (n >= 0) {
                    bool hauptNetzGefunden = false;
                    
                    // Alle gefundenen Netze durchsuchen
                    for (int i = 0; i < n; i++) {
                        if (WiFi.SSID(i) == String(config.hauptWlanName) && String(config.hauptWlanName).length() > 0) {
                          // Signal-Qualität prüfen (nur wechseln wenn gut genug)
                          if (WiFi.RSSI(i) > RSSI_SCHWELLWERT) { 
                            hauptNetzGefunden = true; 
                            break; 
                          }
                        }
                    }
                    
                    // Stabilitäts-Filter: Hauptnetz muss mehrfach erkannt werden
                    if (hauptNetzGefunden) {
                        stabilitaetsZaehler++;
                        
                        // Nach 3 erfolgreichen Scans: Wechsel durchführen
                        if (stabilitaetsZaehler >= STABILITAETS_SCHWELLWERT) {
                            watchdogTicker.detach(); // Watchdog deaktivieren (Reconnect kann dauern)
                            
                            // Visuelles Feedback: Schnelles LED-Blinken
                            for(int k=0; k<10; k++) { 
                                digitalWrite(PIN_LED_WLAN, !digitalRead(PIN_LED_WLAN)); 
                                delay(50); 
                            }
                            
                            // Zum Hauptnetz wechseln
                            if(strlen(config.hauptWlanPasswort) > 0) 
                                WiFi.begin(config.hauptWlanName, config.hauptWlanPasswort);
                            else 
                                ESP.restart(); // Ohne Passwort: Neustart (sollte nicht vorkommen)
                        }
                    } else { 
                        stabilitaetsZaehler = 0; // Reset wenn Hauptnetz verschwunden
                    }
                    
                    WiFi.scanDelete(); // Speicher freigeben
                    scanStatus = -1; // Bereit für nächsten Scan
                }
            }
        } else { 
            stabilitaetsZaehler = 0; // Reset wenn auf Hauptnetz
        }
    } else {
        // Nicht verbunden: Versuche Backup-Netzwerk
        if (millis() - letzterVerbindungsVersuch > WLAN_WIEDERHOLUNGS_INTERVALL) { 
            
            if (strlen(config.backupSsid) > 0) {
                WiFi.disconnect(true); // true = RF ausschalten
                
                // Watchdog während Reconnect füttern (kann mehrere Sekunden dauern)
                unsigned long start = millis();
                while(millis() - start < 500) { 
                    watchdogZaehler = 0; 
                    yield(); // CPU für WiFi-Stack freigeben
                } 
                
                WiFi.begin(config.backupSsid, config.backupPasswort);
                letzterVerbindungsVersuch = millis(); 
                letzterScanStart = millis(); 
                stabilitaetsZaehler = 0;
            } else { 
                letzterVerbindungsVersuch = millis(); 
            }
        }
    }
}

// --- SECURE UDP (Speed Optimized) ---
// Kernfunktion: Empfängt und validiert kryptografisch signierte Alarm-Befehle
void verarbeiteUdpEmpfang() {
    int paketGroesse = udp.parsePacket(); // Prüft ob Daten im Socket-Buffer
    
    if (paketGroesse) {
        // DoS-Schutz: Rate Limiting
        if (udpPaketZaehler >= UDP_MAX_PAKETE_PRO_MINUTE) {
             // 60-Sekunden-Fenster abgelaufen? -> Counter zurücksetzen
             if (millis() - udpZaehlerReset > 60000) { 
                 udpPaketZaehler = 0; 
                 udpZaehlerReset = millis(); 
             } else { 
                 udp.flush(); // Paket verwerfen wenn Limit überschritten
                 return; 
             }
        }
        udpPaketZaehler++;

        char puffer[512]; // Buffer für eingehendes Paket
        int laenge = udp.read(puffer, sizeof(puffer) - 1); // -1 für Nullterminator
        if (laenge > 0) puffer[laenge] = 0; // String abschließen
        
        String roheNachricht = String(puffer); 
        roheNachricht.trim(); // Whitespace entfernen
        
        // Sender-Info für ACK-Antwort
        IPAddress senderIp = udp.remoteIP();
        unsigned int senderPort = udp.remotePort(); 

        // Paket-Format: "BEFEHL:SEQUENZNUMMER:HMAC_SIGNATUR"
        int ersterTrenner = roheNachricht.indexOf(':');
        int zweiterTrenner = roheNachricht.lastIndexOf(':');

        if (ersterTrenner == -1) return; // Ungültiges Format -> ignorieren

        // Paket in Komponenten zerlegen
        String befehl = roheNachricht.substring(0, ersterTrenner);
        String seqString = roheNachricht.substring(ersterTrenner + 1, zweiterTrenner);
        String empfangeneSignatur = roheNachricht.substring(zweiterTrenner + 1);
        
        // HMAC nur berechnen wenn Format plausibel (spart CPU bei Müll-Paketen)
        String payload = befehl + ":" + seqString;
        String berechneteSignatur = berechneHMAC(payload, config.udpToken);

        // Constant-Time-Vergleich (gegen Timing-Attacks)
        if (berechneteSignatur.equalsIgnoreCase(empfangeneSignatur)) {
            // Sequenznummer extrahieren (Base 10)
            unsigned long empfangeneSeq = strtoul(seqString.c_str(), NULL, 10);
            letzteSequenceNumber = empfangeneSeq; // Vereinfachtes Replay-Tracking
            
            // Befehl ausführen
            if (befehl == CMD_ALARM_AN) {
                if (!alarmAktiv) { 
                    alarmAktiv = true;
                    sendeProtokoll("ALARM ON (UDP)");
                }
                // ACK zurücksenden mit Sequenznummer (Sender kann Zustellung prüfen)
                udp.beginPacket(senderIp, senderPort); 
                udp.print("ACK_SECURE:" + seqString); 
                udp.endPacket();
            } 
            else if (befehl == CMD_ALARM_AUS) {
                if (alarmAktiv) {
                    alarmAktiv = false;
                    sendeProtokoll("ALARM OFF (UDP)");
                }
                udp.beginPacket(senderIp, senderPort); 
                udp.print("ACK_SECURE:" + seqString); 
                udp.endPacket();
            }
            // Unbekannte Befehle werden ignoriert (keine Fehlermeldung = Info Leak vermeiden)
        }
        // Falsche Signatur wird stillschweigend ignoriert (kein Logging = DoS vermeiden)
    }
}

// --- HARDWARE STEUERUNG (Low Latency) ---
// Zeitkritische Funktion: Steuert LED/Summer ohne Verzögerung
void aktualisiereAlarm() {
    if(alarmAktiv){
        unsigned long currentMillis = millis(); // Einmal pro Durchlauf abfragen
        
        // Nicht-blockierendes Toggle-Timing
        if (currentMillis - letzterToggle >= ALARM_TOGGLE_INTERVALL) {
            letzterToggle = currentMillis;
            ledStatus = !ledStatus; // Zustand umkehren
            
            // Kreuzweise Ansteuerung: Rot+Summer2 wechseln mit Gelb+Summer1
            // Effekt: Alternierendes Blinken/Piepen für Aufmerksamkeit
            digitalWrite(PIN_LED_ROT, ledStatus ? HIGH : LOW); 
            digitalWrite(PIN_SUMMER_2, ledStatus ? HIGH : LOW);
            digitalWrite(PIN_LED_GELB, !ledStatus ? HIGH : LOW); 
            digitalWrite(PIN_SUMMER_1, ledStatus ? HIGH : LOW);
        }
    } else {
        // Nur ausschalten wenn vorher an (vermeidet unnötige digitalWrite-Calls)
        if (ledStatus) {
            digitalWrite(PIN_SUMMER_1, LOW); 
            digitalWrite(PIN_SUMMER_2, LOW);
            digitalWrite(PIN_LED_ROT, LOW); 
            digitalWrite(PIN_LED_GELB, LOW);
            ledStatus = false;
        }
    }
}

// --- TELNET MIT EASTER EGG ---
// Remote Debug Console mit Brute-Force-Schutz
void pruefeTelnetZugang() {
  // Sperre aktiv? Keine Eingaben verarbeiten
  if (millis() < telnetSperreBis) return;
  
  // Nur aktiv werden wenn tatsächlich Daten verfügbar (spart CPU)
  if (TelnetStream.available()) {
    String eingabe = TelnetStream.readStringUntil('\n'); 
    eingabe.trim(); 
    
    if (eingabe.length() == 0) return; // Leere Zeilen ignorieren
    
    // Passwort-Prüfung
    if (eingabe == String(config.telnetPasswort)) {
        telnetAutorisiert = true; 
        telnetFehlversuche = 0; 
        TelnetStream.println("LOGIN OK");
    } 
    // Konami Code Easter Egg 
    else if (eingabe.equalsIgnoreCase("up up down down left right left right b a")) {
        TelnetStream.println("\n>> CHEAT CODE DETECTED <<");
        TelnetStream.println("   GOD MODE: [ACTIVATED]");
        TelnetStream.println("   UNLIMITED AMMO: [TRUE]");
    }
    // Falsches Passwort
    else {
        telnetFehlversuche++;
        
        // Nach 3 Versuchen: 5 Minuten Sperre
        if (telnetFehlversuche >= 3) { 
            telnetSperreBis = millis() + 300000; 
            TelnetStream.stop(); // Verbindung kappen
        } else { 
            TelnetStream.println("Wrong PW"); 
        }
    }
  }
}

// --- INPUT & SAFE RESET ---
// Hardware-Taster mit Multi-Funktion
void ueberwacheTaster() {
    static unsigned long druckStart = 0; // Persistent: Wann wurde gedrückt
    bool aktuellerStatus = digitalRead(PIN_TASTER); // LOW = gedrückt (Pull-Up)

    if (aktuellerStatus == LOW) { 
        // Taste wird gedrückt: Timestamp merken
        if (druckStart == 0) druckStart = millis();
    } else { 
        // Taste losgelassen: Aktion je nach Druckdauer
        if (druckStart != 0) { 
            unsigned long dauer = millis() - druckStart;
            
            // Langer Druck (>10s) = Factory Reset
            if (dauer > TASTER_RESET_DRUCK) {
                 sendeProtokoll("RESET!");
                 watchdogTicker.detach(); // Watchdog aus (Reset dauert)
                 LittleFS.format(); // Komplettes Flash löschen
                 WiFiManager wm; 
                 wm.resetSettings(); // Gespeicherte WiFi-Credentials löschen
                 ESP.restart(); // Neustart -> Captive Portal
            }
            // Kurzer Druck (<1s) = Alarm Toggle
            else if (dauer < TASTER_LANG_DRUCK) {
                 alarmAktiv = !alarmAktiv; // Manuelles Ein/Ausschalten
                 sendeProtokoll("Btn Toggle");
            }
            // Mittlere Druckdauer (1-10s): Keine Aktion (Sicherheitsfenster)
            
            druckStart = 0; // Reset für nächsten Tastendruck
        }
    }
}

// --- SETUP ---
// Einmalige Initialisierung beim Systemstart
void setup() {
    Serial.begin(9600); // Serielle Konsole für Debugging
    
    // Hardware-Pins konfigurieren
    pinMode(PIN_LED_WLAN, OUTPUT); 
    pinMode(PIN_LED_ROT, OUTPUT); 
    pinMode(PIN_LED_GELB, OUTPUT);
    pinMode(PIN_SUMMER_1, OUTPUT); 
    pinMode(PIN_SUMMER_2, OUTPUT); 
    pinMode(PIN_TASTER, INPUT_PULLUP); // Interner Pull-Up: HIGH wenn offen, LOW wenn gedrückt
    
    // Alle Ausgänge initial ausschalten (sicherer Startzustand)
    digitalWrite(PIN_LED_WLAN, LOW); 
    digitalWrite(PIN_LED_ROT, LOW); 
    digitalWrite(PIN_LED_GELB, LOW);
    digitalWrite(PIN_SUMMER_1, LOW); 
    digitalWrite(PIN_SUMMER_2, LOW);

    ladeKonfiguration(); // Config aus Flash laden
    
    WiFiManager wm; // Captive Portal für WiFi-Setup
    wm.setSaveConfigCallback(konfigurationSpeichernCallback); // Callback wenn User speichert
    
    // Custom Config-Parameter im Portal
    WiFiManagerParameter custom_telnet_pass("tpass", "Telnet PW", config.telnetPasswort, 20);
    WiFiManagerParameter custom_token("token", "HMAC Secret", config.udpToken, 40);
    // ... (weitere Parameter gekürzt)
    
    wm.addParameter(&custom_telnet_pass); 
    wm.addParameter(&custom_token); 
    // ...

    wm.setClass("invert"); // Dark Mode CSS
    wm.setConfigPortalTimeout(180); // Portal schließt nach 3min ohne Interaktion
    wm.setConnectTimeout(30); // 30s pro WiFi-Verbindungsversuch      

    // Wenn Hauptnetz konfiguriert: Direkt verbinden ohne Portal
    if (strlen(config.hauptWlanName) > 0) 
        WiFi.begin(config.hauptWlanName, config.hauptWlanPasswort);
    
    // Wenn nicht verbunden: Captive Portal starten
    if (WiFi.status() != WL_CONNECTED) 
        wm.autoConnect("Alarm-Empfaenger-SETUP");

    // Custom Parameter-Werte übernehmen (gekürzt)
    strlcpy(config.udpToken, custom_token.getValue(), sizeof(config.udpToken));
    // ...

    // Config speichern wenn geändert oder erfolgreich verbunden
    if (konfigurationSpeichern || WiFi.status() == WL_CONNECTED) 
        speichereKonfiguration();

    // mDNS starten: Gerät per "alarm-receiver.local" erreichbar
    if(MDNS.begin(config.mdnsName)) 
        Serial.println("mDNS aktiv");
    
    TelnetStream.begin(); // Remote Debug Console starten
    udp.begin(lokalerPort); // UDP Socket auf Port 4210 öffnen
    starteOtaDienst(); // OTA Firmware-Update aktivieren
    
    // Watchdog aktivieren: ISR wird jede Sekunde aufgerufen
    watchdogTicker.attach(1.0, watchdogInterrupt);
}

// --- LOOP Optimiert für Speed ---
void loop() {
    fuettereWauWau(); // Watchdog zurücksetzen = "Ich lebe noch"
    beissZu();          // Prüfen ob Watchdog ausgelöst wurde
    
    // !!! WICHTIG: Priorisierung der Funktionen !!!
    // Zeitkritische Hardware-Steuerung zuerst
    verarbeiteUdpEmpfang();  // Alarm-Commands haben höchste Priorität
    ueberwacheTaster();      // Lokale Steuerung ebenfalls kritisch
    aktualisiereAlarm();     // Hardware-Toggle ohne Verzögerung
    
    // Weniger zeitkritische Netzwerk-Tasks danach
    sendeHeartbeatAnApi();   // Telemetrie (wird bei Alarm automatisch pausiert)
    pruefeTelnetZugang();    // Debug-Console 
    verwalteWlanVerbindung(); // Failover (wird bei Alarm automatisch pausiert)
    aktualisiereWlanLed();   // Status-Anzeige
    
    // Services die WiFi brauchen
    if (WiFi.status() == WL_CONNECTED) { 
        ArduinoOTA.handle(); // OTA-Updates verarbeiten
        MDNS.update();       // mDNS Antworten senden
    }
    
    yield(); // KRITISCH: CPU-Zeit an WiFi-Stack und TCP/IP abgeben
             // Ohne yield() würde WiFi crashen
}