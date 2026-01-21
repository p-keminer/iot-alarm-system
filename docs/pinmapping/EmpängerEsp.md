# Pinmapping – ESP8266 (Empfänger)

Dieses Dokument beschreibt die **Pinbelegung für ein ESP8266-Board**  
(Status-LEDs, Buzzer und Taster).  
Die Zuordnung ist für eine übersichtliche Statusanzeige und einfache Benutzerinteraktion ausgelegt.

---

## Status-LEDs

| Funktion | GPIO (ESP8266) | Board-Pin | Hinweis |
|--------|---------------|-----------|--------|
| LED Rot | GPIO5 | D1 | Status-/Fehleranzeige |
| LED Gelb | GPIO4 | D2 | System-/Betriebsstatus |
| LED WLAN | GPIO0 | D3 | WLAN-Status |

> **Hinweis:**  
> GPIO0 (D3) ist ein **Boot-Strapping-Pin**.  
> Die LED darf den Pin **beim Start nicht auf LOW ziehen**, sonst startet das ESP nicht korrekt.

---

## Buzzer

| Funktion | GPIO (ESP8266) | Board-Pin | Hinweis |
|--------|---------------|-----------|--------|
| Buzzer 1 | GPIO14 | D5 | PWM-fähig |
| Buzzer 2 | GPIO12 | D6 | PWM-fähig |

> Beide Pins sind unkritisch und gut für akustische Signale geeignet.

---

## Taster

| Funktion | GPIO (ESP8266) | Board-Pin | Hinweis |
|--------|---------------|-----------|--------|
| Taster | GPIO13 | D7 | Digital Input |

> Empfehlung: internen Pull-Up verwenden (`INPUT_PULLUP`) und Taster gegen GND schalten.

---

## Zusammenfassung

```text
D1 (GPIO5)  → LED Rot
D2 (GPIO4)  → LED Gelb
D3 (GPIO0)  → LED WLAN (Boot-Pin beachten)
D5 (GPIO14) → Buzzer 1
D6 (GPIO12) → Buzzer 2
D7 (GPIO13) → Taster
