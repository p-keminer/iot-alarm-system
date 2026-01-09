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
- [Verwendete Tools](#verwendete-tools--tools-used)

## Übersicht / Overview
Dieses Projekt ist eine selbstentwickelte Basis-IoT-Alarmanlage mit zwei ESP8266-Nodes und einem Arduino R3.  
Es dient als modularer Grundbaustein, der es ermöglicht, bei Bedarf weitere Nodes hinzuzufügen und das System so flexibel zu erweitern.
Es umfasst Sensorerfassung, Kommunikation (UART&UDP) zwischen den Nodes, eine zentrale Steuerungseinheit sowie selbst entworfene PCB.  
Für die Sensoren wurden 3D-gedruckte Gehäuse verwendet.  
Dies war mein erstes praktisches Embedded-System-Projekt, umgesetzt mit minimaler Vorerfahrung im ersten Semester.  

This project is a self-developed basic IoT alarm system using two ESP8266 nodes and an Arduino R3.  
It serves as a modular foundation, allowing additional nodes to be integrated as needed, making the system flexible and easily expandable.
It includes sensor data acquisition, communication (UART&UDP) between nodes, a central control unit, and custom-designed PCB.  
3D-printed housings were used for the sensors.  
This was my first hands-on embedded systems project, developed with minimal prior experience during my first semester.

---

## Systemarchitektur / System Architecture
- Zwei ESP8266-Nodes zur Sensorerfassung und Kommunikation / Two ESP8266 nodes for sensor data acquisition and communication
- Arduino R3 als zentrale Steuerung / Arduino R3 as central controller
- Selbst entworfene PCB / Custom designed PCB  [![PCB Sending Nodes](https://img.shields.io/badge/PCB-Sending%20Nodes-blue)](hardware/pcb/r3_senderEsp/rückseite_r3_senderEsp.png)  [![PCB Receiving Nodes](https://img.shields.io/badge/PCB-Receiving%20Nodes-green)](hardware/pcb/empfaengerEsp/rückseiteEmpfaenger.png)
- 3D-gedruckte Sensorgehäuse / 3D-printed housings for sensors

**Ablauf / Workflow:** Sensoren → Arduino → Alarmlogik → ESP-Nodes → Kommunikation → Ausgabe   
**Workflow:** Sensors → Arduino → Alarm logic → ESP-Nodes → Communication → Output 

---

## Firmware
- Embedded C/C++ für ESP8266 und Arduino / Embedded C/C++ for ESP8266 and Arduino
- Node-Kommunikation und Alarmlogik / Node communication and alarm logic
- Strukturierter und kommentierter Code / Structured and well-commented code 
- Einzeltests der Nodes / Individual node testing

---

## Hardware
- Prototyp-PCB selbst gelötet / Custom PCB soldered as prototype
- Funktionsweise getestet / Functionality tested
- Schaltpläne und Lochrasterlayouts in `schematics/` und `pcb/` enthalten / Schematics and perfboard layouts included in `schematics/` and `pcb/`

---

## Mechanik / Mechanical Design
- 3D-gedruckte Gehäuse für Sensoren / 3D-printed housings for sensors
- STL-Dateien für den Druck / STL files for printing [![3D Printed Reed Sensor Housing](https://img.shields.io/badge/3D%20Print-Reed%20Sensor-blueviolet)](mechanics/3d_prints/reed_sensor/reed_sensor_gehaeuse.png) [![3D Printed RFID Sensor Housing](https://img.shields.io/badge/3D%20Print-RFID%20Sensor-blueviolet)](mechanics/3d_prints/rfid_sensor/rfid_sensor_gehaeuse.png)
- G-Code-Datei für **Bambu Lab H2S** enthalten / G-code file for **Bambu Lab H2S** included
- Alle Dateien befinden sich im Ordner `mechanics/` / All files are located in the `mechanics/` folder

---

## Assembly / Zusammenbau
- Programmierung der Nodes und Test der Sensoren / Nodes programmed and sensors tested
- Zusammenführung der Nodes und Test der Funktionalität / Nodes integrated and verified overall functionality
- Komponenten montiert und funktionsfähigen Prototyp erstellt / Components assembled into a functional prototype
- Erstellen der PCB und abschließendes verlöten / PCB designed and finally soldered
- Erstellen der Gehäuse Entwürfe und anschließender Druck / Enclosure designs created and 3D printed
- Zusammenbau der Funktionsfähigen Version für den Dauerbetrieb / Fully functional version assembled for continuous operation
- Siehe Ordner `assembly/` für Fotos und Hinweise / See `assembly/` folder for photos and notes

---

## Lessons Learned
- Dokumentiert in `docs/lessonslearned.md` / Documented in `docs/lessonslearned.md`
- Herausforderungen: Embedded Programming, Löten, Sensorintegration, Debugging / Challenges: embedded programming, soldering, sensor integration, debugging
- Iteratives Vorgehen und Dokumentation entscheidend / Iterative approach and documentation are essential

---

## Status
- Prototyp abgeschlossen / Prototype completed [![Prototype Breadboard](https://img.shields.io/badge/Prototype-Breadboard-orange)](photos/prototyp_breadboards.png) [![Prototype Perfboard](https://img.shields.io/badge/Prototype-Perfboard-orange)](photos/prototyp_perforatedCircuitBoards.jpg) 
- Optimierung für finale Hardware in Arbeit / Optimization for final hardware in progress

---

## Hinweis / Notes
- KI-Tools wurden unterstützend für Code und Debugging verwendet / AI tools were used to assist with code, debugging, and idea generation
- Alle Entscheidungen, Tests und Aufbau wurden persönlich durchgeführt / All decisions, testing, and assembly were performed personally

---

## Verwendete Tools / Tools Used

- **KiCAD** – PCB-Design und Schaltpläne / PCB design and schematics  
- **Tinkercad** – Simulation und Prototyping / Simulation and prototyping  
- **DIY Layout Creator** – Layout-Erstellung für Lötpläne / Layout creation for soldering plans 
- **Bambu Lab Studio** – 3D-Druck-Slicing für H2S / 3D printing slicing for H2S  
- **ChatGPT & Google Gemini** – Unterstützende Tools für Code, Debugging und Ideen / Assistive tools for code, debugging, and idea generation
