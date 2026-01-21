## PCB – Versorgungsvarianten (ESP + Elegoo Uno R3)

In diesem Verzeichnis befinden sich **zwei PCB-Varianten** für das **r3_SenderEsp** System, die sich ausschließlich in der Art der Stromversorgung unterscheiden.

---

## Überblick über die Varianten

### Versorgung über Elegoo Uno R3 (Basisversion)

Diese Version versorgt den ESP **direkt über den Elegoo Uno R3**.

- Ausgelegt für:
  - Grundlogik
  - geringe bis moderate WLAN-Aktivität
- Einschränkungen:
  - begrenzte Stromreserve
  - nicht optimal für hohe WLAN-Lasten oder OTA-Updates

---

### Externe Stromversorgung (Erweiterte Version)

Diese Version benötigt eine **separate externe Spannungsversorgung** für den ESP.

- Ausgelegt für:
  - hohe Stromspitzen beim WLAN-Senden und -Empfangen
  - OTA-Updates
  - stabile Funkverbindungen unter Last
- Vorteile:
  - deutlich stabilere Spannungsversorgung
  - minimiertes Risiko von Resets oder Verbindungsabbrüchen
  - bessere Skalierbarkeit für Erweiterungen

**Vorgesehen für erweiterte und leistungsintensivere Systeme**

---

### Technischer Hintergrund

Beim ESP treten insbesondere während **WLAN-Übertragungen und OTA-Vorgängen** kurzzeitig hohe Stromspitzen auf.  
Die externe Versorgungsvariante berücksichtigt diese Lastfälle gezielt, während die direkte Versorgung über den Elegoo Uno R3 für die **reine Grundfunktionalität ausreichend ist**.

---

### Empfehlung

- **Prototypen / Basislogik:** Versorgung über Elegoo Uno R3  
- **Erweiterte oder produktionsnahe Systeme:** externe Stromversorgung
