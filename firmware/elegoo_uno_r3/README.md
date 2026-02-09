# Alarmsystem - Arduino

## Installation

1. Ordner `alarm_system` nach `Dokumente/Arduino/` kopieren
2. `alarm_system.ino` mit Arduino IDE oeffnen
3. Bibliothek "MFRC522" installieren (Bibliotheksverwalter)
4. UIDs in `uid_check.cpp` eintragen
5. Hochladen


## Dateien

| Datei | Funktion |
|-------|----------|
| `alarm_system.ino` | Hauptprogramm |
| `hal_io.*` | GPIO (Sensoren, LEDs, Buzzer) |
| `hal_rfid.*` | RFID-Leser |
| `hal_comm.*` | Serielle Kommunikation |
| `hal_time.*` | Zeitfunktionen |
| `hal_system.*` | Reboot |
| `alarm_fsm.*` | Zustandsautomat |
| `uid_check.*` | RFID-Whitelist |


## Kommandos (Seriell/Pico2W)

| Senden | Antwort | Funktion |
|--------|---------|----------|
| `1` oder `SCHARF` | `STATUS:ALARM_SCHARF` | Scharfschalten |
| `0` oder `UNSCHARF` | `STATUS:ALARM_UNSCHARF` | Unscharfschalten |
| `ERZWINGE_ALARM` | `ALARM_ON` | Alarm sofort ausloesen |
| `ALARM_STOP` | `STATUS:ALARM_GESTOPPT` | Alarm stoppen |
| `STATUS` | `STATUS:SCHARF=...` | Statusabfrage |
| `REBOOT` | `STATUS:REBOOT` | Neustart |
| `HB_ACK` | - | Heartbeat-Bestaetigung |


## Redundanz-Logik

Alarm wird NUR ausgeloest wenn **beide** Sensoren offen sind:

| Sensor 1 | Sensor 2 | Alarm |
|----------|----------|-------|
| ZU | ZU | Nein |
| OFFEN | ZU | Nein |
| ZU | OFFEN | Nein |
| OFFEN | OFFEN | Ja |


## Pin-Belegung

| Pin | Funktion |
|-----|----------|
| 2 | Reed-Sensor 1 |
| 3 | Reed-Sensor 2 |
| 5 | Buzzer |
| 6 | LED Alarm |
| 7 | LED Sensor 1 |
| 8 | LED Sensor 2 |
| 9 | RFID RST |
| 10 | RFID SS |
| 11 | RFID MOSI |
| 12 | RFID MISO |
| 13 | RFID SCK |
