# Pinmapping – Elegoo Uno R3

Dieses Dokument beschreibt die vollständige **Pinbelegung** des Systems auf Basis eines  
**Elegoo Uno R3** in Kombination mit einem MFRC522 RFID-Sensor, Reed-Kontakten, Status-LEDs und einem Buzzer.

---

## RFID-Sensor (MFRC522 – SPI)

| Funktion | Arduino Pin | Hinweis |
|--------|-------------|--------|
| SDA / SS | D10 | SPI Chip-Select |
| RST | D9 | Reset-Pin |
| MOSI | D11 | SPI (Hardware-Pin) |
| MISO | D12 | SPI (Hardware-Pin) |
| SCK | D13 | SPI (Hardware-Pin) |
| VCC | 3.3 V | **Nur 3.3 V verwenden** |
| GND | GND | Masse |


---

## Reed-Sensoren (Magnetschalter)

| Sensor | Arduino Pin | Typ |
|------|-------------|----|
| Reed Sensor 1 | D2 | Input |
| Reed Sensor 2 | D3 | Input |


---

## Buzzer

| Funktion | Arduino Pin | Typ | Hinweis |
|--------|-------------|----|--------|
| Buzzer | D5 | Output | PWM-fähig |

---

## Status-LEDs

| LED | Arduino Pin | Funktion |
|---|-------------|---------|
| Alarm-LED | D6 | Alarmstatus |
| Sensor-LED 1 | D7 | Status Reed Sensor 1 |
| Sensor-LED 2 | D8 | Status Reed Sensor 2 |

---

## Zusammenfassung

```text
D2  → Reed Sensor 1
D3  → Reed Sensor 2
D5  → Buzzer
D6  → Alarm-LED
D7  → Sensor-LED 1
D8  → Sensor-LED 2
D9  → RFID RST
D10 → RFID SDA / SS
D11 → SPI MOSI
D12 → SPI MISO
D13 → SPI SCK
