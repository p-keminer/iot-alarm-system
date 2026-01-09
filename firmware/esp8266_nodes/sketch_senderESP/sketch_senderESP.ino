#include <ESP8266WiFi.h>								// einbinden der Bibliotheken ESP8266 WLAN
#include <WiFiUdp.h>									//                            UDP-Kommunikation

    	// --- WLAN  & UDP Variablen --- 

const char* ssid = "";									// <- WLAN Daten hier eintragen "x"
const char* password = "";			

		// --- Feste IP-Einstellungen ---

IPAddress localIP(192,168,2,xxx);      	// <- IP Sender hier eintragen 
IPAddress gateway(192,168,2,x);        	// <- Gateway (Router) hier eintragen 
IPAddress subnet(255,255,255,0);       	// <- Subnetzmaske hier ggf. abändern 

		// --- UDP Empfänger ---

const char* empfaengerIP = ""; 	      			 	// <- IP des Empfänger ESP hier eintragen "x"
const unsigned int empfaengerPort = 4210;			// UDP Port fuer empfaenger
WiFiUDP udp;                            			// UDP Objekt erstellen
const unsigned int localPort = 4211;    			// Lokaler Port für UDP

bool wifiWarVerbunden = false;          			// Status WLAN Verbindung

    	// --- Debugging LEDs ---

#define LED_ALARM LED_BUILTIN             			// definieren der LED Pins
#define LED_WIFI D5                       		


unsigned long lastBlink = 0;            			// LED Toggle Variablen
const unsigned long blinkInterval = 500; 

    	// --- Einrichtung Startzustand ---

void setup() {
    Serial.begin(9600);                  			// Seriellen Monitor starten

    
    pinMode(LED_ALARM, OUTPUT);					 	// Alarm LED initialisieren
    digitalWrite(LED_ALARM, LOW);

    
    pinMode(LED_WIFI, OUTPUT);					 	// WLAN LED initialisieren
    digitalWrite(LED_WIFI, LOW);

    
    WiFi.config(localIP, gateway, subnet);  		// WLAN verbinden
    WiFi.begin(ssid, password);              
    Serial.print("Verbinde mit WLAN");						// Debugging Ausgabe

    
    udp.begin(localPort);							// UDP starten
}

    	// --- Ausfuehren der Kommunikation ---

void loop() {

    		// --- WLAN Status prüfen ---
    
		if (WiFi.status() != WL_CONNECTED) {      
       		wifiWarVerbunden = false;

        
        if (millis() - lastBlink >= blinkInterval) { 						// WLAN LED blinkt, wenn keine Verbindung
            digitalWrite(LED_WIFI, !digitalRead(LED_WIFI));			
            lastBlink = millis();                 							
        }

    } else {                                    
        
        digitalWrite(LED_WIFI, HIGH);										// WLAN LED dauerhaft aktivieren, wenn verbunden

        
        if (!wifiWarVerbunden) {                									// Debugging Ausgabe nur einmal beim Verbinden
            Serial.println("\nVerbunden!");
            Serial.print("Zugewiesene IP: ");
            Serial.println(WiFi.localIP());
            wifiWarVerbunden = true;           
        }
    }

    			// --- Seriellen Monitor pruefen (UART KOMMUNIKATION) ---
    
		if (Serial.available()) {                  
        	String cmd = Serial.readStringUntil('\n'); 
        	cmd.trim();                             							// Leerzeichen & Zeilenumbrueche entfernen (sauberer Vergleich)

        				// --- Alarm aktivieren ---
        
				if (cmd == "ALARM_ON") {                
            		digitalWrite(LED_ALARM, HIGH);      						// builtin LED einschalten
            		Serial.println("ESP: Alarm aktiviert");

           					 // --- Alarm Nachricht senden (UDP KOMMUNIKATION) ---
            
					udp.beginPacket(empfaengerIP, empfaengerPort);
            		udp.print("ALARM_ON");
           			udp.endPacket();

        				// --- Alarm deaktivieren ---
        
				} else if (cmd == "ALARM_OFF") {        
            		digitalWrite(LED_ALARM, LOW);       						// builtin LED ausschalten
            		Serial.println("ESP: Alarm deaktiviert");

           					 // --- Alarm Stop Nachricht senden (UDP KOMMUNIKATION) ---
            
					udp.beginPacket(empfaengerIP, empfaengerPort);
            		udp.print("ALARM_OFF");
           			udp.endPacket();

        				// --- Unbekanntes Kommando ---
        
				} else {                                
            		Serial.print("ESP: Unbekanntes Kommando: ");
            		Serial.println(cmd);
        }
    }
}
