# IoT-Alarmanlage / IoT Alarm System

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## 📑 Inhaltsverzeichnis
- [Übersicht](#übersicht--overview)
- [Systemarchitektur](#systemarchitektur--system-architecture)
- [Firmware](#firmware)
- [Hardware](#hardware)
- [Mechanik](#mechanik--mechanical-design)
- [Assembly](#assembly--zusammenbau)
- [Lessons Learned](#lessons-learned)
- [Status](#status)
- [Hinweis](#hinweis--notes)

## Übersicht / Overview
Dieses Projekt ist eine selbstentwickelte IoT-Alarmanlage mit zwei ESP8266-Nodes und einem Arduino R3.  
Sie umfasst Sensorerfassung, Kommunikation (UART&UDP) zwischen den Nodes, eine zentrale Steuerungseinheit sowie selbst entworfene PCB.  
Für die Sensoren wurden 3D-gedruckte Gehäuse verwendet.  
Dies war mein erstes praktisches Embedded-System-Projekt, umgesetzt mit minimaler Vorerfahrung.  

This project is a self-developed IoT alarm system using two ESP8266 nodes and an Arduino R3.  
It includes sensor data acquisition, communication (UART&UDP) between nodes, a central control unit, and custom-designed PCB.  
3D-printed housings were used for the sensors.  
This was my first hands-on embedded system project, developed with minimal prior experience.

---

## Systemarchitektur / System Architecture
- Zwei ESP8266-Nodes zur Sensorerfassung und Kommunikation / Two ESP8266 nodes for sensor data acquisition and communication
- Arduino R3 als zentrale Steuerung / Arduino R3 as central controller
- Selbst entworfene PCB / Custom designed PCB [PCB Sending Nodes](hardware/pcb/r3_senderEsp/rückseite_r3_senderEsp.png) [PCB Receiving Nodes](hardware/pcb/empfaengerEsp/rückseiteEmpfaenger.png)  
- 3D-gedruckte Sensorgehäuse / 3D-printed housings for sensors 

**Ablauf / Workflow:** Sensoren → Arduino → Alarmlogik → ESP-Nodes  → Kommunikation → Ausgabe   
**Workflow:** Sensors → Arduino → Alarm logic → ESP-Nodes → Communication → Output 

---

## Firmware
- Embedded C/C++ für ESP8266 und Arduino / Embedded C/C++ for ESP8266 and Arduino
- Node-Kommunikation und Alarmlogik / Node communication and alarm logic
- Strukturierter und kommentierter Code erleichtert Debugging / Structured and commented code simplifies debugging
- Einzeltests der Nodes / Individual node testing

---

## Hardware
- Prototyp-PCB selbst gelötet / Custom PCB soldered as prototype
- Sensoren verbunden und getestet / Sensors connected and tested
- Schaltpläne und Lochrasterlayouts in `schematics/` und `pcb/` enthalten / Schematics and perfboard layouts included in `schematics/` and `pcb/`

---

## Mechanik / Mechanical Design
- 3D-gedruckte Gehäuse für Sensoren / 3D-printed housings for sensors
- STL-Dateien für den Druck / STL files for printing
- G-Code-Datei für **Bambu Lab H2S** enthalten / G-code file for **Bambu Lab H2S** included
- Alle Dateien befinden sich im Ordner `mechanics/` / All files are located in the `mechanics/` folder

---

## Assembly / Zusammenbau
- Komponenten montiert und funktionsfähiger Prototyp erstellt / Components assembled to form a functional prototype
- Programmierung der Nodes und Test der Sensorwerte / Nodes programmed and sensor readings tested
- Siehe Ordner `assembly/` für Fotos und Hinweise / See `assembly/` folder for photos and notes

---

## Lessons Learned
- Dokumentiert in `docs/lessonslearned.md` / Documented in `docs/lessonslearned.md`
- Herausforderungen: Embedded Programming, Löten, Sensorintegration, Debugging / Challenges: embedded programming, soldering, sensor integration, debugging
- Iteratives Vorgehen und Dokumentation entscheidend / Iterative approach and documentation are essential

---

## Status
- Prototyp abgeschlossen / Prototype completed
- Optimierung für finale Hardware in Arbeit / Optimization for final hardware in progress

---

## Hinweis / Notes
- KI-Tools wurden unterstützend für Code und Debugging verwendet / AI tools were used to assist with code and debugging
- Alle Entscheidungen, Tests und Aufbau wurden persönlich durchgeführt / All decisions, testing, and assembly were performed personally
