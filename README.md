# IoT-Basis-Alarmanlage / IoT Basic Alarm System

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md) [![PCB Sending Nodes](https://img.shields.io/badge/PCB-Sending%20Nodes-blue)](hardware/pcb/r3_senderEsp/rückseite_r3_senderEsp.png)  [![PCB Receiving Nodes](https://img.shields.io/badge/PCB-Receiving%20Nodes-blue)](hardware/pcb/empfaengerEsp/rückseiteEmpfaenger.png) [![UNO R3](https://img.shields.io/badge/Firmware-%20UNO%20R3-green)](firmware/elegoo_uno_r3/sketchR3/sketchR3.ino) [![ESP8266 Empfänger](https://img.shields.io/badge/Firmware-ESP8266%20Empfänger-green)](firmware/esp8266_nodes/sketch_empfaengerESP/sketch_empfaengerESP.ino) [![ESP8266 Sender](https://img.shields.io/badge/Firmware-ESP8266%20Sender-green)](firmware/esp8266_nodes/sketch_senderESP/sketch_senderESP.ino) [![3D Printed Reed Sensor Housing](https://img.shields.io/badge/3D%20Print-Reed%20Sensor-red)](mechanics/3d_prints/reed_sensor/reed_sensor_gehaeuse.stl) [![3D Printed RFID Sensor Housing](https://img.shields.io/badge/3D%20Print-RFID%20Sensor-red)](mechanics/3d_prints/rfid_sensor/rfid_sensor_gehaeuse.stl) [![Prototype Breadboard](https://img.shields.io/badge/Prototype-Breadboard-pink)](photos/prototyp_breadboards.png) [![Prototype Perfboard](https://img.shields.io/badge/Prototype-Perfboard-pink)](photos/prototyp_perforatedCircuitBoards.jpg) 

## 📑 Inhaltsverzeichnis
- [Übersicht](#übersicht--overview)
- [Systemarchitektur](#systemarchitektur--system-architecture)
- [Firmware](#firmware)
- [Hardware](#hardware)
- [Mechanik](#mechanik--mechanical-design)
- [Assembly](#zusammenbau--assembly) 
- [Lessons Learned](#reflektion--lessons-learned)
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
- Selbst entworfene PCB / Custom designed PCB  [![PCB Sending Nodes](https://img.shields.io/badge/PCB-Sending%20Nodes-blue)](hardware/pcb/r3_senderEsp/rückseite_r3_senderEsp.png)  [![PCB Receiving Nodes](https://img.shields.io/badge/PCB-Receiving%20Nodes-blue)](hardware/pcb/empfaengerEsp/rückseiteEmpfaenger.png)
- 3D-gedruckte Sensorgehäuse / 3D-printed housings for sensors

**Ablauf / Workflow:** Sensoren → Arduino → Alarmlogik → ESP-Nodes → Kommunikation → Ausgabe   
**Workflow:** Sensors → Arduino → Alarm logic → ESP-Nodes → Communication → Output 

---

## Firmware
- Embedded C/C++ für ESP8266 und Arduino / Embedded C/C++ for ESP8266 and Arduino
- Node-Kommunikation und Alarmlogik / Node communication and alarm logic
- Strukturierter und kommentierter Code / Structured and well-commented code 
- Einzeltests der Nodes / Individual node testing
[![UNO R3](https://img.shields.io/badge/Firmware-%20UNO%20R3-green)](firmware/elegoo_uno_r3/sketchR3/sketchR3.ino) [![ESP8266 Empfänger](https://img.shields.io/badge/Firmware-ESP8266%20Empfänger-green)](firmware/esp8266_nodes/sketch_empfaengerESP/sketch_empfaengerESP.ino) [![ESP8266 Sender](https://img.shields.io/badge/Firmware-ESP8266%20Sender-green)](firmware/esp8266_nodes/sketch_senderESP/sketch_senderESP.ino)
---

## Hardware
- Prototyp-PCBs selbst gelötet / Custom PCBs soldered as prototype
- Funktionsweise getestet / Functionality tested
- Schaltpläne und Lochrasterlayouts in `schematics/` und `pcb/` enthalten / Schematics and perfboard layouts included in `schematics/` and `pcb/`

---

## Mechanik / Mechanical Design
- 3D-gedruckte Gehäuse für Sensoren / 3D-printed housings for sensors
- STL-Dateien für den Druck / STL files for printing [![3D Printed Reed Sensor Housing](https://img.shields.io/badge/3D%20Print-Reed%20Sensor-red)](mechanics/3d_prints/reed_sensor/reed_sensor_gehaeuse.stl) [![3D Printed RFID Sensor Housing](https://img.shields.io/badge/3D%20Print-RFID%20Sensor-red)](mechanics/3d_prints/rfid_sensor/rfid_sensor_gehaeuse.stl)
- G-Code-Datei für **Bambu Lab H2S** enthalten / G-code file for **Bambu Lab H2S** included
- Alle Dateien befinden sich im Ordner `mechanics/` / All files are located in the `mechanics/` folder

---

## Zusammenbau / Assembly
- Programmierung der Nodes und Test der Sensoren / Nodes programmed and sensors tested
- Zusammenführung der Nodes und Test der Funktionalität / Nodes integrated and verified overall functionality
- Komponenten montiert und funktionsfähigen Prototyp erstellt / Components assembled into a functional prototype
- Erstellen der PCBs und abschließendes Verlöten / PCBs designed and finally soldered
- Erstellen der Gehäuse Entwürfe und anschließender Druck / Enclosure designs created and 3D printed
- Zusammenbau der funktionsfähigen Version für den Dauerbetrieb / Fully functional version assembled for continuous operation
- Siehe Ordner `assembly/` für Fotos und Hinweise / See `assembly/` folder for photos and notes

---

## Reflektion / Lessons Learned
- Dokumentiert in `docs/lessonsLearned.md` / Documented in `docs/lessonsLearned.md`
- Herausforderungen: Embedded Programming, Löten, Sensorintegration, Debugging / Challenges: embedded programming, soldering, sensor integration, debugging
- Iteratives Vorgehen und Dokumentation entscheidend / Iterative approach and documentation are essential

---

## Status
- Prototyp abgeschlossen / Prototype completed [![Prototype Breadboard](https://img.shields.io/badge/Prototype-Breadboard-pink)](photos/prototyp_breadboards.png) [![Prototype Perfboard](https://img.shields.io/badge/Prototype-Perfboard-pink)](photos/prototyp_perforatedCircuitBoards.jpg) 
- Optimierung für finale Hardware in Arbeit / Optimization for final hardware in progress

---

## Hinweis / Notes
- KI-Tools wurden unterstützend für Code und Debugging verwendet / AI tools were used to assist with code, debugging, and idea generation
- Alle Entscheidungen, Tests und Aufbau wurden persönlich durchgeführt / All decisions, testing, and assembly were performed personally

---

## Verwendete Tools / Tools Used
- **[KiCad](https://www.kicad.org/)** – PCB-Design und Schaltpläne / PCB design and schematics  
- **[Tinkercad](https://www.tinkercad.com/)** – Simulation und Prototyping / Simulation and prototyping  
- **[DIY Layout Creator](https://bancika.github.io/diy-layout-creator/)** – Layout-Erstellung für Lötpläne / Layout creation for soldering plans  
- **[Bambu Lab Studio](https://bambulab.com/en/download/studio)** – 3D-Druck-Slicing für H2S / 3D printing slicing for H2S  
- **[ChatGPT](https://chat.openai.com/) & [Google Gemini](https://gemini.google.com/)** – Unterstützende Tools für Code, Debugging und Ideen / Assistive tools for code, debugging, and idea generation
