# Lessons Learned – IoT-Alarmanlage / IoT Alarm System

Dieses Dokument fasst die wichtigsten Erfahrungen und Erkenntnisse zusammen, die während der Entwicklung des IoT-Alarmanlagen-Prototyps gesammelt wurden.  
This document summarizes the key experiences and insights gained during the development of the IoT alarm system prototype.

---

## 1. PCB-Design / KiCAD
- Gleichnamige Leitungen wie GND und +5V müssen im Schaltplan **nicht mehrfach aufgeführt werden**, da sie automatisch verbunden werden.  
  Identical nets such as GND and +5V do not need to be listed multiple times in the schematic, as they are automatically connected.
- Die Nutzung von Shortcuts zum Routen erleichtert den Entwurf und reduziert unnötig lange Leitungen.  
  Using shortcuts for routing facilitates the design and reduces unnecessarily long traces.

---

## 2. Prototyping on a perforated circuit board / Prototyping auf Lochrasterplatine
- Prototyping auf Lochrasterplatinen ist optional; es ist möglich, direkt auf die PCB zu wechseln.  
  Prototyping on perfboard is optional; it is possible to go directly to the PCB.
- Buchsenleisten für Stromschienen sind nicht erforderlich.  
  Socket headers for power rails are not necessary.

---

## 3. Prototyping on a breadboard / Prototyping auf Breadboard 
- Debugging über den Serial Monitor und LEDs ist äußerst hilfreich und erleichtert die Fehlersuche erheblich.  
  Debugging via the serial monitor and LEDs is extremely helpful and significantly facilitates troubleshooting.

---

## 4. Soldering / Löten
- Die regelmäßige Pflege der Lötspitzen durch Reinigung und Verzinnen verlängert deren Lebensdauer.  
  Regular maintenance of soldering tips through cleaning and tinning extends their lifespan.

---

## 5. Firmware / Programmierung
- Strukturierter und gut kommentierter Code erleichtert Debugging und zukünftige Erweiterungen.  
  Structured and well-commented code simplifies debugging and future enhancements.

---

## 6. 3D Printed Housings / 3D-gedruckte Gehäuse
- Den Zusammenbau bei zukünftigen Projekten erleichtern durch integrierte Clips, Steckverbindungen, Schraubverbindungen oder Magnetverbindungen.
- Enable easier assembly in future projects by using integrated clips, snap-fit connections, screw fasteners, or magnetic fasteners.  

---


## 7. General Insights / Allgemeine Erkenntnisse
- Iterative Tests auf Hardware- und Softwareebene sind entscheidend, insbesondere bei ersten Projekten.  
  Iterative testing on both hardware and software is crucial, especially in first projects.
- Dokumentation unterstützt die Nachvollziehbarkeit von Fortschritten und erleichtert Fehleranalyse.  
  Documentation supports traceability of progress and facilitates error analysis.
- Der Einsatz von KI-Tools kann die Codeentwicklung und das Debugging unterstützen, ersetzt jedoch nicht das eigene Verständnis.  
  The use of AI tools can assist in code development and debugging but does not replace personal understanding.

---

## Conclusion / Fazit 
Dieses Projekt verdeutlicht, dass ein systematisches Vorgehen, saubere Dokumentation und iterative Tests entscheidend sind, um ein funktionierendes Embedded-System zu realisieren.  
This project demonstrates that a systematic approach, clear documentation, and iterative testing are essential to successfully develop a functional embedded system.




