# Assembly Guide – IoT-Alarmanlage / IoT Alarm System

## Übersicht / Overview
Dieses Dokument fasst die physische Montage des IoT-Alarmanlagen-Prototyps zusammen.  
This document describes the physical assembly of the IoT alarm system prototype.

Der Prototyp wurde als erstes Embedded-System-Projekt entwickelt und getestet.  
The prototype was developed and tested as a first embedded system project.

---

## Komponenten / Components
Verwendete Hauptkomponenten siehe `components.md`.  
Main components used are listed in `components.md`.

---

## Aufbau / Assembly Notes
- Die Komponenten wurden auf dem Breadboard und der Lochraster-Prototypplatine montiert.  
  Components were placed on the breadboard and perfboard-prototype.
- Elegoo Uno R3 als zentrale Steuerung und die zwei ESP8266-Nodes als Kommunikationstools.  
  Elegoo Uno R3 as central controller and the two ESP8266 nodes as communication tools.
- Sensoren (RFID RC522 und KY-021 Bewegungssensoren) wurden angeschlossen und auf Funktion geprüft.  
  Sensors (RFID RC522 and KY-021 motion sensors) were connected and verified.
- LEDs, Summer und Widerstände wurden entsprechend den Schaltplänen integriert.  
  LEDs, buzzers, and resistors were integrated according to the schematics.
- Der Prototyp wurde mehrfach getestet, um die Kommunikation zwischen Nodes und die Alarmlogik zu prüfen.  
  The prototype was tested multiple times to verify node communication and alarm logic.
- Der Raspberry Pi Zero 2 W wurde als Webserver-Host integriert und über eine USB-Verbindung an den Elegoo Uno R3 gekoppelt (Serielle Kommunikation).  
The Raspberry Pi Zero 2 W was integrated as the web server host and coupled to the Elegoo Uno R3 via a USB connection (serial communication).

---

## Fotos / Photos
Alle Bilder des Aufbaus befinden sich im Ordner `media/photos/`.  
All photos of the assembly can be found in the `media/photos/` folder.


---

## Status
- Prototyp fertiggestellt  
  Prototype completed
- Optimierung für finale Hardware in Arbeit  
  Optimization for final hardware in progress

