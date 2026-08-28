<a id="top"></a>

<div align="center">

[![Deutsch](https://img.shields.io/badge/🇩🇪_Deutsch-24292f?style=for-the-badge)](#deutsch)
[![English](https://img.shields.io/badge/🇬🇧_English-24292f?style=for-the-badge)](#english)

</div>

---

<a id="deutsch"></a>

# <a id="de-hardware"></a>Hardware

Die Pinbelegung ist aus den drei aktiven Sketchen abgeleitet. Schaltbilder und KiCad-Dateien bleiben die Referenz fuer den realen Aufbau; vor dem Einschalten immer gegen die vorhandene Platine pruefen.

<a id="de-komponenten"></a>

## Komponenten

| Anzahl | Komponente | Aufgabe |
|---:|---|---|
| 1 | Elegoo Uno R3 / ATmega328P | lokale Alarm-FSM, RFID und Reed-Sensoren |
| 2 | NodeMCU V2 / ESP8266 | Funk-Sender und entfernter Empfaenger |
| 1 | Raspberry Pi Zero 2 W | Dashboard, API, Serial-Monitor, optionale Kamera |
| 1 + 2 | MFRC522 + KY-021 / Reed-Modul | RFID-Zugang und Tuer-/Fensterkontakte |
| 3 + 7 | aktive Summer + LEDs | akustischer Alarm und Statusanzeigen |
| 1 | Raspberry-Pi-Kameramodul | optionale Alarmaufnahme |

Erforderlich sind ausserdem passende LED-Vorwiderstaende, ein Pegelwandler oder berechneter Spannungsteiler fuer Uno TX, Taster, Steckverbinder und eine stabile Versorgung.

<a id="de-pins"></a>

## Pinbelegung

### <a id="de-uno-pins"></a>Uno R3

| Pin | Funktion | Hinweis |
|---|---|---|
| `D0/RX` | Befehle ueber USB-Serial | gemeinsame Hardware-UART |
| `D1/TX` | Events an ESP-Sender und USB-Serial | `5 V`; zum ESP pegeln |
| `D2`, `D3` | Reed-Sensor 1 und 2 | `INPUT_PULLUP`; `HIGH` = offen |
| `D5` | lokaler Summer | Ausgang |
| `D6`, `D7`, `D8` | Alarm-/Scharf-LED, Sensor-LED 1 und 2 | Ausgang |
| `D9` | MFRC522 `RST` | `3.3 V`-Peripherie |
| `D10` | MFRC522 `SS/SDA` | SPI Chip Select |
| `D11`, `D12`, `D13` | MFRC522 `MOSI`, `MISO`, `SCK` | `D13` teilt die Board-LED |

Bereits ein offener Reed-Kreis loest im scharfen Zustand aus. Scharf- und Alarmzustand liegen mit Sequenz, CRC und Commit-Markierung in zwei EEPROM-Slots; ein beschaedigter nichtleerer Stand startet fail-secure im Alarm. Ohne lokale UID-Datei akzeptiert der Uno keine RFID-Karte.

### <a id="de-sender-pins"></a>ESP8266-Sender

| NodeMCU-Pin | GPIO | Funktion |
|---|---:|---|
| `RX` | `GPIO3` | `ALARM_ON`/`ALARM_OFF` vom Uno, `9600` Baud |
| `D3` | `GPIO0` | Factory-Reset-Taster, `INPUT_PULLUP` |
| `D5` | `GPIO14` | WLAN-LED |
| `LED_BUILTIN` | `GPIO2` | Alarm-/Sende-LED, aktiv-low |

Der Factory Reset wird erst nach mehr als zehn Sekunden Tastendruck beim Loslassen ausgeloest. `GPIO0` und `GPIO2` beeinflussen den Bootmodus; externe Schaltungen muessen die erforderlichen Einschaltpegel erhalten.

### <a id="de-empfaenger-pins"></a>ESP8266-Empfaenger

| NodeMCU-Pin | GPIO | Funktion |
|---|---:|---|
| `D1`, `D2` | `GPIO5`, `GPIO4` | rote und gelbe Alarm-LED |
| `D3` | `GPIO0` | WLAN-LED |
| `D5`, `D6` | `GPIO14`, `GPIO12` | Summer 1 und 2 |
| `D7` | `GPIO13` | Toggle-/Factory-Reset-Taster, `INPUT_PULLUP` |

Ein Druck unter einer Sekunde schaltet den entfernten Alarm um. Mehr als zehn Sekunden loesen beim Loslassen den Factory Reset aus; dazwischen erfolgt keine Aktion.

<a id="de-versorgung"></a>

## Versorgung und Pegel

- Alle verbundenen Baugruppen brauchen eine gemeinsame Masse.
- ESP8266-GPIOs und MFRC522 arbeiten mit `3.3 V`; Uno TX zum ESP pegeln.
- Eine stabile Versorgung und Abblockung muessen WLAN-Stromspitzen tragen.
- Fuer einen per USB versorgten Uno ist der Sender mit externer ESP-Versorgung die empfohlene Variante.
- Stromhungrige Summer oder mehrere LEDs bei Bedarf ueber Treiber schalten.
- Es gibt keine galvanische Trennung. Vor Umverdrahtung Strom und USB trennen.

<a id="de-pcb"></a>

## Leiterplatten und Mechanik

| Pfad | Inhalt | Fertigungsdaten |
|---|---|---|
| `hardware/pcb/receiver/` | Empfaenger-KiCad-Projekt und Renderings | `receiver_gerber.zip` |
| `hardware/pcb/sender/` | Sender-Basisprojekt und Renderings | `sender_r3_gerber.zip` |
| `hardware/pcb/sender/sender_r3_external.*` | Sender mit externer ESP-Versorgung | `sender_r3_external_gerber/` |

Die ZIPs sind fuer Basis-Sender und Empfaenger kanonisch; fuer die externe Variante ist der lose Gerber-Satz die einzige Fertigungsquelle. Vor Bestellung Lagen, Bohrungen, Kontur, Leiterbahnbreiten, Polaritaeten und Abmessungen im Gerber-Viewer pruefen. Die Dateien sind keine Produktionsfreigabe.

| STL-Verzeichnis | Modell |
|---|---|
| `mechanics/prints_3d/sender/` | Sendergehaeuse R3 |
| `mechanics/prints_3d/receiver/` | Empfaengergehaeuse |
| `mechanics/prints_3d/reed_sensor/` | Reed-Sensorgehaeuse |
| `mechanics/prints_3d/rfid_sensor/` | RFID-Lesergehaeuse |

Jeder Ordner enthaelt eine STL-Datei und eine Vorschau. Toleranzen, Schraubpunkte und Ausschnitte vor dem Druck an der realen Hardware pruefen.

<a id="de-aufbau"></a>

## Aufbau

1. Versorgung und Pegel ohne Controller messen; dann Uno, RFID und beide Reed-Kreise einzeln testen.
2. Sender und Empfaenger getrennt flashen und lokal provisionieren.
3. UART nur mit gemeinsamer Masse und korrektem Pegel verbinden.
4. Funkpfad zuerst ohne laute Aktoren pruefen; Pi, API und Kamera zuletzt anbinden.

Aufbaufotos liegen unter `media/photos/`, Schalt- und Lochrasterzeichnungen unter `hardware/schematics/`.

<div align="center">

[![Nach oben](https://img.shields.io/badge/⬆_Nach_oben-24292f?style=for-the-badge)](#top)

</div>

---

<a id="english"></a>

# <a id="en-hardware"></a>Hardware

The pin map is derived from the three active sketches. Schematics and KiCad files remain the reference for the physical build; always compare them with the actual board before applying power.

<a id="en-components"></a>

## Components

| Qty | Component | Purpose |
|---:|---|---|
| 1 | Elegoo Uno R3 / ATmega328P | local alarm FSM, RFID, and reed sensors |
| 2 | NodeMCU V2 / ESP8266 | radio sender and remote receiver |
| 1 | Raspberry Pi Zero 2 W | dashboard, API, serial monitor, optional camera |
| 1 + 2 | MFRC522 + KY-021 / reed module | RFID access and door/window contacts |
| 3 + 7 | active buzzers + LEDs | audible alarm and status indicators |
| 1 | Raspberry Pi camera module | optional alarm recording |

The build also needs suitable LED resistors, a level shifter or calculated divider for Uno TX, buttons, connectors, and a stable power supply.

<a id="en-pins"></a>

## Pin Map

### <a id="en-uno-pins"></a>Uno R3

| Pin | Function | Note |
|---|---|---|
| `D0/RX` | commands over USB serial | shared hardware UART |
| `D1/TX` | events to ESP sender and USB serial | `5 V`; shift for ESP |
| `D2`, `D3` | reed sensors 1 and 2 | `INPUT_PULLUP`; `HIGH` = open |
| `D5` | local buzzer | output |
| `D6`, `D7`, `D8` | alarm/armed LED, sensor LEDs 1 and 2 | output |
| `D9` | MFRC522 `RST` | `3.3 V` peripheral |
| `D10` | MFRC522 `SS/SDA` | SPI chip select |
| `D11`, `D12`, `D13` | MFRC522 `MOSI`, `MISO`, `SCK` | `D13` shares the board LED |

Either open reed circuit triggers an armed system. Armed and alarm state use sequence, CRC, and commit markers across two EEPROM slots; corrupt non-empty state boots fail-secure into alarm. Without a local UID file, the Uno accepts no RFID card.

### <a id="en-sender-pins"></a>ESP8266 Sender

| NodeMCU pin | GPIO | Function |
|---|---:|---|
| `RX` | `GPIO3` | `ALARM_ON`/`ALARM_OFF` from Uno at `9600` baud |
| `D3` | `GPIO0` | factory-reset button, `INPUT_PULLUP` |
| `D5` | `GPIO14` | WiFi LED |
| `LED_BUILTIN` | `GPIO2` | alarm/transmit LED, active-low |

Factory reset occurs on release after holding the button for more than ten seconds. `GPIO0` and `GPIO2` affect boot mode; external circuitry must preserve the required levels during power-up.

### <a id="en-receiver-pins"></a>ESP8266 Receiver

| NodeMCU pin | GPIO | Function |
|---|---:|---|
| `D1`, `D2` | `GPIO5`, `GPIO4` | red and yellow alarm LEDs |
| `D3` | `GPIO0` | WiFi LED |
| `D5`, `D6` | `GPIO14`, `GPIO12` | buzzers 1 and 2 |
| `D7` | `GPIO13` | toggle/factory-reset button, `INPUT_PULLUP` |

A press below one second toggles the remote alarm. More than ten seconds triggers factory reset on release; the interval between them has no action.

<a id="en-power"></a>

## Power and Logic Levels

- Every connected board needs a common ground.
- ESP8266 GPIO and the MFRC522 use `3.3 V`; level-shift Uno TX to the ESP.
- A stable, decoupled supply must support WiFi current peaks.
- For a USB-powered Uno, the sender with an external ESP supply is recommended.
- Drive high-current buzzers or multiple LEDs through suitable drivers.
- There is no galvanic isolation. Disconnect power and USB before rewiring.

<a id="en-pcb"></a>

## PCBs and Mechanics

| Path | Contents | Manufacturing data |
|---|---|---|
| `hardware/pcb/receiver/` | receiver KiCad project and renderings | `receiver_gerber.zip` |
| `hardware/pcb/sender/` | base sender project and renderings | `sender_r3_gerber.zip` |
| `hardware/pcb/sender/sender_r3_external.*` | sender with external ESP supply | `sender_r3_external_gerber/` |

The ZIPs are canonical for the base sender and receiver; the loose Gerbers are the only manufacturing source for the external variant. Before ordering, verify layers, drills, outline, trace widths, polarities, and dimensions in a Gerber viewer. These files are not a production release.

| STL directory | Model |
|---|---|
| `mechanics/prints_3d/sender/` | R3 sender enclosure |
| `mechanics/prints_3d/receiver/` | receiver enclosure |
| `mechanics/prints_3d/reed_sensor/` | reed-sensor enclosure |
| `mechanics/prints_3d/rfid_sensor/` | RFID-reader enclosure |

Each directory contains one STL and one preview. Check tolerances, mounting points, and cut-outs against the real hardware before printing.

<a id="en-assembly"></a>

## Assembly

1. Measure power and logic levels without controllers; then test the Uno, RFID reader, and both reed circuits.
2. Flash and locally provision sender and receiver separately.
3. Connect UART only with common ground and the correct logic level.
4. Test the radio path without loud actuators; integrate Pi, API, and camera last.

Assembly photos are under `media/photos/`; circuit and perfboard drawings are under `hardware/schematics/`.

<div align="center">

[![Back to top](https://img.shields.io/badge/⬆_Back_to_top-24292f?style=for-the-badge)](#top)

</div>
