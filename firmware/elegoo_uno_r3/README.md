# Alarmsystem - Arduino Nano

RFID-gesteuerte Alarmanlage mit Magnetsensoren, HAL-Architektur und Pico2W-Anbindung.

---

## Features
- RFID-Zutrittskontrolle (MFRC522)
- Zwei redundante Magnetsensoren (Alarm nur wenn beide offen)
- USB-Heartbeat-Watchdog zur Verbindungsueberwachung
- Hardware-Watchdog gegen Absturz
- Remote-Steuerung via Pico2W
- State Machine Architektur

---

## Workflow
w
```
                         ┌─────────────────────────────────────────┐
                         │              SYSTEM START               │
                         └────────────────────┬────────────────────┘
                                              ▼
                         ┌─────────────────────────────────────────┐
                         │           STATUS:SYSTEM_BEREIT          │
                         └────────────────────┬────────────────────┘
                                              ▼
┌────────────────────────────────────────────────────────────────────────────────┐
│                                  HAUPTSCHLEIFE                                 │
│                                                                                │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐    ┌──────────────┐  │
│  │   Kommando   │───▶│    RFID      │───▶│   Sensoren  │───▶│     FSM      │ │
│  │    lesen     │    │    lesen     │    │    lesen     │    │  verarbeiten │  │
│  └──────────────┘    └──────────────┘    └──────────────┘    └──────────────┘  │
│         │                                                            │         │
│         ▼                                                            ▼         │
│  ┌──────────────┐                                           ┌──────────────┐   │
│  │   Watchdog   │                                           │     LEDs     │   │
│  │   fuettern   │                                           │ aktualisieren│   │
│  └──────────────┘                                           └──────────────┘   │
│                                                                                │
└────────────────────────────────────────────────────────────────────────────────┘
```

---

## State Machine

```
                    ┌──────────────────┐
        ┌───────────│     UNSCHARF     │◄───────────┐
        │           └────────┬─────────┘            │
        │                    │                      │
        │   KARTE_ERLAUBT    │     KARTE_ERLAUBT    │
        │   CMD_SCHARF       │     CMD_UNSCHARF     │
        │                    │     ALARM_STOP       │
        │                    ▼                      │
        │           ┌──────────────────┐            │
        │           │      SCHARF      │────────────┤
        │           └────────┬─────────┘            │
        │                    │                      │
        │   SENSOR_OFFEN     │                      │
        │   ERZWINGE_ALARM   │                      │
        │                    ▼                      │
        │           ┌──────────────────┐            │
        └───────────│      ALARM       │────────────┘
                    │    (Buzzer an)   │
                    └──────────────────┘
```

---

## Ordnerstruktur

### Arduino IDE (flach)

```
Dokumente/Arduino/
└── alarm_system/
    ├── alarm_system.ino    # Hauptprogramm
    ├── hal_io.h            # GPIO Header
    ├── hal_io.cpp          # GPIO Implementierung
    ├── hal_rfid.h          # RFID Header
    ├── hal_rfid.cpp        # RFID Implementierung
    ├── hal_comm.h          # Kommunikation Header
    ├── hal_comm.cpp        # Kommunikation Implementierung
    ├── hal_time.h          # Zeit Header
    ├── hal_time.cpp        # Zeit Implementierung
    ├── hal_system.h        # System Header
    ├── hal_system.cpp      # System Implementierung
    ├── alarm_fsm.h         # State Machine Header
    ├── alarm_fsm.cpp       # State Machine Implementierung
    ├── uid_check.h         # UID-Pruefung Header
    ├── uid_check.cpp       # UID-Pruefung Implementierung
    └── README.md
```

### VS Code + PlatformIO (strukturiert)

```
alarm_system/
├── platformio.ini          # PlatformIO Konfiguration
├── README.md
└── src/
    ├── main.cpp            # Hauptprogramm
    ├── hal/                # Hardware Abstraction Layer
    │   ├── hal_io.h
    │   ├── hal_io.cpp
    │   ├── hal_rfid.h
    │   ├── hal_rfid.cpp
    │   ├── hal_comm.h
    │   ├── hal_comm.cpp
    │   ├── hal_time.h
    │   ├── hal_time.cpp
    │   ├── hal_system.h
    │   └── hal_system.cpp
    └── logic/              # Anwendungslogik
        ├── alarm_fsm.h
        ├── alarm_fsm.cpp
        ├── uid_check.h
        └── uid_check.cpp
```

---

## Installation

### Arduino IDE

1. Ordner `alarm_system` nach `Dokumente/Arduino/` kopieren
2. `alarm_system.ino` oeffnen
3. Bibliothek "MFRC522" installieren (Werkzeuge → Bibliotheken verwalten)
4. UIDs in `uid_check.cpp` eintragen
5. Board: Arduino Nano, Prozessor: ATmega328P
6. Hochladen

### VS Code + PlatformIO

1. VS Code + PlatformIO Extension installieren
2. Ordner oeffnen
3. UIDs in `src/logic/uid_check.cpp` eintragen
4. Build & Upload (Strg+Alt+U)

---

## Dateien

| Datei | Funktion |
|-------|----------|
| `alarm_system.ino` | Hauptprogramm, Orchestrierung |
| `hal_io.*` | GPIO: Sensoren, LEDs, Buzzer |
| `hal_rfid.*` | RFID-Leser (MFRC522) |
| `hal_comm.*` | Serielle Kommunikation, Protokoll |
| `hal_time.*` | Zeitfunktionen (millis-Wrapper) |
| `hal_system.*` | Systemfunktionen (Reboot) |
| `alarm_fsm.*` | Zustandsautomat |
| `uid_check.*` | RFID-Whitelist |

---

## Kommunikationsprotokoll

### Eingehend (Pico2W → Arduino)

| Kommando | Funktion |
|----------|----------|
| `1` / `SCHARF` | Alarm scharfschalten |
| `0` / `UNSCHARF` | Alarm unscharfschalten |
| `ERZWINGE_ALARM` | Alarm sofort ausloesen |
| `ALARM_STOP` | Alarm stoppen |
| `STATUS` | Statusabfrage |
| `REBOOT` | Neustart |
| `HB_ACK` | Heartbeat bestaetigen |

### Ausgehend (Arduino → Pico2W)

| Nachricht | Bedeutung |
|-----------|-----------|
| `STATUS:SYSTEM_BEREIT` | System gestartet |
| `STATUS:ALARM_SCHARF` | Scharfgeschaltet |
| `STATUS:ALARM_UNSCHARF` | Unscharfgeschaltet |
| `ALARM_ON` | Alarm ausgeloest |
| `ALARM_OFF` | Alarm beendet |
| `STATUS:ALARM_ERZWUNGEN` | Manuell ausgeloest |
| `STATUS:ALARM_GESTOPPT` | Manuell gestoppt |
| `STATUS:VERBINDUNG_VERLOREN` | Heartbeat-Timeout |
| `STATUS:VERBINDUNG_AKTIV` | Verbindung wiederhergestellt |
| `STATUS:REBOOT` | Neustart eingeleitet |
| `HB` | Heartbeat-Anfrage |

### Statusabfrage

Anfrage: `STATUS`

Antwort: `STATUS:SCHARF=1,AUSGELOEST=0,TUER1=ZU,TUER2=ZU,VERBINDUNG=OK`

---

## Redundanz-Logik

Alarm wird NUR ausgeloest wenn **beide** Sensoren offen sind:

| Sensor 1 | Sensor 2 | Alarm |
|----------|----------|-------|
| ZU | ZU | Nein |
| OFFEN | ZU | Nein |
| ZU | OFFEN | Nein |
| OFFEN | OFFEN | **Ja** |

→ Schutz vor Fehlalarmen durch Wackelkontakt oder Kabelbruch

---

## Watchdogs

### Hardware-Watchdog (WDT)

- Timeout: 2 Sekunden
- Reset bei Programmabsturz
- Wird in jeder Loop gefuettert

### Heartbeat-Watchdog (Software)

- Sendet alle 5s `HB` an Pico2W
- Erwartet `HB_ACK` innerhalb 15s
- Bei Timeout: `STATUS:VERBINDUNG_VERLOREN`
- Alarm funktioniert weiterhin lokal

---

## Pin-Belegung

| Pin | Funktion | Anschluss |
|-----|----------|-----------|
| 2 | TX | ESP Sender |
| 2 | Reed-Sensor 1 | Magnetsensor Tuer/Fenster |
| 3 | Reed-Sensor 2 | Magnetsensor Tuer/Fenster |
| 5 | Buzzer | Piezo-Summer |
| 6 | LED Alarm | Status: Scharf |
| 7 | LED Sensor 1 | Status: Tuer 1 |
| 8 | LED Sensor 2 | Status: Tuer 2 |
| 9 | RFID RST | MFRC522 Reset |
| 10 | RFID SS | MFRC522 Slave Select |
| 11 | RFID MOSI | SPI Daten |
| 12 | RFID MISO | SPI Daten |
| 13 | RFID SCK | SPI Clock |

---

## Schaltplan

```
                          Elegoo Uno
                         ┌────────────┐
                         │        TX  │────► ESP Sender
                         │        D2  │◄──── Reed-Sensor 1 ────┐
                         │        D3  │◄──── Reed-Sensor 2 ────┤
                         │            │                        │
                         │        D5  │────► Buzzer (+)        │
                         │        D6  │────► LED Alarm         │
                         │        D7  │────► LED Sensor 1      │
                         │        D8  │────► LED Sensor 2      │
                         │            │                        │
                         │        D9  │────► RFID RST          │
                         │       D10  │────► RFID SDA          │
                         │       D11  │────► RFID MOSI         │
                         │       D12  │◄──── RFID MISO         │
                         │       D13  │────► RFID SCK          │
                         │            │                       GND
                         │       USB  │◄───► Pico2W (Serial)
                         │            │
                         │       3.3V │────► RFID VCC
                         │        GND │────► GND
                         └────────────┘
```

---

## Konfiguration

In `alarm_system.ino`:

```cpp
static const uint32_t BAUDRATE = 9600;
static const uint32_t HEARTBEAT_INTERVALL_MS = 5000;   // Heartbeat alle 5s
static const uint32_t WATCHDOG_TIMEOUT_MS = 15000;     // Timeout nach 15s
static const uint32_t RFID_SPERRZEIT_MS = 2000;        // Entprellung 2s
```

In `uid_check.cpp`:

```cpp
static const ErlaubteUID WHITELIST[] = {
  {{0xAB, 0xCD, 0xEF, 0x12}, 4},    // UID 1
  {{0x12, 0x34, 0x56, 0x78}, 4}     // UID 2
};
```

---

## Lizenz

MIT
