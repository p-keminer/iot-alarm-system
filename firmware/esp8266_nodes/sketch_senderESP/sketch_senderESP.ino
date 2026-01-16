#include <ESP8266WiFi.h>							// einbinden der Bibliotheken ESP8266 WLAN
#include <WiFiUdp.h>								//                            UDP-Kommunikation
#include <ESP8266mDNS.h>                            //                            mDNS

    // --- WLAN Daten ---

const char* ssid_main = "";							// <- WLAN Daten hier eintragen
const char* pass_main = "";
const char* ssid_backup = ""; 
const char* pass_backup = "";

    // --- mDNS Namen ---
const char* meinName = "";							// mDNS Name Objekt
const char* zielName = "";							// mDNS Name Kommunikationsgegenueber

    // --- WLAN Management Variablen ---

unsigned long disconnectZeit = 0;					// Zeitstempel disconnect
unsigned long letzterVersuch = 0; 					// Zeitstempel Versuch Verbindungsaufbau Haupt-WLAN
unsigned long letzterScanStart = 0;					// Zeitstempel letzter WLAN-scan
int reconnectVersuche = 0;							// Versuchzaehler
int scanStatus = -1;								// Scan-Statusvariable
bool imBackupModus = false;							// BackUpNetz-Statusvariable
bool mdnsGestartet = false;							// mDNS-Statusvariable

    // --- ACK & Retry Variablen ---

String ausstehendeNachricht = "";                   // Speichert den aktuellen Befehl
bool wartetAufACK = false;                          // Antwortstatus
unsigned long letzteSendeZeit = 0;                  // Zeitstempel für Wiederholungen
const unsigned long retryInterval = 1000;           // Alle 1 Sekunde neu senden
int retryCount = 0;                                 // Anzahl der Wiederholungen
const int maxRetries = 10;                          // Max. Versuche

		// --- UDP Empfänger ---

IPAddress empfaengerIP;                     	    
const unsigned int empfaengerPort = 4210;			// UDP Port fuer empfaenger
WiFiUDP udp;                            			// UDP Objekt erstellen
const unsigned int localPort = 4211;    			// Lokaler Port für UDP

bool wifiWarVerbunden = false;          			// Status WLAN Verbindung

    // --- Debugging LEDs ---

#define LED_ALARM LED_BUILTIN             		    // definieren der LED Pins
#define LED_WIFI D5                       		


unsigned long lastBlink = 0;            			// LED Toggle Variablen
const unsigned long blinkInterval = 500; 

    // --- Funktionen WLAN ---

void updateLED(){
    if (WiFi.status() != WL_CONNECTED) {
        if (millis() - lastBlink >= blinkInterval) {
            digitalWrite(LED_WIFI, !digitalRead(LED_WIFI));
            lastBlink = millis();
        }
    } else {
        digitalWrite(LED_WIFI, HIGH);
    }
}

void checkWiFiConnection() {    // Statusabfrage ob Verbunden
    bool aktuellVerbunden = (WiFi.status() == WL_CONNECTED);

    // 1. Hintergrund-Scan nach dem Hauptnetz
    if (!aktuellVerbunden || imBackupModus) {       
        if (millis() - letzterScanStart >= 30000 && scanStatus == -1) {         // HauptWLAN-Scan alle 30 sek.
            letzterScanStart = millis();                                        // Zeitstempel fuer Scan-Intervall
            scanStatus = 0;                                                     // Scan gestartet
            WiFi.scanNetworks(true);                                            // Asynchronen WLAN-Scan starten
            Serial.println("Suche nach Haupt-WLAN...");                         // LOG Ausgabe
        }
        int n = WiFi.scanComplete();                                            // Pruefen, ob WLAN-Scan abgeschlossen 
        if (n >= 0 && scanStatus != -1) {                                       // Scan abgeschlossen!
            for (int i = 0; i < n; i++) {                                       // iteriere ueber gefundene Netzwerke   
                if (WiFi.SSID(i) == ssid_main) {                                // wenn Name identisch mit HauptWlan
                    Serial.println("Haupt-WLAN gefunden! Springe zurück...");   // LOG Ausgabe
                    imBackupModus = false;                                      // Backup Status wechseln
                    mdnsGestartet = false;                                      
                    reconnectVersuche = 0;                                      // Versuchszaehler zuruecksetzen
                    disconnectZeit = 0;                                         // WLAN verloren Meldung zuruecksetzen
                    WiFi.disconnect();                                          // vom BAckup Netz abmelden
                    WiFi.begin(ssid_main, pass_main);                           // in HauptWLAN wechseln
                    break;                                                      // nach Wechsel -> loop weiter
                }
            }
            WiFi.scanDelete();                                                  // Speicher freigbeben
            scanStatus = -1;                                                    // Status beibehalten
        }
    }

    
    if (aktuellVerbunden) {                                                      // Wenn wieder Verbunden
        if (disconnectZeit != 0) {                                               // und vorher nicht verbunden war
            Serial.println("WLAN wieder da!");                                   // LOG Ausgabe
            disconnectZeit = 0;                                                  // Zeitstempel zuruecksetzen
            reconnectVersuche = 0;                                               // Versuchszaehler zuruecksetzen
        }
        if (!mdnsGestartet) {                                                    // Wenn mDNS nicht gestartet                           
            if (MDNS.begin(meinName)) {                                          // -> starten und fallls erfolgreich                                                       
                Serial.println("mDNS aktiv: " + String(meinName) + ".local");    // LOG Ausgabe 
                mdnsGestartet = true;                                            // Status merken
            }
        }
        MDNS.update();                                                           // mDNS am laufne halten                                                   
        return;                                                                  // Funktion beenden
    }

    
    if (disconnectZeit == 0) {                                                    // Wenn Verbindung weg  
        disconnectZeit = millis();                                                // merke Zeitpunkt
        Serial.println("WLAN verloren! Warte 30s...");                            // LOG Ausgabe
    }


    unsigned long vergangen = millis() - disconnectZeit;                           // Zeitstempel seit WLAN weg
    if (vergangen > 30000 && reconnectVersuche < 3) {                              // wenn laenger als 30 sek. nicht verbunden und weniger als 3 Versuche
        if (millis() - letzterVersuch > 20000) {                                   // und Zeitstempel groesser als 20 sek. 
            reconnectVersuche++;                                                   // Versuchszaehler erhoehen
            letzterVersuch = millis();                                             // Zeitstempel seit letztem Versuch setzen
            Serial.printf("Versuch Nr. %d\n", reconnectVersuche);                  // LOG Ausgabe
            WiFi.disconnect();                                                     // Netze trennen
            WiFi.begin(ssid_main, pass_main);                                      // Versuch neu zu verbinden (Hauptnetz)
        }
    }

    
    if (reconnectVersuche >= 3 && (millis() - letzterVersuch > 15000) && !imBackupModus && scanStatus == -1) {          // Netzwerkwechsel nach 3 Fehlversuchen
        Serial.println("Wechsel zum iPhone!");                                                                          // LOG Ausgabe
        imBackupModus = true;                                                                                           // BackupStatus setzen
        reconnectVersuche = 0;                                                                                          // Versuchszaehler zuruecksetzen       
        mdnsGestartet = false;                                                                                          // mDNS Status setzen
        WiFi.disconnect();                                                                                              // Netze trennen    
        delay(100);                                                                                                     // Zeit für Verbindungsversuch geben    
        WiFi.begin(ssid_backup, pass_backup);                                                                           // Versuch mit BackUpNetz zu Verbinden
        disconnectZeit = millis();                                                                                      // und Zeitpunkt merken
    }
}

  // --- mDNS IP-Suche ---

  void aktualisiereEmpfaengerIP() {
    WiFi.hostByName((String(zielName) + ".local").c_str(), empfaengerIP);                                               // Ordne Kommunikationsziel Adresse zu
}   
  
  // --- Funktionen Kommunikation ---

/* Prueft auf eingehende Nachrichten, auf erlaubte IPs, liest ein, wandelt um und reagiert entsprechend*/

void pruefeACK() {
    int packetSize = udp.parsePacket();                                            // Pruefe auf eingehendes Paket
    if (packetSize) {                                                              // wenn Paket vorhanden
        
        if (udp.remoteIP() != empfaengerIP) {                                      // wenn nicht von empfaenger geschickt 
            return;                                                                // Funktion beenden
        }

        char buffer[255];                                                       
        int len = udp.read(buffer, 254);                                           // Nachricht einlesen
        if (len > 0) buffer[len] = 0;                                              // Null-Terminierung
        String antwort = String(buffer);                                           // In String umwandeln
        antwort.trim();                                                            // Leerzeichen & Zeilenumbrueche entfernen (sauberer Vergleich)

    // --- Falls Antwort erhalten ---

        if (wartetAufACK && antwort == "ACK_" + ausstehendeNachricht) {            // Antwortstatus ja ? & Antwort = bestätigung + Befehl ?
            Serial.println("Erfolg: Bestätigung erhalten!");                       // Debug Ausgabe
            wartetAufACK = false;                                                  // warte auf Anttwort Status setzen
            ausstehendeNachricht = "";                                             // austetehende Nachricht loeschen
            retryCount = 0;                                                        // Zaehler zuruecksetzen
        }
    }

    // --- Falls keine Antwort erhalten ---

    if (wartetAufACK && retryCount < maxRetries &&
        millis() - letzteSendeZeit >= retryInterval) {                             // Wenn Zeit seit letztem Senden hoeher als definiert

        retryCount++;                                                              // erhoehe Zaehler
        Serial.println("Kein ACK, Wiederholung " + String(retryCount) +            // Debugging Ausgabe
        "/" + String(maxRetries) + ": " + ausstehendeNachricht);                   

       aktualisiereEmpfaengerIP();                                                 // vor Versuch mDNS-Adresse prüfen
        if (empfaengerIP.toString() != "0.0.0.0") {                                 
            udp.beginPacket(empfaengerIP, empfaengerPort);
            udp.print(ausstehendeNachricht);
            udp.endPacket();
        }
        letzteSendeZeit = millis();
    }
    
 /* Falls nach maximalen Versuchen keine Antwort erhalten */
     
      if (wartetAufACK && retryCount >= maxRetries) {
        Serial.println("FEHLER: Keine Bestätigung nach max. Versuchen!");           // LOG Ausgabe
        wartetAufACK = false;                                                       // Bestaetigungstatus aendern
        ausstehendeNachricht = "";                                                  // ausstehende Nachricht zuruecksetzen
        retryCount = 0;                                                             // Versuchszaehler zuruecksetzen
    }
}

/* prueft auf eingehende Nachrichten und leitet diese weiter */
void pruefeUART(){
    
    // --- Seriellen Monitor pruefen (UART KOMMUNIKATION) ---
    
    if (Serial.available()) {                  
        String cmd = Serial.readStringUntil('\n'); 
        cmd.trim();                             							// Leerzeichen & Zeilenumbrueche entfernen (sauberer Vergleich)

        		// --- Alarm aktivieren ---
        
			if (cmd == "ALARM_ON") {                
            digitalWrite(LED_ALARM, HIGH);      							// builtin LED einschalten
            Serial.println("ESP: Alarm aktiviert");

            ausstehendeNachricht = "ALARM_ON";                              // Nachricht setzen
            wartetAufACK = true;                                            // Antwortstatus
            retryCount = 0;                                                 // Wiederholungszaehler
            letzteSendeZeit = millis();                                     // Zeitstempel

            // --- Alarm Nachricht senden (UDP KOMMUNIKATION) ---
           
            aktualisiereEmpfaengerIP();
            if (empfaengerIP.toString() != "0.0.0.0") {
			    udp.beginPacket(empfaengerIP, empfaengerPort);
                udp.print("ALARM_ON");
                udp.endPacket();
            }
        		// --- Alarm deaktivieren ---
        
			} else if (cmd == "ALARM_OFF") {        
            digitalWrite(LED_ALARM, LOW);       							// builtin LED ausschalten
            Serial.println("ESP: Alarm deaktiviert");

            ausstehendeNachricht = "ALARM_OFF";
            wartetAufACK = true;
            retryCount = 0;
            letzteSendeZeit = millis();

            // --- Alarm Stop Nachricht senden (UDP KOMMUNIKATION) ---
           
            aktualisiereEmpfaengerIP();
            if (empfaengerIP.toString() != "0.0.0.0") {
			udp.beginPacket(empfaengerIP, empfaengerPort);
            udp.print("ALARM_OFF");
            udp.endPacket();
            }
        }
    }
}

    // --- Einrichtung Startzustand ---

void setup() {
    Serial.begin(9600);                  			        // Seriellen Monitor starten
    Serial.setTimeout(50);                                  // Echtzeit Timeout fuer .readStringUntil('\n')
    
    pinMode(LED_ALARM, OUTPUT);					 			// Alarm LED initialisieren
    digitalWrite(LED_ALARM, LOW);

    
    pinMode(LED_WIFI, OUTPUT);					 			// WLAN LED initialisieren
    digitalWrite(LED_WIFI, LOW);

    
    	            
    WiFi.begin(ssid_main, pass_main);                       // WLAN mit Hauptnetz verbinden
    Serial.print("Verbinde mit WLAN");						// Debugging Ausgabe

    // UDP starten
    udp.begin(localPort);
}

    // --- Ausfuehren der Kommunikation ---

void loop() {

    checkWiFiConnection();
    updateLED();
    pruefeUART();
    pruefeACK();

}
