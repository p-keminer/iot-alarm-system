# Technische Dokumentation: Schwachstellenanalyse & Systemerweiterung

## 1. Aktuelle Schwachstellenanalyse (Security & Robustness)
Die folgenden Punkte sind als kritische Härtungsmaßnahmen in den jeweiligen Sketches zuletzt implementiert worden:

* **Datentyp-Validierung (Robustness):** Implementierung eines `isdigit()`-Checks vor der Konvertierung von Sequenznummern im Empfänger-Sketch, um Fehlverhalten durch korrupte Payloads zu verhindern.
* **Memory Management (Stability):** Vollständige Umstellung von `String`-Objekten auf statische `char`-Arrays und `strtok` zur Partitionierung der UDP-Pakete, um Heap-Fragmentierung und DoS-Anfälligkeit zu eliminieren.
* **Replay-Schutz & Persistenz:** Verlagerung der Sequenz-Logik in das `LittleFS`. Implementierung einer Validierung, die nur Sequenznummern innerhalb eines definierten Fensters (z. B. n+10) akzeptiert, um Replay-Attacken nach System-Reboots auszuschließen.
* **Side-Channel-Resilienz:** Einsatz einer **Constant Time Compare** Schleife für die HMAC-Validierung. Der Vergleich darf nicht beim ersten falschen Zeichen abbrechen, um Zeitmessungs-Angriffe auf den Key zu verhindern.
* **OTA:** (entfernt) USB only Wartung für weniger Angriffsvektoren.

## 2. Dauerzustandsbeschreibung (Betriebsmodus)
Definition der Verhaltensweisen im aktiven Betrieb:

* **Konnektivität:** Deaktivierung des WiFiManagers im produktiven Dauerzustand; statische Verbindung zum Ziel-WLAN zur Minimierung der Angriffsfläche.
* **Telnet-Honeypot:** Betrieb der Telnet-Schnittstelle als aktives Täuschungssystem.
* **Netzwerk-Filterung:** Definition der Trust-Boundary; Layer-3 Flooding-Schutz, sowie VPN. wird als externe Leistung durch den Router vorausgesetzt.


## 3. Mögliche Projekterweiterung
Hardware- und Protokoll-Upgrades:

* **Hardware-Migration:** Umstellung auf **ESP32** zur Nutzung von Dual-Core-Isolierung und Hardware-Krypto-Beschleunigern.
* **Protokoll-Standardisierung:** Integration von **MQTTS** oder MQTT mit dedizierter **AES-GCM** Payload-Verschlüsselung für End-to-End Sicherheit.
* **Akustische Signalisierung:** Erweiterung um **Passive Buzzer** an PWM-fähigen Pins zur akustischen Differenzierung verschiedener Alarmzustände und Systemereignisse.

* **Dead Man's Switch (Jamming-Schutz):**
Implementierung einer Überwachungslogik gegen Layer-1 Angriffe:

- **Hysterese-Logik:** Überwachung des vorherigem RSSI Signal und verfügbarer Anzahl an WLAN-Netzwerken durch ein externes Python-Skript bei Verbindungsverlust.
- **Eskalations-Zeitraum:** Festlegung eines 60-sekündigen Zeitfensters (Grace Period), um Router-Reboot-Zyklen von tatsächlichen Jamming-Angriffen zu differenzieren.
- **Alarm-Trigger:** Automatisches Auslösen des Alarmzustands (R3 über USB), falls innerhalb der definierten Zeitspanne keine Verbindung wiederhergestellt werden kann oder Netzwerkanzahl nicht +-2 der zuvor gespeocherten Netzwerkanzahl entspricht.