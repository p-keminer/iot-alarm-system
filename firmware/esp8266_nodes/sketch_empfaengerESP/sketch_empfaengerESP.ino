    // --- Blynk Konfiguration ---

#define BLYNK_PRINT Serial					  	// Debug-Ausgabe über Seriellen Monitor
#define BLYNK_TEMPLATE_ID ""					// <- Blynk Template ID hier eintragen "x"
#define BLYNK_TEMPLATE_NAME ""					// <- Blynk Template Name hier eintragen "x"
#define BLYNK_AUTH_TOKEN  ""					// <- Blynk Auth Token hier eintragen "x"


#include <ESP8266WiFi.h>						// einbinden der Bibliotheken ESP8266 WLAN
#include <WiFiUdp.h>							//                            UDP-Kommunikation
#include <BlynkSimpleEsp8266.h>					//                            Blynk Library für ESP8266



#define LEDR_PIN D1								// definieren der LED Pins
#define LEDY_PIN D2							
#define LED_WIFI D3							


#define BUZZER1_PIN D5							// definieren der Buzzer Pins
#define BUZZER2_PIN D6							


#define TASTER_PIN D7							// definieren des Taster Pin

    // --- WLAN & UDP Variablen ---

const char* ssid = "";							  			// <- WLAN SSID hier eintragen "xyz"
const char* password = "";									// <- WLAN Passwort hier eintragen "xyz"

    // --- Blynk Authentifizierungsvariable ---

char auth[] = BLYNK_AUTH_TOKEN;					

WiFiUDP udp;										// UDP Objekt erstellen
const unsigned int localPort = 4210;				// Lokaler Port für UDP

    // --- Alarmvariablen ---

bool alarmEmpfangen = false;			          	// Statusvariablen für Empfang & Aktivierung		  
bool alarmAn = false;								      


    // --- LED Variablen ---

bool ledStatus = false;							    // LED Toggle Variablen
unsigned long toggleZuletzt = 0;					
unsigned long toggleInterval = 200;				
unsigned long blinkZuletzt = 0;					  
unsigned long blinkInterval = 500;				

    // --- Taster Variablen ---

bool tasterZuletzt = HIGH;							// Taster Toggle/Status Variablen
bool tasterJetzt = HIGH;							


            // -------- Funktionen gesamt  ---------

    // --- Funktionen UDP  ---

/* prueft auf eingehende Nachrichten, liest aus,
wandelt um & aktiviert Alarm */

void pruefeUDP() {
	int packetSize = udp.parsePacket();						// Prüfe auf eingehendes Paket
	if (packetSize) {
		char buffer[255];									
		int len = udp.read(buffer, 254);					// Nachricht einlesen
		if (len > 0) buffer[len] = 0;						// Null-Terminierung
		String msg = String(buffer);						// In String umwandeln
		msg.trim();										    // Leerzeichen & Zeilenumbrueche entfernen (sauberer Vergleich)

		if (msg == "ALARM_ON") {							
			alarmEmpfangen = true;
			Serial.println("ALARM vom Sender empfangen!");      			// Debugging Ausgabe
		} else if (msg == "ALARM_OFF") {					
			alarmEmpfangen = false;
			alarmAn = false;
			Serial.println("ALARM vom Sender deaktiviert!");    			// Debugging Ausgabe
		} else {											
			Serial.println("Unbekannte Nachricht: " + msg);     			// Debugging Ausgabe
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

	
  if (tasterZuletzt == HIGH && tasterJetzt == LOW) {    	// Taster gedrueckt ?
		alarmAn = !alarmAn;								
		alarmEmpfangen = false;							   	// Sender-Signal ignorieren
		Serial.println(alarmAn ? "Alarm manuell aktiviert" : "Alarm manuell deaktiviert");   	// Debugging Ausgabe
	}
	tasterZuletzt = tasterJetzt;						 	// Status speichern
}


    // --- Blynk Funktion ---

/* ermoeglicht manuelles de- & aktivieren 
des Alarms per App oder Dashboard */

BLYNK_WRITE(V0) {										
	int value = param.asInt();							       	// Wert auslesen & speichern

	if (value == 1) {										    // Alarm aktivieren
		alarmAn = true;
		alarmEmpfangen = false;
		Serial.println("Alarm über Blynk EIN");       					// Debugging Ausgabe
	} else {												                
		alarmAn = false;										// Alarm deaktivieren
		alarmEmpfangen = false;
		Serial.println("Alarm über Blynk AUS");       					// Debugging Ausgabe
	}
}


    // --- Funktion Alarmanzeige (visuell & optisch) ---

/* schaltet LEDs und Buzzer togglend an */

void updateAlarm() {
	if(alarmAn){											    		// wenn Alarm aktiviert 
		
		if (millis() - toggleZuletzt >= toggleInterval) {				 
			ledStatus = !ledStatus;										// wenn LED umschaltzeit >= intervall,
			digitalWrite(LEDR_PIN, ledStatus ? HIGH : LOW);				// LED & Buzzer togglen (status = !status)
			digitalWrite(BUZZER2_PIN, ledStatus ? HIGH : LOW);
			digitalWrite(LEDY_PIN, !ledStatus ? HIGH : LOW);
			digitalWrite(BUZZER1_PIN, ledStatus ? HIGH : LOW);
			toggleZuletzt = millis();									// speichere umschaltzeit
		}
	} else {												
		digitalWrite(BUZZER1_PIN, LOW);     							// wenn Alarm deaktiviert, 
		digitalWrite(BUZZER2_PIN, LOW);									// deaktiviere alles
		digitalWrite(LEDR_PIN, LOW);
		digitalWrite(LEDY_PIN, LOW);
	}
}


		// ---Funktion WLAN LED ---
	
/* WLAN Status Anzeige */

void updateLedWLAN() {
	if (WiFi.status() == WL_CONNECTED) {										
		digitalWrite(LED_WIFI, HIGH);													
	} else {
		digitalWrite(LED_WIFI, LOW);													 
	}
}


		// --- Einrichtung Startzustand ---

void setup() {
	
	Serial.begin(9600);									// Seriellen Monitor starten

	pinMode(LED_WIFI, OUTPUT);							// initialisieren LED, Buzzer und Taster Pin/s 
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

	WiFi.begin(ssid, password);												// WLAN verbinden
	Serial.print("Verbinde mit WLAN");											// Debugging Ausgabe

	
	udp.begin(localPort);													// UDP starten
	Serial.println("UDP-Empfang gestartet auf Port " + String(localPort));

	
	Blynk.begin(auth, ssid, password);										// Blynk initialisieren
}


// --- Ausfuehren der Funktionen ---

void loop() {
	
	if (WiFi.status() != WL_CONNECTED) {									// WLAN pruefen
		WiFi.reconnect();													// erneut verbinden
	}

	updateLedWLAN();											
	Blynk.run();													
	pruefeUDP();													
	tasterSetztAlarm();										
	pruefeAlarm();												
	updateAlarm();												

	
	Blynk.virtualWrite(V1, alarmAn ? 255 : 0); 								// Blynk input aktualisieren
}
