# IoT-Basis-Alarmanlage / IoT Basic Alarm System

**Kurzbeschreibung / Brief Description:**  
IoT-Alarmanlagen-Basissystem auf ESP8266 und Elegoo Uno R3-Basis mit magnetischen Hall-Sensoren (KY-021) und RFID-Zugangskontrolle (RC522). Das System verarbeitet Sensordaten lokal, steuert Ausgaben wie LED und Buzzer, kommuniziert über UART und UDP und ist modular erweiterbar. Ziel ist ein praxisnahes Embedded-/IoT-Projekt eines Studierenden der THGA Bochum.

Basic IoT alarm system based on ESP8266 and Elegoo Uno R3, featuring magnetic Hall-effect sensors (KY-021) and RFID access control (RC522). The system processes sensor data locally, controls outputs such as LEDs and buzzers, communicates via UART and UDP, and is designed for modular expansion. This project serves as a hands-on Embedded/IoT application developed by a student at THGA Bochum.

---

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md) [![PCB Sending Nodes](https://img.shields.io/badge/PCB-Sending%20Nodes-blue)](hardware/pcb/r3_senderEsp/rückseite_r3_senderEsp.png)  [![PCB Receiving Nodes](https://img.shields.io/badge/PCB-Receiving%20Nodes-blue)](hardware/pcb/empfaengerEsp/rückseiteEmpfaenger.png) [![Elegoo Uno R3](https://img.shields.io/badge/Firmware-Elegoo%20Uno%20R3-green)](firmware/elegoo_uno_r3/sketchR3/sketchR3.ino) [![ESP8266 Empfänger](https://img.shields.io/badge/Firmware-ESP8266%20Empfänger-green)](firmware/esp8266_nodes/sketch_empfaengerESP/sketch_empfaengerESP.ino) [![ESP8266 Sender](https://img.shields.io/badge/Firmware-ESP8266%20Sender-green)](firmware/esp8266_nodes/sketch_senderESP/sketch_senderESP.ino) [![3D Printed Reed Sensor Housing](https://img.shields.io/badge/3D%20Print-Reed%20Sensor-red)](mechanics/3d_prints/reed_sensor/reed_sensor_gehaeuse.stl) [![3D Printed RFID Sensor Housing](https://img.shields.io/badge/3D%20Print-RFID%20Sensor-red)](mechanics/3d_prints/rfid_sensor/rfid_sensor_gehaeuse.stl) [![Prototype Breadboard](https://img.shields.io/badge/Prototype-Breadboard-pink)](photos/prototyp_breadboards.png) [![Prototype Perfboard](https://img.shields.io/badge/Prototype-Perfboard-pink)](photos/prototyp_perforatedCircuitBoards.jpg) ![Schematic: Sending](https://img.shields.io/badge/Schematic-%20Sending-orange)[![Schematic: Receiving](https://img.shields.io/badge/Schematic-%20Receiving-orange)](hardware/schematics/circuitDiagram/empfaengerEsp.png) [![Schematic: Sending](https://img.shields.io/badge/Schematic-%20Sending_KiCad-blueviolet)](hardware/pcb/r3_senderEsp/schaltplan_r3_senderEsp.png) [![Schematic: Receiving](https://img.shields.io/badge/Schematic-%20Receiving_KiCad-blueviolet)](hardware/pcb/empfaengerEsp/schaltplanEmpfaenger.png)
 [![Hardware: Components](https://img.shields.io/badge/Hardware-%20Components-violet)](hardware/assembly/components.md) [![Prototyp: Gehäuse](https://img.shields.io/badge/Prototyp-%20Gehäuse-white)](photos/gehauesePrototyp.png)

---

## 📑 Inhaltsverzeichnis
- [Übersicht / Overview](#übersicht--overview)
- [Systemarchitektur / System Architecture](#systemarchitektur--system-architecture)
- [Firmware](#firmware)
- [Hardware](#hardware)
- [Projekt-Highlights / Features](#projekt-highlights--features)
- [Mechanik / Mechanical Design](#mechanik--mechanical-design)
- [Zusammenbau / Assembly](#zusammenbau--assembly) 
- [Reflektion / Lessons Learned](#reflektion--lessons-learned)
- [Status](#status)
- [Hinweis / Notes](#hinweis--notes)
- [Verwendete Tools / Tools Used](#verwendete-tools--tools-used)

---

## Übersicht / Overview
Dieses Projekt ist eine selbstentwickelte Basis-IoT-Alarmanlage mit zwei ESP8266-Nodes und einem Elegoo Uno R3.  
Es dient als modularer Grundbaustein, der es ermöglicht, bei Bedarf weitere Nodes hinzuzufügen und das System so flexibel und einfach zu erweitern.
Es umfasst Sensorerfassung, Kommunikation (UART&UDP) zwischen den Nodes, eine zentrale Steuerungseinheit sowie selbst entworfene PCBs.  
Für die Sensoren wurden 3D-gedruckte Gehäuse verwendet.  
Dies war mein erstes praktisches Embedded-System-Projekt, umgesetzt mit minimaler Vorerfahrung im ersten Semester.  

This project is a self-developed basic IoT alarm system using two ESP8266 nodes and an Elegoo Uno R3.  
It serves as a modular foundation, allowing additional nodes to be integrated as needed, making the system flexible and easily expandable.
It includes sensor data acquisition, communication (UART&UDP) between nodes, a central control unit, and custom-designed PCBs.  
3D-printed housings were used for the sensors.  
This was my first hands-on embedded systems project, developed with minimal prior experience during my first semester.

---

## Systemarchitektur / System Architecture
- Zwei ESP8266-Nodes zur Kommunikation / Two ESP8266-Nodes for communication
- Elegoo Uno R3 als zentrale Steuerung / Elegoo Uno R3 as central controller
- Selbst entworfene PCBs / Custom designed PCBs  [![PCB Sending Nodes](https://img.shields.io/badge/PCB-Sending%20Nodes-blue)](hardware/pcb/r3_senderEsp/rückseite_r3_senderEsp.png)  [![PCB Receiving Nodes](https://img.shields.io/badge/PCB-Receiving%20Nodes-blue)](hardware/pcb/empfaengerEsp/rückseiteEmpfaenger.png)
- 3D-gedruckte Sensorgehäuse / 3D-printed housings for sensors [![Prototyp: Gehäuse](https://img.shields.io/badge/Prototyp-%20Gehäuse-white)](photos/gehauesePrototyp.png)

Der folgende Workflow zeigt, wie Sensoren und Aktoren über die Steuerungseinheit und ESP-Nodes interagieren.  
The following workflow shows how sensors and actuators interact via the control unit and ESP nodes.

**Workflow / Ablauf:**  
RC522 + KY-021 → Elegoo Uno R3 (Alarm-Logik / Alarm logic) → LED + Buzzer → Kommunikation / Communication (UART) →  ESP-Nodes → Kommunikation / Communication (WLAN/UDP) → LED + Buzzer

---

## Firmware
- Embedded C/C++ für ESP8266 und Elegoo Uno R3 / Embedded C/C++ for ESP8266 and Elegoo Uno R3
- Node-Kommunikation und Alarmlogik / Node communication and alarm logic
- Strukturierter und kommentierter Code / Structured and well-commented code 
- Einzeltests der Nodes / Individual node testing
[![Elegoo Uno R3](https://img.shields.io/badge/Firmware-Elegoo%20Uno%20R3-green)](firmware/elegoo_uno_r3/sketchR3/sketchR3.ino) [![ESP8266 Empfänger](https://img.shields.io/badge/Firmware-ESP8266%20Empfänger-green)](firmware/esp8266_nodes/sketch_empfaengerESP/sketch_empfaengerESP.ino) [![ESP8266 Sender](https://img.shields.io/badge/Firmware-ESP8266%20Sender-green)](firmware/esp8266_nodes/sketch_senderESP/sketch_senderESP.ino)

---

## Hardware

| Komponente / Component | Typ / Modell / Type | Beschreibung / Function |
|------------------------|------------------|------------------------|
| MCU | ESP8266 | Ermöglicht drahtlose Kommunikation (UDP), verarbeitet Events / Enables wireless communication (UDP), processes events |
| MCU | Elegoo Uno R3 | Zentrale Alarm-Logik, sammelt Sensordaten / Central alarm logic, collects sensor data |
| Sensor | KY-021 | Magnetischer Hall-Sensor zur Tür-/Fensterüberwachung / Magnetic Hall sensor for door/window detection |
| Sensor | RC522 | RFID-Modul für Zugangskontrolle / RFID module for access control |
| Aktor / Actuator | LED | Visuelle Alarmanzeige / Visual alarm indicator |
| Aktor / Actuator | Buzzer | Akustische Alarmanzeige / Acoustic alarm indicator |
| PCB | Custom | Selbst entworfene Leiterplatten für Nodes / Custom designed PCBs for nodes |
| Gehäuse / Housing | 3D-gedruckt / 3D-printed | Schützt Sensoren und Elektronik / Protects sensors and electronics |


[![Schematic: Sending](https://img.shields.io/badge/Schematic-%20Sending-orange)](hardware/schematics/circuitDiagram/r3_senderEsp.png) 
[![Schematic: Receiving](https://img.shields.io/badge/Schematic-%20Receiving-orange)](hardware/schematics/circuitDiagram/empfaengerEsp.png) [![Schematic: Sending](https://img.shields.io/badge/Schematic-%20Sending_KiCad-blueviolet)](hardware/pcb/r3_senderEsp/schaltplan_r3_senderEsp.png) [![Schematic: Receiving](https://img.shields.io/badge/Schematic-%20Receiving_KiCad-blueviolet)](hardware/pcb/empfaengerEsp/schaltplanEmpfaenger.png) [![Hardware: Components](https://img.shields.io/badge/Hardware-%20Components-violet)](hardware/assembly/components.md)

---

## Projekt Highlights / Features

- Magnetische Tür-/Fensterüberwachung mit KY-021 Sensoren / Magnetic door/window monitoring with KY-021 sensors  
- RFID-basierte Zugangskontrolle über RC522 / RFID-based access control via RC522  
- Ereignisbasierte Auslösung von LED und Buzzer / Event-based triggering of LED and buzzer  
- Lokale Alarm-Logik auf Elegoo Uno R3 / Local alarm logic on Elegoo Uno R3  
- Kommunikation zwischen ESP8266-Nodes für verteilte Sensorik / Communication between ESP8266 nodes for distributed sensing  
- Modular erweiterbar für zusätzliche Sensoren oder Aktoren, z. B. ESP32-Kamera-Modul oder PIR-Bewegungssensoren / Modularly extendable for additional sensors or actuators, e.g., ESP32 camera module or PIR motion sensors  
- Prototypische IoT-Funktionalität über UART und UDP / Prototype IoT functionality via UART and UDP

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
- Montage der Komponenten und funktionsfähigen Prototyp erstellt / Components assembled into a functional prototype
- Design der PCBs und abschließende Lötarbeiten / PCBs designed and finally soldered
- Entwurf der Gehäuse und anschließender 3D-Druck / Enclosure designs created and 3D printed
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
- Entwicklungsmethode:  Das Projekt wurde nach dem Prinzip des 'AI-assisted Engineering' umgesetzt. Während die      Systemarchitektur, die Hardware-Auswahl und das Logik-Konzept (Redundanz, Master-Slave-Struktur) von mir entworfen wurden, kam KI zur Code-Optimierung und zum Rapid Prototyping der Netzwerk-Schnittstellen zum Einsatz.

  Development Method: The project was carried out following the 'AI-assisted Engineering' approach. I was       responsible for designing the   system architecture, selecting the hardware, and defining the logic concept     (including redundancy and a master-      slave structure), while AI was employed to optimize code and accelerate prototyping of the network interfaces.

---

## Verwendete Tools / Tools Used
- **[KiCad](https://www.kicad.org/)** – PCB-Design und Schaltpläne / PCB design and schematics  
- **[Tinkercad](https://www.tinkercad.com/)** – Simulation und Prototyping / Simulation and prototyping  
- **[DIY Layout Creator](https://bancika.github.io/diy-layout-creator/)** – Layout-Erstellung für Lötpläne / Layout creation for soldering plans  
- **[Bambu Lab Studio](https://bambulab.com/en/download/studio)** – 3D-Druck-Slicing für H2S / 3D printing slicing for H2S  
- **[ChatGPT](https://chat.openai.com/) & [Google Gemini](https://gemini.google.com/)** – Unterstützende Tools für Code, Debugging und Ideen / Assistive tools for code, debugging, and idea generation
