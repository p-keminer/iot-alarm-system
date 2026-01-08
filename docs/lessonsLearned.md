# Lessons Learned – IoT-Alarmanlage / IoT Alarm System

Dieses Dokument fasst die wichtigsten Erfahrungen und Erkenntnisse zusammen, die während der Entwicklung des IoT-Alarmanlagen-Prototyps gesammelt wurden.  
This document summarizes the key experiences and insights gained during the development of the IoT alarm system prototype.

---

## 1. PCB-Design / KiCAD
- Gleichnamige Leitungen wie GND und +5V müssen im Schaltplan **nicht mehrfach aufgeführt werden**, da sie automatisch verbunden werden.  
  Identical nets such as GND and +5V do not need to be listed multiple times in the schematic, as they are automatically connected.
- Präzises Routing auf der PCB ist entscheidend, um Funktionsfehler zu vermeiden.  
  Precise routing on the PCB is crucial to avoid functional errors.
- Die Nutzung von Shortcuts (z. B. X, V) erleichtert den Entwurf und reduziert unnötig lange Leitungen.  
  Using shortcuts (e.g., X, V) facilitates the design and reduces unnecessarily long traces.

---

## 2. Prototyping auf Lochrasterplatine
- Prototyping auf Lochrasterplatinen ist optional; es ist möglich, direkt auf die PCB zu wechseln.  
  Prototyping on perfboard is optional; it is possible to go directly to the PCB.
- Nicht jeder Lochrasterpunkt muss verwendet werden; Flexibilität ist möglich.  
  Not every perfboard hole needs to be used; flexibility is possible.
- Buchsenleisten für Stromschienen sind nicht erforderlich.  
  Socket headers for power rails are not necessary.

---

## 3. Prototyping auf Breadboard
- Debugging über den Serial Monitor und LEDs ist äußerst hilfreich und erleichtert die Fehlersuche erheblich.  
  Debugging via the serial monitor and LEDs is extremely helpful and significantly facilitates troubleshooting.

---

## 4. Löten
- Die regelmäßige Pflege der Lötspitzen durch Reinigung und Verzinnen verlängert deren Lebensdauer.  
  Regular maintenance of soldering tips through cleaning and tinning extends their lifespan.
- Sauberes Löten ist entscheidend für stabile Verbindungen und einen funktionierenden Prototypen.  
  Clean soldering is essential for stable connections and a functioning prototype.

---

## 5. Firmware / Programmierung
- Non-blocking Delays mittels `millis()` wurden verwendet, anstelle von `delay()`, um parallele Abläufe der Nodes zu ermöglichen.  
  Non-blocking delays using `millis()` were employed instead of `delay()` to allow parallel execution of the nodes.
- Strukturierter und gut kommentierter Code erleichtert Debugging und zukünftige Erweiterungen.  
  Structured and well-commented code simplifies debugging and future enhancements.

---

## 6. Allgemeine Erkenntnisse
- Iterative Tests auf Hardware- und Softwareebene sind entscheidend, insbesondere bei ersten Projekten.  
  Iterative testing on both hardware and software is crucial, especially in first projects.
- Dokumentation unterstützt die Nachvollziehbarkeit von Fortschritten und erleichtert Fehleranalyse.  
  Documentation supports traceability of progress and facilitates error analysis.
- Der Einsatz von KI-Tools kann die Codeentwicklung und das Debugging unterstützen, ersetzt jedoch nicht das eigene Verständnis.  
  The use of AI tools can assist in code development and debugging but does not replace personal understanding.

---

## Fazit / Conclusion
Dieses Projekt verdeutlicht, dass ein systematisches Vorgehen, saubere Dokumentation und iterative Tests entscheidend sind, um ein funktionierendes Embedded-System zu realisieren.  
This project demonstrates that a systematic approach, clear documentation, and iterative testing are essential to successfully develop a functional embedded system.


