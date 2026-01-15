#include <ESP8266WiFi.h>                                              // einbinden der Bibliotheken ESP8266 WLAN
#include <WiFiUdp.h>                                                  //                            UDP-Kommunikation
#include <ESP8266mDNS.h>                                              //                            mDNS  

#define LEDR_PIN D1                                                   // definieren der LED Pins
#define LEDY_PIN D2                                                     
#define LED_WIFI D3                                                     

#define BUZZER1_PIN D5                                                // definieren der Buzzer Pins
#define BUZZER2_PIN D6                                                  

#define TASTER_PIN D7                                                 // definieren des Taster Pin

    // --- WLAN & UDP Variablen ---

const char* ssid_main = "";                                 // <- WLAN SSID hier eintragen 
const char* pass_main = "";                       			// <- WLAN Passwort hier eintragen 
const char* ssid_backup = "";                               // <- Ausweich-WLAN Daten hier eintragen
const char* pass_backup = "";

unsigned long letzterVersuch = 0;
int scanStatus = -1 ;                                                 // Status des Scans
unsigned long letzterScanStart = 0;                                   // Zeitpunkt, wann der Scan gestartet wurde
bool imBackupModus = false;                                           // Status ob im Backup Netzwerk
int reconnectVersuche = 0;                                            // reconnect Versuchszaehler
unsigned long disconnectZeit = 0;                                     // Zeitstempel WLAN weg 


WiFiUDP udp;                                                          // UDP Objekt erstellen
const unsigned int localPort = 4210;                                  // Lokaler Port für UDP

    // --- mDNS & Sicherheit ---

const char* meinName = "";                          				   // Erreichbar unter alarm-empfaenger.local
const char* senderName = "";                            			   // Name des Senders zur Filterung
bool mdnsGestartet = false;                                        	   // Status ob mDNS Netzwerk laeuft


    // --- Alarmvariablen ---

bool alarmEmpfangen = false;                                          // Statusvariablen für Empfang & Aktivierung                     
bool alarmAn = false;                                                                  

    // --- LED Variablen ---

bool ledStatus = false;                                              // LED Toggle Variablen
unsigned long toggleZuletzt = 0;                                        
unsigned long toggleInterval = 200;                             
                           

    // --- Taster Variablen ---

bool tasterZuletzt = HIGH;                                           // Taster Toggle/Status Variablen
bool tasterJetzt = HIGH;      
unsigned long tasterLetzteZeit = 0;                                  // Entprell Variablen
const unsigned long debounce = 50;                                                  


            // -------- Funktionen gesamt  ---------

    // --- Funktionen WLAN ---

void pruefeWLAN() {
   bool aktuellVerbunden = (WiFi.status() == WL_CONNECTED);                             // Statusabfrage ob Verbunden

 /* Wechselt falls Wlan getrennt wurde und nicht
wiederhergestellt werden kann ins Backup Netz und wieder zurueck sobald Wlan da ist */  
   
    if (!aktuellVerbunden || imBackupModus) {
            
        if (millis() - letzterScanStart >= 30000 && scanStatus == -1) {                         // HauptWLAN-Scan alle 30 sek.
            letzterScanStart = millis();                                                        // Zeitstempel fuer Scan-Intervall
            scanStatus = 0;                                                                     // Scan gestartet
            WiFi.scanNetworks(true);                                                            // Asynchronen WLAN-Scan starten
            Serial.println("Prüfe im Hintergrund auf Haupt-WLAN...");                           // LOG Ausgabe
        }

        int n = WiFi.scanComplete();                                                            // Pruefen, ob WLAN-Scan abgeschlossen 
            if (n >= 0 && scanStatus != -1) {                                                   // Scan abgeschlossen!
                for (int i = 0; i < n; i++) {                                                   // iteriere ueber gefundene Netzwerke
                    if (WiFi.SSID(i) == ssid_main) {                                            // wenn Name identisch mit HauptWLAN
                        Serial.println("Haupt-WLAN wieder in Reichweite! Springe zurück...");
                        imBackupModus = false;                                                  // Backup Status wechseln
                        mdnsGestartet = false;                                                  // mDNS nach Wechsel neu starten
                        reconnectVersuche = 0;                                                  // Backup Wechsel ausschließen
                        disconnectZeit = 0;                                                     // WLAN verloren Meldung zuruecksetzen
                        WiFi.disconnect();                                                      // vom Backup Netz abmelden
                        WiFi.begin(ssid_main, pass_main);                                       // in HauptWLAN wechseln
                        break;                                                                  // nach wechsel -> Loop weiter
                    }
                }
                WiFi.scanDelete();                                                              // Speicher freigeben
                scanStatus = -1;                                                                // Status beibehalten
            }
    }
   
    if (aktuellVerbunden) {                                                                     // Wenn wieder verbunden                
        if (disconnectZeit != 0) {                                                              // und vorher nicht verbunden war
            Serial.println("WLAN wieder da!");                                                  // LOG Ausgabe
            disconnectZeit = 0;                                                                 // Zeitstempel zuruecksetzen
            reconnectVersuche = 0;                                                              // Versuchzaehler zuruecksetzen
        }
        
        if (!mdnsGestartet) {                                                                   // Wenn mDNS nicht gestartet
            if (MDNS.begin(meinName)) {                                                         // -> starten und falls erfolgreich
                Serial.println("mDNS gestartet: " + String(meinName) + ".local");               // LOG Ausgabe
                mdnsGestartet = true;                                                           // Status merken
            }
        }
        MDNS.update();                                                                          // mDNS am laufen halten
        return;
    }

   
    if (disconnectZeit == 0) {                                                                  // wenn Zeitstempel nicht gesetzt
        disconnectZeit = millis();                                                              // setze Zeitstempel 
        Serial.println("WLAN verloren! Warte 30 sekunden...");                                  // LOG Ausgabe        
    }

    unsigned long vergangen = millis() - disconnectZeit;                                        // Zeitstempel seit Wlan weg

   
    if (vergangen > 30000) {                                                                    // nach 30 Sekunden versuchen wieder zu Verbinden
        
        if (millis() - letzterVersuch > 20000 && reconnectVersuche < 3) {                       // alle 20 Sekunden neuer Versuch (maximal 3)
            reconnectVersuche++;                                                                // Versuchzaehler erhoehen
            letzterVersuch = millis();                                                          // Zeitstempel seit letztem Versuch setzen
            Serial.print("Versuch Nr. "); Serial.println(reconnectVersuche);                    // LOG Ausgabe
            WiFi.disconnect();                                                                  // sicherheitshalber Verbindungen trennen (falls vorhanden)
            WiFi.begin(ssid_main, pass_main);                                                   // Versuch neu zu verbinden
        }
    }

    if (reconnectVersuche >= 3 && (millis() - letzterVersuch > 15000) && !imBackupModus && scanStatus== -1) {       // Netzwerkwechsel nach 3 Fehlversuchen
        Serial.println("Wechsel zum BACKUP-WLAN!");                                                                 // LOG Ausgabe
        imBackupModus = true;                                                                                       // BackupStatus setzen
        reconnectVersuche = 0;                                                                                      // Versuchszaehler zuruecksetzen
        WiFi.disconnect();                                                                                          // sicherheitshalber Verbindungen trennen (falls vorhanden)
        delay(100);                                                                                                 // Zeit fuer Versuch geben
        WiFi.begin(ssid_backup, pass_backup);                                                                       // Backup Netz starten
        disconnectZeit=millis();                                                                                    // Zeitstempel fuer WLAN-Verlust
    }
}

    // --- Funktionen UDP  ---

/* prueft auf eingehende Nachrichten und erlaubte IPs, liest diese aus aus,
wandelt um & aktiviert Alarm und bestätigt den erhalt. */

void pruefeUDP() {
    int packetSize = udp.parsePacket();                                                           // Prüfe auf eingehendes Paket
    if (packetSize) {                                                                             // Wenn Paket vorhanden
        
            // --- mDNS IP Check ---
        
        IPAddress senderIP;                                                                        // IP Pruefung
        String dnsName = String(senderName) + ".local"; 
        WiFi.hostByName(dnsName.c_str(), senderIP);                                                

        if (udp.remoteIP() != senderIP && senderIP.toString() != "0.0.0.0") {                       // Falls IP nicht vom Sender und nicht leer
            Serial.print("Sicherheitswarnung: Paket von unbekannter IP ignoriert: ");               // Debug Ausgabe 
            Serial.println(udp.remoteIP());                                                         
            udp.flush();                                                                            // Paket verwerfen
            return;                                                                                 // Funktion verlassen
        }

        char buffer[255];                                                           // Buffer fuer Nachricht erstellen           
        int len = udp.read(buffer, 254);                                            // Nachricht einlesen
        if (len > 0) buffer[len] = 0;                                               // Null-Terminierung
        String msg = String(buffer);                                                // In String umwandeln
        msg.trim();                                                                 // Leerzeichen & Zeilenumbrueche entfernen (sauberer Vergleich)

        if (msg == "ALARM_ON") {                                                        
            alarmEmpfangen = true;                                                  
            Serial.println("ALARM via mDNS verifiziert!");                          // Debugging Ausgabe
            
            udp.beginPacket(udp.remoteIP(), 4211);                                  // Bestaetigung zuruecksenden
            udp.print("ACK_ALARM_ON");                                              
            udp.endPacket();

        } else if (msg == "ALARM_OFF") {                                        
            alarmEmpfangen = false;
            alarmAn = false;
            Serial.println("ALARM vom Sender deaktiviert!");                        // Debugging Ausgabe
            
            udp.beginPacket(udp.remoteIP(), 4211);                                  // Bestaetigung zuruecksenden
            udp.print("ACK_ALARM_OFF");
            udp.endPacket();

        } else {                                                                                        
            Serial.println("Unbekannte Nachricht: " + msg);                         // Debugging Ausgabe
        }
    }
}


    // --- Alarm Funktionen ---

/* Pruefe ob Alarm Signal empfangen */

void pruefeAlarm() {
    if(alarmEmpfangen){                                                                 
        alarmAn = true;                                                                         
    }
}

    // --- Taster Funktionen ---

/* Alarm manuell umschalten */

void tasterSetztAlarm() {
    tasterJetzt = digitalRead(TASTER_PIN);                          

    
  if (tasterZuletzt == HIGH && tasterJetzt == LOW && millis() - tasterLetzteZeit > debounce) {          // Taster gedrueckt und entprellung > 50msekunden?
        alarmAn = !alarmAn;                                                                             // AlarmStatus togglen
        alarmEmpfangen = false;                                                                         // Sender-Signal ignorieren
        Serial.println(alarmAn ? "Alarm manuell aktiviert" : "Alarm manuell deaktiviert");              // Debugging Ausgabe
        tasterLetzteZeit = millis();                                                                    
    }
    tasterZuletzt = tasterJetzt;                                                                        // Status speichern
}

    // --- Funktion Alarmanzeige (visuell & optisch) ---

/* schaltet LEDs und Buzzer togglend an */

void updateAlarm() {
    if(alarmAn){                                                                                        // wenn Alarm aktiviert 
        
        if (millis() - toggleZuletzt >= toggleInterval) {                               
            ledStatus = !ledStatus;                                                                     // wenn LED umschaltzeit >= intervall,
            digitalWrite(LEDR_PIN, ledStatus ? HIGH : LOW);                                             // LED & Buzzer togglen (status = !status)
            digitalWrite(BUZZER2_PIN, ledStatus ? HIGH : LOW);
            digitalWrite(LEDY_PIN, !ledStatus ? HIGH : LOW);
            digitalWrite(BUZZER1_PIN, ledStatus ? HIGH : LOW);
            toggleZuletzt = millis();                                                                   // speichere umschaltzeit
        }
    } else {                                                                                                
        digitalWrite(BUZZER1_PIN, LOW);                                                                 // wenn Alarm deaktiviert, 
        digitalWrite(BUZZER2_PIN, LOW);                                                                 // deaktiviere alles
        digitalWrite(LEDR_PIN, LOW);
        digitalWrite(LEDY_PIN, LOW);
    }
}


        // ---Funktion WLAN LED ---
    
/* WLAN Status Anzeige */

void updateLedWLAN() {
    if (WiFi.status() == WL_CONNECTED) {
        digitalWrite(LED_WIFI, HIGH);                                                   // Dauerleuchten = Verbunden
    } else {
        if (millis() % 1000 < 500) {                                                    // Blinken während der Suche/Wartezeit
            digitalWrite(LED_WIFI, HIGH);
        } else {
            digitalWrite(LED_WIFI, LOW);                                                // sonst Ausschalten
        }
    }
}


        // --- Einrichtung Startzustand ---

void setup() {
    
    Serial.begin(9600);                                                                 // Seriellen Monitor starten

    pinMode(LED_WIFI, OUTPUT);                                                          // initialisieren LED, Buzzer und Taster Pin/s 
    pinMode(LEDR_PIN, OUTPUT);
    pinMode(LEDY_PIN, OUTPUT);
    pinMode(BUZZER1_PIN, OUTPUT);
    pinMode(BUZZER2_PIN, OUTPUT);
    pinMode(TASTER_PIN, INPUT_PULLUP);                                  

    digitalWrite(LED_WIFI, LOW);
    digitalWrite(LEDR_PIN, LOW);
    digitalWrite(LEDY_PIN, LOW);
    digitalWrite(BUZZER1_PIN, LOW);
    digitalWrite(BUZZER2_PIN, LOW);

    WiFi.begin(ssid_main, pass_main);                                                   // WLAN verbinden (Hauptnetz)
    Serial.print("Verbinde mit WLAN");                                                  // Debugging Ausgabe

    
    udp.begin(localPort);                                                               // UDP starten
    Serial.println("UDP-Empfang gestartet auf Port " + String(localPort));              // LOG Ausgabe

    
}


// --- Ausfuehren der Funktionen ---

void loop() {

    pruefeWLAN();
    updateLedWLAN();                                                                                               
    pruefeUDP();                                                    
    tasterSetztAlarm();                                         
    pruefeAlarm();                                                
    updateAlarm();                                                
}
