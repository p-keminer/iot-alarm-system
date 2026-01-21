# Pinmapping – ESP8266 (Sender)

Dieses Dokument beschreibt die Pinbelegung für ein ESP8266-Board
(Status-LEDs).

Die Zuordnung ist für eine übersichtliche Statusanzeige und einfache Benutzerinteraktion ausgelegt.

---

## LEDs

| Funktion | GPIO (ESP8266) | Board-Pin | Hinweis |
|----------|---------------|-----------|--------|
| Alarm / Debug | GPIO2 | LED_BUILTIN | Onboard-LED (meist **active LOW**) |
| WLAN-Status | GPIO14 | D5 | Externe LED zur Anzeige des WLAN-Zustands |

> **Hinweis:**  
> Die Onboard-LED (LED_BUILTIN) ist auf GPIO2 gelegt und **invertiert** (`LOW = LED an`).  
> GPIO2 ist ein Boot-Strapping-Pin – beim Start muss er HIGH sein.

---

## Zusammenfassung

```text
LED_BUILTIN (GPIO2) → Alarm / Debug
D5 (GPIO14)         → WLAN-Status LED
