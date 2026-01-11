// --- RFID-Sensor (MFRC522) ---

#include <SPI.h>			                // einbinden der Bibliotheken RFID-Sensor 
#include <MFRC522.h>			            // Buskommunikation (SPI) und Objekt 


#define RST_PIN 9			             	// definieren der Pins des RFID-Sensor
#define SS_PIN 10

MFRC522 mfrc522(SS_PIN, RST_PIN);	    	// Initialisieren des RFID-Sensor 


bool karteDa = 0;			                // Statusvariablen für RFID-Sensor	
bool zutritt = 0;
bool alarmScharf = 0;
byte erlaubteUIDs[2][4] = {{0x,0x,0x,0x},{0x,0x,0x,0x}};  	// <- UIDs eintragen (4 Bytes pro UID) * 



#define BUZZER_PIN 5			            // definieren des Buzzer Pin


#define REED_PIN1 2			             	// definieren der Magnetsensor Pins
#define REED_PIN2 3

#define LED_ALARM 6			           		// definieren der LED Pins
#define LED_SENS1 7
#define LED_SENS2 8

   // --- Variablen Magnetsensoren & Alarm ---

bool alarmAusgeloest = false;		      	// Variable Alarm senden

uint8_t reed1=HIGH;			              	// Variablen Magnetsensor
uint8_t reed2=HIGH;


    // --- Funktionen gesamt ---

  // Funktionen Magnetsensoren

/* Zustaende der Magnetsensoren einlesen
(HIGH = geöffnet , LOW = geschlossen) */

void pruefeSensor1(){
 reed1 = digitalRead(REED_PIN1);	
}					

void pruefeSensor2(){
 reed2 = digitalRead(REED_PIN2);
}

  // --- Funktionen RFID Sensor ---

/* Vergleicht gelesene UID mit erlaubten UIDs 
& gibt true zurueck, wenn UID übereinstimmt */

bool UIDvergleich(){
bool uidErlaubt = false;
for(int k = 0; k < 2; k++){      		 		// iteriere über jede erlaubte UID [x][]
  bool gleich = true;
 for(int i = 0; i < 4; i++){   				  	// iteriere über jedes Byte [][x]
      if(mfrc522.uid.uidByte[i] != erlaubteUIDs[k][i]){
          gleich = false;					    // aktuelles Byte stimmt nicht ueberein
     }
  }
  if(gleich){
  uidErlaubt = true;						    // richtige UID
}
}
return uidErlaubt;						        // Rueckgabe Pruefung UIDs
}


/* Kontrolliert ob Karten einzulesen sind
und aktiviert (funktionell) Alarm */

void pruefeRFID(){
 if (!mfrc522.PICC_IsNewCardPresent()) {				// kontrolle ob Karte da ist
   karteDa = false;
   return;
}
if (!mfrc522.PICC_ReadCardSerial()) {				 	// falls da, aber nicht lesbar
   return;
 }
zutritt = UIDvergleich();					
 if (zutritt && !karteDa) {					          	// Alarm de- & aktivieren
   karteDa = true;
   alarmScharf = !alarmScharf;					
   Serial.print("Alarm jetzt: ");				      	// Debugging Ausgaben
   Serial.println(alarmScharf ? "scharf" : "unscharf");
 }

 mfrc522.PICC_HaltA();						            // Kommunikation mit Karte beenden

}

  // --- Funktionen LEDs ---

/* prüft funktionellen Alarm
und zeigt Status der Sensoren an */

void updateLEDs(){

digitalWrite(LED_ALARM, alarmScharf ? HIGH : LOW);		    // Visuelle Anzeige Alarm scharf gestellt ?
 if (alarmScharf) {
   digitalWrite(LED_SENS1, reed1 == LOW ? HIGH : LOW);		// Visuelle Anzeige Sensoren in Magnetfeld ?
   digitalWrite(LED_SENS2, reed2 == LOW ? HIGH : LOW);
 } else {
   digitalWrite(LED_SENS1, LOW);				            // Alarm funktionell deaktiviert = LEDS deaktivieren
   digitalWrite(LED_SENS2, LOW);
 }
}

  // --- Funktionen Kommunikation ---

/*  wenn Alarm aktiviert & Sensoren nicht in Magnetfeld 
kommuniziert an senderESP über UART und aktiviert akustischen Alarm */

void sendeAlarm(){						
 if(alarmScharf && !alarmAusgeloest && reed1 == HIGH && reed2 == HIGH){	
   alarmAusgeloest = true;							
   Serial.println("ALARM_ON");							
   digitalWrite(BUZZER_PIN,HIGH);						 
 }
 if(!alarmScharf && alarmAusgeloest){						// manuelles deaktivieren 
   alarmAusgeloest = false;							        // des Alarms per Karte
   Serial.println("ALARM_OFF");
   digitalWrite(BUZZER_PIN,LOW);
 }
}

/* Debugging über Seriellen Monitor */	
				
void pruefeSerielleEingabe() {							
 if (Serial.available() > 0) {
   char input = Serial.read();
				
   if (input == '1') {							  	// 1 zum funktionellen aktivieren
     alarmScharf = true;						
     Serial.println("Serieller Befehl: Alarm SCHARF");
   }
   else if (input == '0') {							// 0 zum funktionellen deaktivieren
     alarmScharf = false;
     Serial.println("Serieller Befehl: Alarm UNSCHARF");
   }
 }
}

  // --- Einrichtung Startzustand ---

void setup() {

pinMode(REED_PIN1, INPUT_PULLUP);			// initialisiere Magnetsensor pins
pinMode(REED_PIN2, INPUT_PULLUP);

pinMode(LED_BUILTIN, OUTPUT);			 	// initialisiere LED pins
pinMode(LED_SENS1, OUTPUT);
pinMode(LED_SENS2, OUTPUT);
pinMode(LED_ALARM, OUTPUT);

pinMode(BUZZER_PIN, OUTPUT);			  	// initialisiere Buzzer pin

digitalWrite(LED_BUILTIN, LOW);

Serial.begin(9600);



SPI.begin();         				        // initialisiere SPI Bus des RFID-Sensor
mfrc522.PCD_Init();  				        // initialisiere Kartenlesen des RFID-Sensor

}
	// --- Ausfuehren der Funktionen ---

void loop() {
 
 pruefeSerielleEingabe();
 pruefeRFID();
 pruefeSensor1();
 pruefeSensor2();

 sendeAlarm();
 updateLEDs();

}
