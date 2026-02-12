# Pinmapping – ESP8266 (Empfänger)

Dieses Dokument beschreibt die **Pinbelegung für ein ESP8266-Board**  
(LEDs, Buzzer und Taster).  
Die Zuordnung ist für eine übersichtliche Statusanzeige und einfache Benutzerinteraktion ausgelegt.

---

## LEDs

| Funktion | GPIO (ESP8266) | Board-Pin | Hinweis |
|--------|---------------|-----------|--------|
| LED Rot | GPIO5 | D1 | Alarmanzeige |
| LED Gelb | GPIO4 | D2 | Alarmanzeige |
| LED WLAN | GPIO0 | D3 | WLAN-Status |

 


---

## Buzzer

| Funktion | GPIO (ESP8266) | Board-Pin | Hinweis |
|--------|---------------|-----------|--------|
| Buzzer 1 | GPIO14 | D5 | PWM-fähig |
| Buzzer 2 | GPIO12 | D6 | PWM-fähig |

---

## Taster

| Funktion | GPIO (ESP8266) | Board-Pin | Hinweis |
|--------|---------------|-----------|--------|
| Taster | GPIO13 | D7 | Input |

---

## Zusammenfassung

```text
D1 (GPIO5)  → LED Rot
D2 (GPIO4)  → LED Gelb
D3 (GPIO0)  → LED WLAN (Boot-Pin beachten)
D5 (GPIO14) → Buzzer 1
D6 (GPIO12) → Buzzer 2
D7 (GPIO13) → Taster
