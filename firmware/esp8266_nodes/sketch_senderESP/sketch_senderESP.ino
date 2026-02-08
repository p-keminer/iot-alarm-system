/*
 * PROJEKT: ESP8266 UDP Alarm-System (Sender)
 * ------------------------------------------
 * Beschreibung: Sendet kryptografisch signierte UDP-Pakete bei Tastendruck/Befehl.
 * Beinhaltet Sicherheits-Features und Priorisierungs-Logik für maximale Geschwindigkeit.
 *
 * Features & Funktionen:
 * ----------------------
 * 1. NETZWERK & PERFORMANCE
 * - Priority-Mode: Blockiert API-Calls, solange auf UDP-Antwort gewartet wird.
 * - Resultat: Sofortige Reaktion auf Tastendruck, auch wenn Server offline ist.
 * - WLAN Failover (Backup-SSID) & automatischer Reconnect.
 *
 * 2. SICHERHEIT (SECURITY HARDENING)
 * - HMAC-SHA256: Signiert jeden Befehl mit dem Secret Token.
 * - Anti-Replay: Zählt Sequenznummern hoch (Schutz vor Aufzeichnung).
 * - Traffic Obfuscation: Sendet "NICE_TRY_WIRESHARK_USER" statt "ALARM_ON".
 * - Telnet Härtung: Auto-Logout & Brute-Force-Sperre + Konami Code Easter Egg.
 *
 * 3. SYSTEM
 * - Watchdog V2: Ausgelagert ("fuettere_wau_wau"), überwacht Loop-Zyklus.
 * - Safe Reset: Werksreset nur bei bewusstem Loslassen des Tasters (>10s).
 * - Telemetrie: Sendet Vitaldaten (RSSI, Heap) an API.
 *
 * Hardware:   NodeMCU V2 (ESP8266)
 * Autor:      Philip Keminer
 * Version:    V10.0 (Sender - Final)
 * Datum:      2026-02-03
 */

// --- BIBLIOTHEKEN ---
#include <FS.h>                 // Dateisystem-Basisklasse für Abstraktionsschicht
#include <LittleFS.h>           // Flash-Dateisystem für Config-Persistierung
#include <ArduinoJson.h>        // JSON De/Serialisierung für Config und API-Kommunikation
#include <ESP8266WiFi.h>        // WiFi Stack mit TCP/IP Implementation
#include <WiFiManager.h>        // Captive Portal für initiale WLAN-Konfiguration
#include <WiFiUdp.h>            // User Datagram Protocol - verbindungslos für schnelle Commands
#include <ESP8266mDNS.h>        // Multicast DNS für lokale Namensauflösung (alarm-receiver.local)
#include <ESP8266HTTPClient.h>  // HTTP Client für REST API Kommunikation
#include <WiFiClient.h>         // Basis TCP Client Implementation
#include <ArduinoOTA.h>         // Over-The-Air Firmware Updates ohne USB
#include <TelnetStream.h>       // Remote Debugging Console über Telnet
#include <Ticker.h>             // Hardware-Timer für periodische Interrupts
#include <WiFiClientSecure.h>   // TLS/SSL Support für verschlüsselte Verbindungen
#include <bearssl/bearssl.h>    // Low-Level Krypto-Bibliothek für HMAC-SHA256

// --- KONFIGURATION ---
const char* DEVICE_NAME = "sender"; 

// --- TIMING ---
const unsigned long WLAN_WIEDERHOLUNGS_INTERVALL = 20000; // Nach Verbindungsabbruch: 20s warten
const unsigned long WLAN_SCAN_INTERVALL = 30000;          // Alle 30s nach besserem Hauptnetz scannen
const uint8_t STABILITAETS_SCHWELLWERT = 3;               // Hauptnetz muss 3x erkannt werden vor Wechsel
const int8_t RSSI_SCHWELLWERT = -75;                      // Signal muss besser als -75dBm sein
const unsigned long TASTER_RESET_DRUCK = 10000;           // Drücke über 10s = Factory Reset
const unsigned long BLINK_INTERVALL = 500;                // WLAN-LED blinkt alle 500ms
const int WATCHDOG_TIMEOUT_SEK = 30;                      // System rebootet nach 30s ohne Loop
const unsigned long SENDE_WIEDERHOLUNGS_INTERVALL = 1000; // Wiederhole UDP-Befehl jede Sekunde
const int MAX_SENDE_VERSUCHE = 10;                        // Max 10 Versuche dann Timeout
const unsigned long HEARTBEAT_INTERVALL = 2000;           // Telemetrie alle 2 Sekunden
const unsigned long TELNET_TIMEOUT = 300000;              // Telnet Auto-Logout nach 5min Inaktivität
const unsigned long IP_UPDATE_INTERVALL = 60000;          // Empfänger-IP alle 60s via mDNS aktualisieren

// --- SICHERHEIT ---
const uint8_t MAX_TELNET_VERSUCHE = 3;                    // Brute-Force-Schutz: 3 Fehlversuche = 5min Sperre

// --- OBFUSCATED PAYLOADS (Muss mit Empfänger übereinstimmen!) ---
// Diese Strings müssen identisch zum Empfänger sein, sonst werden Commands ignoriert
const char* CMD_ALARM_AN  = "NICE_TRY_WIRESHARK_USER";    // Verschleiert echten Befehl "ALARM_ON"
const char* CMD_ALARM_AUS = "ENCRYPTION_IS_YOUR_FRIEND";  // Verschleiert echten Befehl "ALARM_OFF"

// --- PINS ---
#define PIN_RESET_TASTER D3      // Hardware-Reset-Taster (>10s = Factory Reset)
#define PIN_LED_ALARM LED_BUILTIN // Interne LED: LOW=Alarm aktiv, HIGH=aus
#define PIN_LED_WLAN D5          // Status-LED für WiFi-Verbindung

// --- DATENSTRUKTUR ---
// Alle persistenten Einstellungen in einer Struktur
struct SystemKonfiguration {
    char udpToken[41] = "";             // 40 Zeichen HMAC Secret + Nullterminator
    char mdnsZiel[33] = "alarm-receiver"; // mDNS-Hostname des Empfängers
    char apiServer[33] = "192.168.178.50"; // Backend-Server IP/Hostname
    char telnetPasswort[21] = "admin";     // Passwort für Debug-Konsole
    char backupSsid[33] = "";              // Fallback-WLAN bei Hauptnetz-Ausfall
    char backupPasswort[65] = "";          // WPA2 kann bis 64 Zeichen
    char hauptWlanName[33] = "";           // Primäres WLAN mit bester Performance
    char hauptWlanPasswort[65] = "";    
    char apPasswort[65] = "12345678";      // Setup-Access-Point Passwort (min 8 Zeichen)
};

SystemKonfiguration config; // Globale Instanz der Konfiguration

// --- GLOBALE VARIABLEN ---
bool konfigurationSpeichern = false;    // Flag: Wurde Config im WiFiManager geändert?
bool telnetAutorisiert = false;         // Zugriffskontrolle für Debug-Konsole

// Timing-Variablen für nicht-blockierende Operationen
unsigned long letzterVerbindungsVersuch = 0; // Timestamp für Reconnect-Logik
unsigned long letzterScanStart = 0;           // Timestamp für WLAN-Scan
int scanStatus = -1;                          // -1=Idle, 0=Läuft, >0=Ergebnisse

// UDP & Security
String ausstehendeNachricht = "";       // Befehl der gerade gesendet wird (mit Signatur)
bool wartetAufBestatigung = false;      // CRITICAL: Blockiert HTTP während UDP-Transaktion
unsigned long letzterSendeZeitpunkt = 0; // Timestamp für Retry-Timing
int wiederholungsZaehler = 0;           // Zählt Wiederholungsversuche
unsigned long sequenceNumber = 0;       // Anti-Replay: Zählt bei jedem Befehl hoch

// Netzwerk
IPAddress zielIpAdresse;                // IP des Empfängers (via mDNS ermittelt)
const unsigned int zielPort = 4210;     // UDP-Port des Empfängers
const unsigned int lokalerPort = 4211;  // Eigener UDP-Port (muss unterschiedlich sein)
WiFiUDP udp;                            // UDP Socket

// Status
unsigned long letztesBlinken = 0;       // Timestamp für WLAN-LED Blinken
unsigned long letzteIpAktualisierung = 0; // Timestamp für mDNS-Refresh
unsigned long letzterHeartbeat = 0;      // Timestamp für letzte Telemetrie
unsigned long aktuellesHeartbeatIntervall = 2000; // Dynamisch: 2s normal, 60s bei Fehler
unsigned long letzterTelnetInput = 0;    // Timestamp für Session-Timeout

bool darfLoggen = false;                // Server-gesteuertes Logging
bool otaLauft = false;                  // Verhindert andere Operationen während Update
uint8_t telnetFehlversuche = 0;         // Zählt Falsch-Logins
unsigned long telnetSperreBis = 0;      // Timestamp bis wann Telnet gesperrt

// Watchdog
Ticker watchdogTicker;                  // Hardware-Timer feuert jede Sekunde
volatile int watchdogZaehler = 0;       // Zählt hoch wenn Loop hängt
volatile bool mussNeustarten = false;   // Flag für sauberen Reboot

// --- VORDEKLARATIONEN ---
// Funktionen die vor ihrer Definition aufgerufen werden
void sendeLogAnApi(String nachricht);
void sendeHeartbeatAnApi();

// --- KRYPTO HELFER (C-API) ---
// HMAC-SHA256: Hash-based Message Authentication Code
// Erstellt digitale Signatur die nur mit dem Secret Key erzeugt werden kann
String berechneHMAC(String nachricht, String secret) {
    br_hmac_key_context kc;  // Key-Kontext speichert Hash-Algorithmus und Secret
    br_hmac_context ctx;      // Hash-Kontext für Berechnung
    
    // 1. Key-Kontext mit SHA256 und Secret initialisieren
    br_hmac_key_init(&kc, &br_sha256_vtable, secret.c_str(), secret.length());
    
    // 2. Hash-Kontext mit Key verknüpfen
    br_hmac_init(&ctx, &kc, 0);
    
    // 3. Nachricht hashen (kann mehrfach für große Daten aufgerufen werden)
    br_hmac_update(&ctx, nachricht.c_str(), nachricht.length());
    
    // 4. Hash finalisieren - SHA256 = 32 Bytes Output
    uint8_t result[32];
    br_hmac_out(&ctx, result);
    
    // 5. Binäre Bytes in Hexadezimal-String umwandeln (für UDP-Übertragung)
    String hexString = "";
    for (int i = 0; i < 32; i++) {
        if (result[i] < 16) hexString += "0"; // Führende Null für 0x00-0x0F
        hexString += String(result[i], HEX);
    }
    return hexString; // Liefert z.B. "a3f5b9..." (64 Zeichen)
}

// --- WATCHDOG SYSTEM ---
// Interrupt Service Routine - wird jede Sekunde vom Hardware-Timer aufgerufen
void ICACHE_RAM_ATTR watchdogInterrupt() {
    watchdogZaehler++; // Zählt hoch wenn loop() nicht reagiert
    if (watchdogZaehler >= WATCHDOG_TIMEOUT_SEK) mussNeustarten = true;
}

// Watchdog zurücksetzen - beweist dass loop() noch läuft
// Rate-Limited: Max alle 100ms zurücksetzen (vermeidet unnötige Schreibzugriffe)
void fuettereWauWau() {
    static unsigned long letzterHappen = 0; // Persistent zwischen Aufrufen
    if (millis() - letzterHappen > 100) {   // Nur alle 100ms aktiv werden
         watchdogZaehler = 0; // Zähler nullen = System lebt noch
         letzterHappen = millis();
    }
}

// Prüft ob Watchdog ausgelöst wurde und führt Reboot durch
void beissZu(){
    if (mussNeustarten) {
        Serial.println("WATCHDOG RESET!"); // Letzte Nachricht vor Neustart
        delay(100); // Sicherstellen dass Nachricht gesendet wurde
        ESP.restart(); // Hardware-Reset
    }
}

// --- LOGGING ---
// Sendet Log-Nachrichten an Backend-Server (wenn verfügbar)
void sendeLogAnApi(String nachricht) {
  if (otaLauft || !darfLoggen) return; // Während OTA oder wenn deaktiviert: Skip
  
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client; 
    client.setTimeout(2000); // 2 Sekunden Timeout (etwas mehr als Receiver wegen Sender-Rolle)
    HTTPClient http; 
    http.setTimeout(2000);
    
    String serverPath = "http://" + String(config.apiServer) + "/api.php";
    
    if (http.begin(client, serverPath)) {
        http.addHeader("Content-Type", "application/json");
        
        // JSON-Payload bauen
        StaticJsonDocument<256> doc; // Stack-Allokation für Speed
        doc["source"] = DEVICE_NAME; // Identifiziert dieses Gerät als "sender"
        doc["log"] = nachricht;
        
        String requestBody; 
        serializeJson(doc, requestBody); // JSON zu String
        
        http.POST(requestBody); // Fire & Forget - Antwort ignorieren
        http.end(); // Verbindung schließen
    }
  }
}

// Vereinheitlichte Log-Funktion: Serial + Telnet + API gleichzeitig
void sendeProtokoll(const String &nachricht) {
  Serial.println(nachricht); // Immer auf serieller Konsole
  
  // Telnet nur wenn verbunden UND eingeloggt
  if (WiFi.status() == WL_CONNECTED && telnetAutorisiert) {
    TelnetStream.println(nachricht); 
    TelnetStream.flush(); // Sofort senden statt buffern
  }
  
  sendeLogAnApi(nachricht); // Parallel auch an Backend
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
      File datei = LittleFS.open("/config.json", "r"); // Read-Only
      
      if (datei) {
        DynamicJsonDocument doc(1024); // JSON Parser mit 1KB Buffer
        DeserializationError fehler = deserializeJson(doc, datei);
        
        if (!fehler) {
           // Alle Werte aus JSON in Config-Struct kopieren
           // strlcpy = sichere String-Kopie mit Längen-Check (Buffer Overflow Schutz)
           // doc["key"] | "" = Fallback auf leeren String wenn Key fehlt
           strlcpy(config.udpToken, doc["token"] | "", sizeof(config.udpToken));
           strlcpy(config.mdnsZiel, doc["ziel"] | "", sizeof(config.mdnsZiel));
           strlcpy(config.telnetPasswort, doc["tpass"] | "", sizeof(config.telnetPasswort));
           strlcpy(config.backupSsid, doc["bssid"] | "", sizeof(config.backupSsid));
           strlcpy(config.backupPasswort, doc["bpass"] | "", sizeof(config.backupPasswort));
           strlcpy(config.hauptWlanName, doc["hssid"] | "", sizeof(config.hauptWlanName)); 
           strlcpy(config.hauptWlanPasswort, doc["hpass"] | "", sizeof(config.hauptWlanPasswort)); 
           strlcpy(config.apPasswort, doc["appw"] | "", sizeof(config.apPasswort));            
           strlcpy(config.apiServer, doc["api"] | "", sizeof(config.apiServer));
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
  doc["ziel"] = config.mdnsZiel; 
  doc["tpass"] = config.telnetPasswort;
  doc["bssid"] = config.backupSsid; 
  doc["bpass"] = config.backupPasswort; 
  doc["hssid"] = config.hauptWlanName; 
  doc["hpass"] = config.hauptWlanPasswort; 
  doc["api"] = config.apiServer; 
  doc["appw"]  = config.apPasswort;
  
  File datei = LittleFS.open("/config.json", "w"); // Write-Mode (überschreibt)
  if (datei) { 
    serializeJson(doc, datei); // JSON direkt in Datei
    datei.close(); // Wichtig: Schließen für Flash-Write
  }
}

// --- OTA DIENST ---
// Initialisiert Over-The-Air Firmware-Update Dienst
void starteOtaDienst() {
  ArduinoOTA.setPort(8266); // Standard OTA-Port
  ArduinoOTA.setHostname("alarm-sender"); // Hostname für OTA-Discovery
  
  // Passwortschutz wenn Telnet-Passwort gesetzt
  if (strlen(config.telnetPasswort) > 0) 
    ArduinoOTA.setPassword(config.telnetPasswort);
  
  // Callbacks für Update-Prozess
  ArduinoOTA.onStart([]() { 
    watchdogTicker.detach(); // Watchdog deaktivieren (Update kann lange dauern)
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
void aktualisiereWlanLed(){
    if (WiFi.status() != WL_CONNECTED) {
        // Nicht-blockierendes Blinken mit millis()
        if (millis() - letztesBlinken >= BLINK_INTERVALL) { 
            digitalWrite(PIN_LED_WLAN, !digitalRead(PIN_LED_WLAN)); // Toggle
            letztesBlinken = millis(); 
        }
    } else { 
        digitalWrite(PIN_LED_WLAN, HIGH); // Dauerhaft an wenn verbunden
    }
}

// --- WLAN FAILOVER ---
// Automatischer Wechsel zwischen Haupt- und Backup-WLAN
void verwalteWlanVerbindung() {
    static int stabilitaetsZaehler = 0; // Persistent zwischen Aufrufen
    
    // CRITICAL Performance: Keine Scans während UDP-Transaktion!
    // WLAN-Scan dauert ~1 Sekunde und würde UDP-ACK verzögern
    if (wartetAufBestatigung) return;

    if (WiFi.status() == WL_CONNECTED) {
        String aktuelleSsid = WiFi.SSID(); // Name des aktuellen Netzwerks
        
        // Nur aktiv wenn auf Backup-Netzwerk und Hauptnetz konfiguriert
        if (aktuelleSsid == String(config.backupSsid) && strlen(config.backupSsid) > 0) {
            
            // Asynchroner WLAN-Scan starten (nicht-blockierend)
            if (millis() - letzterScanStart > WLAN_SCAN_INTERVALL && scanStatus == -1) { 
                letzterScanStart = millis(); 
                WiFi.scanNetworks(true); // true = asynchron im Hintergrund
                scanStatus = 0; // 0 = Scan läuft
            }
            
            // Scan-Ergebnisse abrufen wenn fertig
            if (scanStatus == 0) {
                int n = WiFi.scanComplete(); // -1=läuft noch, -2=Fehler, >=0=Anzahl
                
                if (n >= 0) {
                    bool hauptNetzGefunden = false;
                    
                    // Alle gefundenen Netze durchsuchen
                    for (int i = 0; i < n; i++) {
                        if (WiFi.SSID(i) == String(config.hauptWlanName) && String(config.hauptWlanName).length() > 0) {
                          // Signal-Qualität prüfen (nur wechseln wenn stark genug)
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
                             watchdogTicker.detach(); // Watchdog aus (Reconnect kann dauern)
                             
                             // Visuelles Feedback: Schnelles LED-Blinken
                             for(int k=0; k<10; k++) { 
                                 digitalWrite(PIN_LED_WLAN, !digitalRead(PIN_LED_WLAN)); 
                                 delay(50); 
                             }
                             
                             // Zum Hauptnetz wechseln
                             if(strlen(config.hauptWlanPasswort) > 0) 
                                 WiFi.begin(config.hauptWlanName, config.hauptWlanPasswort);
                             else 
                                 ESP.restart(); // Ohne Passwort: Neustart
                        }
                    } else { 
                        stabilitaetsZaehler = 0; // Reset wenn Hauptnetz weg
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
                
                // Watchdog während Reconnect füttern (dauert mehrere Sekunden)
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

// Aktualisiert IP-Adresse des Empfängers via mDNS
// mDNS löst "alarm-receiver.local" zu echter IP auf (funktioniert nur im lokalen Netz)
void aktualisiereZielIp() {
    // Nur alle 60s aktualisieren UND nur wenn noch keine gültige IP
    if (millis() - letzteIpAktualisierung < IP_UPDATE_INTERVALL && zielIpAdresse.toString() != "0.0.0.0") return; 
    
    // mDNS-Auflösung: .local Domain zu IP
    WiFi.hostByName((String(config.mdnsZiel) + ".local").c_str(), zielIpAdresse);
    letzteIpAktualisierung = millis();
    
    // Fallback: Wenn mDNS fehlschlägt -> Broadcast (erreicht alle im Netzwerk)
    if (zielIpAdresse.toString() == "0.0.0.0") 
        zielIpAdresse = IPAddress(255, 255, 255, 255);
}

// --- UDP (Secure) ---
// Verarbeitet eingehende UDP-Antworten (ACKs) vom Empfänger
void verarbeiteUdpAntworten() {
    int paketGroesse = udp.parsePacket(); // Prüft ob Daten im Socket-Buffer
    
    if (paketGroesse) {                                                      
        char puffer[255]; // Buffer für eingehendes Paket
        int laenge = udp.read(puffer, sizeof(puffer) - 1); // -1 für Nullterminator
        if (laenge > 0) puffer[laenge] = 0; // String abschließen
        
        String roheNachricht = String(puffer); 
        roheNachricht.trim(); // Whitespace entfernen
        
        // ACK-Format prüfen: "ACK_SECURE:SEQUENZNUMMER"
        String erwartet = "ACK_SECURE:" + String(sequenceNumber);
        
        // Nur akzeptieren wenn wir tatsächlich auf Antwort warten UND Sequenznummer passt
        if (wartetAufBestatigung && roheNachricht == erwartet) {            
            sendeProtokoll("Erfolg: Validiertes ACK erhalten!");
            
            // LED Feedback basierend auf dem gesendeten Befehl
            // LED_BUILTIN ist invertiert: LOW = an, HIGH = aus
            if (ausstehendeNachricht.startsWith(CMD_ALARM_AN)) 
                digitalWrite(PIN_LED_ALARM, LOW);  // Alarm aktiv = LED an
            else if (ausstehendeNachricht.startsWith(CMD_ALARM_AUS)) 
                digitalWrite(PIN_LED_ALARM, HIGH); // Alarm aus = LED aus
            
            // Transaktion erfolgreich abgeschlossen
            wartetAufBestatigung = false; 
            ausstehendeNachricht = ""; 
            wiederholungsZaehler = 0;                                                   
        }
    }
    
    // Timeout Handling: Wiederhole Befehl wenn keine Antwort
    if (wartetAufBestatigung && 
        wiederholungsZaehler < MAX_SENDE_VERSUCHE && 
        millis() - letzterSendeZeitpunkt >= SENDE_WIEDERHOLUNGS_INTERVALL) { 
        
        wiederholungsZaehler++; // Nächster Versuch
        sendeProtokoll("Wiederholung " + String(wiederholungsZaehler));                  
        
        // Befehl erneut senden (identisches Paket mit gleicher Signatur)
        if (zielIpAdresse.toString() != "0.0.0.0") {                                         
            udp.beginPacket(zielIpAdresse, zielPort); 
            udp.print(ausstehendeNachricht); 
            udp.endPacket();
        }
        letzterSendeZeitpunkt = millis();
    }
    
    // Maximale Versuche erreicht: Aufgeben
    if (wartetAufBestatigung && wiederholungsZaehler >= MAX_SENDE_VERSUCHE) {
        sendeProtokoll("FEHLER: Timeout - Empfänger antwortet nicht!"); 
        
        // Transaktion abbrechen (System wird weiter versuchen wenn User erneut drückt)
        wartetAufBestatigung = false; 
        ausstehendeNachricht = ""; 
        wiederholungsZaehler = 0;                                                   
    }
}

// --- COMMAND PARSER (Seriell) ---
// Verarbeitet Befehle von serieller Konsole (für Debugging/Testing)
void verarbeiteSerielleBefehle(){
    if (Serial.available()) {                  
        String befehl = Serial.readStringUntil('\n'); 
        befehl.trim(); // Whitespace entfernen
        
        // Nur bekannte Befehle akzeptieren
        if (befehl == "ALARM_ON" || befehl == "ALARM_OFF") {   
            sequenceNumber++; // Sequenznummer hochzählen (Replay-Schutz)
            
            // Klartext-Befehl zu obfuscated Command mappen
            String echterBefehl = (befehl == "ALARM_ON") ? CMD_ALARM_AN : CMD_ALARM_AUS;
            
            // HMAC-Signatur berechnen
            String payload = echterBefehl + ":" + String(sequenceNumber);
            String signatur = berechneHMAC(payload, config.udpToken);
            
            // Komplettes signiertes Paket: "OBFUSCATED_CMD:SEQ:HMAC"
            ausstehendeNachricht = payload + ":" + signatur;
            
            // Sende-State aktivieren (blockiert nun HTTP-Requests)
            wartetAufBestatigung = true; 
            wiederholungsZaehler = 0; 
            letzterSendeZeitpunkt = millis();                                      
            
            // IP aktualisieren (falls Empfänger neu gestartet hat)
            aktualisiereZielIp(); 
            
            // Erstes UDP-Paket senden
            udp.beginPacket(zielIpAdresse, zielPort); 
            udp.print(ausstehendeNachricht); 
            udp.endPacket();
        }
        // Unbekannte Befehle werden ignoriert
    }
}

// --- TELNET MIT EASTER EGG ---
// Remote Debug Console mit Auto-Logout und Brute-Force-Schutz
void pruefeTelnetZugang() {
  // Sperre aktiv? Keine Eingaben verarbeiten
  if (millis() < telnetSperreBis) return;
  
  // Auto-Logout nach 5 Minuten Inaktivität
  if (telnetAutorisiert && (millis() - letzterTelnetInput > TELNET_TIMEOUT)) {
      telnetAutorisiert = false; 
      TelnetStream.println("\n--- AUTO LOGOUT ---");
  }
  
  // Nur aktiv werden wenn Daten verfügbar (spart CPU)
  if (TelnetStream.available()) {
    letzterTelnetInput = millis(); // Activity Timestamp aktualisieren
    
    String eingabe = TelnetStream.readStringUntil('\n'); 
    eingabe.trim(); 
    
    if (eingabe.length() == 0) return; // Leere Zeilen ignorieren

    // Passwort-Prüfung
    if (eingabe == String(config.telnetPasswort)) {
        telnetAutorisiert = true; 
        telnetFehlversuche = 0; 
        TelnetStream.println("LOGIN OK");
    } 
    // Konami Code Easter Egg (nostalgischer Videospiel-Cheat)
    else if (eingabe.equalsIgnoreCase("up up down down left right left right b a")) {
        TelnetStream.println("\n>> CHEAT CODE DETECTED <<");
        TelnetStream.println("   GOD MODE: [FAKE ENABLED]");
        // Tut natürlich nichts, nur für die Kultur
    }
    // Manuelles Logout
    else if (eingabe == "logout") {
        telnetAutorisiert = false; 
        TelnetStream.println("Ausgeloggt.");
    } 
    // Falsches Passwort
    else {
        telnetFehlversuche++;
        
        // Nach 3 Versuchen: 5 Minuten Sperre
        if (telnetFehlversuche >= MAX_TELNET_VERSUCHE) {
            telnetSperreBis = millis() + 300000; 
            TelnetStream.stop(); // Verbindung kappen
        } else { 
            TelnetStream.println("Falsches PW"); 
        }
    }
  }
}

// --- SAFE RESET (Hardware) ---
// Hardware-Taster für Factory Reset mit visueller Feedback
void pruefePhysischenReset() {
    static unsigned long druckStart = 0; // Persistent: Wann wurde gedrückt
    
    // Taste gedrückt (LOW = gedrückt wegen Pull-Up)
    if (digitalRead(PIN_RESET_TASTER) == LOW) {
        if (druckStart == 0) druckStart = millis(); // Timestamp merken
        
        // Visuelles Feedback während Taste gedrückt: LED blinkt
        // millis() % 200 erzeugt Werte 0-199, daraus binäres Blinken
        if (millis() % 200 < 100) 
            digitalWrite(PIN_LED_ALARM, LOW);  // Erste Hälfte: an
        else 
            digitalWrite(PIN_LED_ALARM, HIGH); // Zweite Hälfte: aus
    } 
    // Taste losgelassen
    else { 
        if (druckStart != 0) {
            unsigned long dauer = millis() - druckStart;
            digitalWrite(PIN_LED_ALARM, HIGH); // LED ausschalten
            
            // Aktion nur beim Loslassen (Safe Reset Pattern)
            // Verhindert versehentliches Löschen bei kurzem Kontakt
            if (dauer > TASTER_RESET_DRUCK) {
                 sendeProtokoll("!!! HARDWARE RESET (SAFE) !!!");
                 watchdogTicker.detach(); // Watchdog aus (Reset dauert)
                 
                 // Visuelles Feedback: 2 Sekunden LED an
                 digitalWrite(PIN_LED_ALARM, LOW); 
                 delay(2000); 
                 digitalWrite(PIN_LED_ALARM, HIGH);
                 
                 // Komplettes System zurücksetzen
                 LittleFS.format(); // Flash löschen
                 WiFiManager wm; 
                 wm.resetSettings(); // WiFi-Credentials löschen
                 ESP.restart(); // Neustart -> Captive Portal
            }
            druckStart = 0; // Reset für nächsten Tastendruck
        }
    }
}

// --- HEARTBEAT (Priority optimized) ---
// Sendet Telemetrie-Daten an Backend-Server
void sendeHeartbeatAnApi() {
    // !!! CRITICAL PRIORITY FIX !!!
    // Wenn wir auf UDP ACK warten, KEINESFALLS HTTP blockieren!
    // HTTP-Request kann 500ms+ dauern und würde UDP-Transaktion verzögern
    if (wartetAufBestatigung) return; 

    // Nicht-blockierendes Timing mit dynamischem Intervall
    if (millis() - letzterHeartbeat > aktuellesHeartbeatIntervall) {
        if (WiFi.status() == WL_CONNECTED) {
            WiFiClient client; 
            client.setTimeout(2000); // 2 Sekunden Timeout
            HTTPClient http; 
            http.setTimeout(2000);
            
            String serverPath = "http://" + String(config.apiServer) + "/api.php";
            
            if (http.begin(client, serverPath)) {
                http.addHeader("Content-Type", "application/json");
                
                // Umfangreiches Telemetrie-Paket
                StaticJsonDocument<512> doc; 
                doc["source"] = DEVICE_NAME; // Identifiziert als "sender"
                doc["ip"] = WiFi.localIP().toString(); // Aktuelle IP
                
                // Status-Message zeigt aktuellen Zustand
                doc["status_msg"] = wartetAufBestatigung ? "Sende Cmd..." : "Bereit";
                doc["rssi"] = WiFi.RSSI(); // Signal-Stärke für Diagnose
                doc["heap"] = ESP.getFreeHeap(); // Freier RAM (Memory Leak Check)
                doc["reset_reason"] = ESP.getResetReason(); // Letzte Reset-Ursache

                // Optional: Aktuell ausstehender Befehl
                if (ausstehendeNachricht.length() > 0) 
                    doc["log"] = "Pending: " + ausstehendeNachricht;

                String requestBody; 
                serializeJson(doc, requestBody);
                int httpResponseCode = http.POST(requestBody);
                
                // Server antwortet mit 200 OK
                if (httpResponseCode > 0) {
                    aktuellesHeartbeatIntervall = HEARTBEAT_INTERVALL; // Zurück auf 2s
                    // Response Parsing optional (Server kann Commands senden)
                    // ... gekürzt da nicht kritisch für Grundfunktion ...
                } else { 
                    // Server Error -> Backoff-Strategie
                    aktuellesHeartbeatIntervall = 60000; // Nur alle 60s probieren
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

// --- SETUP ---
// Einmalige Initialisierung beim Systemstart
void setup() {
    delay(1000); // Kurz warten damit Serial stabil ist
    Serial.begin(9600); // Serielle Konsole für Debugging
    Serial.setTimeout(1000); // Timeout für Serial.readString()

    // Hardware-Pins konfigurieren
    pinMode(PIN_LED_ALARM, OUTPUT); 
    digitalWrite(PIN_LED_ALARM, HIGH); // LED_BUILTIN invertiert: HIGH = aus
    pinMode(PIN_LED_WLAN, OUTPUT); 
    digitalWrite(PIN_LED_WLAN, LOW);
    pinMode(PIN_RESET_TASTER, INPUT_PULLUP); // Interner Pull-Up: HIGH wenn offen

    ladeKonfiguration(); // Config aus Flash laden

    WiFiManager wm; // Captive Portal für WiFi-Setup
    wm.setSaveConfigCallback(konfigurationSpeichernCallback); // Callback wenn gespeichert
    
    // Custom Config-Parameter im Portal definieren
    WiFiManagerParameter custom_api("api", "API Server IP", config.apiServer, 32);
    WiFiManagerParameter custom_token("token", "HMAC Secret", config.udpToken, 40);
    // ... (weitere Parameter)
    
    wm.addParameter(&custom_api); 
    wm.addParameter(&custom_token); 
    // ... (alle Parameter hinzufügen)

    wm.setClass("invert"); // Dark Mode CSS
    wm.setConfigPortalTimeout(180); // Portal schließt nach 3min
    wm.setConnectTimeout(30); // 30s pro WiFi-Verbindungsversuch

    Serial.println("Starte WiFiManager...");

    // Wenn Hauptnetz konfiguriert: Direkt verbinden ohne Portal
    if (strlen(config.hauptWlanName) > 0 && strlen(config.hauptWlanPasswort) > 0) {
        WiFi.begin(config.hauptWlanName, config.hauptWlanPasswort);
        
        // Blockierendes Warten max 10 Sekunden
        int i = 0; 
        while(i < 20 && WiFi.status() != WL_CONNECTED) { 
            delay(500); 
            i++; 
            Serial.print("."); 
        }
    }

    // Wenn nicht verbunden: Captive Portal starten
    if (WiFi.status() != WL_CONNECTED) {
        bool erfolg;
        
        // Wenn AP-Passwort gesetzt: Geschütztes Netzwerk
        if (strlen(config.apPasswort) >= 8) 
            erfolg = wm.autoConnect("Alarm-Sender-Konfig", config.apPasswort);
        else 
            erfolg = wm.autoConnect("Alarm-Sender-SETUP-OPEN");
        
        if (!erfolg) Serial.println("Offline Start...");
    }

    // Custom Parameter-Werte übernehmen (gekürzt für Übersicht)
    strlcpy(config.apiServer, custom_api.getValue(), sizeof(config.apiServer));
    strlcpy(config.udpToken, custom_token.getValue(), sizeof(config.udpToken));
    // ...

    // Config speichern wenn geändert oder erfolgreich verbunden
    if (konfigurationSpeichern || WiFi.status() == WL_CONNECTED) 
        speichereKonfiguration();

    // mDNS starten: Gerät per "sender.local" erreichbar
    if (MDNS.begin(DEVICE_NAME)) 
        Serial.println("mDNS gestartet");
    
    TelnetStream.begin(); // Remote Debug Console starten
    udp.begin(lokalerPort); // UDP Socket auf Port 4211 öffnen
    starteOtaDienst(); // OTA Firmware-Update aktivieren
    aktualisiereZielIp(); // Empfänger-IP initial auflösen
    
    // Wenn erfolgreich verbunden: Initiales Heartbeat senden
    if (WiFi.status() == WL_CONNECTED) {
        darfLoggen = true; 
        letzterHeartbeat = 0; // Sofortiges erstes Heartbeat erzwingen
        sendeHeartbeatAnApi(); 
        sendeLogAnApi("System erfolgreich gestartet!");
    }

    // Watchdog aktivieren: ISR wird jede Sekunde aufgerufen
    watchdogTicker.attach(1.0, watchdogInterrupt); 
    Serial.println("Watchdog aktiv. Loop beginnt.");
}

// --- LOOP ---
// Hauptschleife: Wird kontinuierlich durchlaufen
void loop() {
    fuettereWauWau(); // Watchdog zurücksetzen = "Ich lebe noch"
    beissZu();        // Prüfen ob Watchdog ausgelöst wurde
    
    // Wichtige Funktionen in optimierter Reihenfolge
    aktualisiereZielIp();        // Empfänger-IP aktuell halten (mDNS)
    sendeHeartbeatAnApi();       // Telemetrie (wird bei UDP-Transaktion pausiert)
    pruefeTelnetZugang();        // Debug-Console
    verwalteWlanVerbindung();    // Failover (wird bei UDP-Transaktion pausiert)
    aktualisiereWlanLed();       // Status-Anzeige
    verarbeiteSerielleBefehle(); // Commands von Serial Monitor
    verarbeiteUdpAntworten();    // ACKs vom Empfänger (KRITISCH für Retries)
    pruefePhysischenReset();     // Hardware-Reset-Taster
    
    // Services die WiFi brauchen
    if (WiFi.status() == WL_CONNECTED) { 
        ArduinoOTA.handle(); // OTA-Updates verarbeiten
        MDNS.update();       // mDNS Antworten senden
    }
    
    yield(); // KRITISCH: CPU-Zeit an WiFi-Stack und TCP/IP abgeben
             // Ohne yield() würde WiFi crashen
}