# Komponentenliste – IoT-Alarmanlage / Component List – IoT Alarm System

Dies ist die Übersicht der Hauptkomponenten, die für die selbstentwickelte IoT-Alarmanlage verwendet wurden.  
This is an overview of the main components used for the self-developed IoT alarm system.

---

## Mikrocontroller / Microcontrollers
- **Elegoo Uno R3** – zentrale Steuerungseinheit / central control unit  
- **ESP8266 Nodes v2** (2x) – Sensor-Datenverarbeitung und Kommunikation / sensor data processing and communication
- **Raspberry Pi Zero 2 W** – Webserver & Backend-Host / web server & backend host

## Sensoren / Sensors
- **RFID RC522** – RFID-Leser für Zutrittskontrolle / RFID reader for access control  
- **KY-021 Bewegungssensoren** (2x) – Erfassung von Bewegungen / motion detection sensors

## Widerstände / Resistors
- **100 Ω** – 4 Stück / 4 pieces 
- **1 kΩ** – 1 Stück / 1 piece  
- **2 kΩ** – 1 Stück / 1 piece  
- **330 Ω** – 3 Stück / 3 pieces

## LEDs
- **7x LEDs** – zur visuellen Anzeige von Status oder Alarm / for visual status or alarm indication

## Summer / Buzzers
- **3x aktive Summer** – akustische Alarmanzeige / audible alarm indicators

## Sonstige Bauteile / Other Components
- **2-Pin Taster / 2-Pin push button** – für manuelle Steuerung / Reset / for manual control / reset  
- **Steckbrücken / Jumper-Kabel / Breadboard** – zum Aufbau der Prototypenverbindungen / for prototyping connections

## PCBs
**Stiftleisten / Male Header**
- **1 x 8** 1 Stück / 1 piece                              
- **1 x 10** 1 Stück / 1 piece
- **1 x 7** 1 Stück / 1 piece
- **1 x 6** 1 Stück / 1 piece

optional:

**Buchsenleisten / Female Header**
- **1x15** 4 Stück / 4 pieces
---

## Hinweise / Notes
- Die Bauteile wurden auf Lochrasterplatinen und Breadboard getestet.  
  All components were tested on perfboard and breadboard.  
- Die Widerstände dienen zur Strombegrenzung für LEDs, Sensoren und einen Spannungsteiler.  
  Resistors are used to limit current for LEDs and sensors and a Voltage Divider.  
- Alle Komponenten wurden einzeln getestet, bevor sie in das Gesamtsystem integriert wurden.  

  All components were individually tested before integration into the complete system.


