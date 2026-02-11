# ESP8266 UDP Alarm-System — Empfänger V12

**Kryptografisch gesicherter Alarm-Empfänger mit HAL/FSM-Architektur und Priority-Mode**

> Empfängt HMAC-signierte UDP-Pakete, validiert sie gegen Replay-Angriffe und steuert Alarm-Hardware (LEDs + Summer) mit minimaler Latenz. Im Alarm-Zustand werden alle Netzwerk-Tasks pausiert (Stop-the-World), damit die Hardware-Signale niemals stottern.

---

## Inhaltsverzeichnis

1. [Architektur-Überblick](#architektur-überblick)
2. [FSM-Zustandsdiagramm](#fsm-zustandsdiagramm)
3. [Schichten-Modell](#schichten-modell)
4. [Sicherheits-Features](#sicherheits-features)
5. [UDP-Paketverarbeitung](#udp-paketverarbeitung)
6. [Priority-Mode (Alarm)](#priority-mode-alarm)
7. [Hardware & Pinbelegung](#hardware--pinbelegung)
8. [Konfiguration](#konfiguration)
9. [Watchdog-System](#watchdog-system)
10. [WLAN-Failover](#wlan-failover)
11. [Heartbeat & Remote-Befehle](#heartbeat--remote-befehle)
12. [Telnet-Debug-Konsole](#telnet-debug-konsole)
13. [Abhängigkeiten](#abhängigkeiten)
14. [Schnellstart](#schnellstart)

---

## Architektur-Überblick

```
┌──────────────────────────────────────────────────────┐
│  FSM (Finite State Machine)                          │
│  fsmUpdate() → Dispatcher für alle Zustände          │
│  INIT → WLAN_VERBINDEN → BEREIT ↔ ALARM              │
│                              ↓                       │
│                         WERKSRESET                   │
├──────────────────────────────────────────────────────┤
│  Service-Schicht                                     │
│  verarbeiteUdpEmpfang()   · aktualisiereAlarmHardware│
│  verarbeiteHeartbeat()    · verarbeiteTaster()       │
│  verwalteWlanVerbindung() · pruefeTelnetZugang()     │
├──────────────────────────────────────────────────────┤
│  Security (plattformunabhängig)                      │
│  berechneHMAC()    · sichererVergleich()             │
│  nurZiffern()      · pruefeReplay()                  │
│  speichereSequenz()· ladeSequenz()                   │
├──────────────────────────────────────────────────────┤
│  HAL (Hardware Abstraction Layer)                    │
│  GPIO · WiFi · UDP · Flash · Watchdog · mDNS         │
│  Telnet · System                                     │
│  ← Nur diese Schicht ist ESP8266-spezifisch →        │
└──────────────────────────────────────────────────────┘
```

**Portierungs-Vorteil:** Bei Wechsel auf ESP32 muss ausschließlich der `HAL`-Namespace angepasst werden. FSM, Security und Service-Logik bleiben unverändert.

---

## FSM-Zustandsdiagramm

```
                    ┌──────────┐
                    │   INIT   │
                    └────┬─────┘
                         │ gpioInit, Flash, Config, Replay-Window laden
                         ▼
                ┌─────────────────┐
                │ WLAN_VERBINDEN  │
                │ WiFiManager     │
                │ Captive Portal  │
                └────────┬────────┘
                         │ Dienste starten (mDNS, Telnet, UDP)
                         ▼
              ┌──────────────────────┐            ┌───────────────────┐
              │       BEREIT         │◄──────────►│      ALARM        │
              │                      │            │                   │
              │ • UDP-Empfang        │  ALARM_ON  │ PRIORITY MODE:    │
              │ • Heartbeat an API   │ ─────────► │ • Hardware-Toggle │
              │ • Telnet-Zugang      │            │   (200ms Rot/Gelb │
              │ • WLAN-Failover      │  ALARM_OFF │    + Summer)      │
              │ • mDNS-Update        │ ◄───────── │ • UDP-Empfang     │
              │ • Taster (Toggle)    │  od. Taster│ • Taster          │
              └───┬──────────────────┘            │                   │
    Taster >10s   │                               │ PAUSIERT:         │
                  │                               │ • Heartbeat       │
                  ▼                               │ • Telnet          │
         ┌────────────┐                           │ • WLAN-Scan       │
         │ WERKSRESET  │◄─── Taster >10s ──────── │                   │
         │             │                          └───────────────────┘
         │ Flash       │
         │ formatieren │
         │ WiFi löschen│
         │ Neustart    │
         └─────────────┘
```

### Zustandsübergänge im Detail

| Von | Nach | Auslöser |
|-----|------|----------|
| INIT | WLAN_VERBINDEN | Automatisch nach Initialisierung |
| WLAN_VERBINDEN | BEREIT | WLAN verbunden oder Portal-Timeout |
| BEREIT | ALARM | UDP `ALARM_ON` empfangen oder Taster-Toggle |
| ALARM | BEREIT | UDP `ALARM_OFF` empfangen oder Taster-Toggle |
| BEREIT/ALARM | WERKSRESET | Taster >10 Sekunden gedrückt, dann losgelassen |
| WERKSRESET | *(Neustart)* | Automatisch nach Flash-Löschung |

---

## Schichten-Modell

### HAL-Namespace — `HAL::`

Alle direkten Hardware-Zugriffe sind hier gekapselt:

| Kategorie | Funktionen | Beschreibung |
|-----------|-----------|--------------|
| **GPIO** | `gpioInit()`, `wlanLed()`, `wlanLedToggle()`, `tasterGedrueckt()` | Status-LEDs, Taster |
| **Alarm-HW** | `alarmHardwareSetzen(phase)`, `alarmHardwareAus()` | Rot/Gelb LEDs + 2 Summer im Wechsel |
| **Watchdog** | `watchdogStarten()`, `watchdogStoppen()`, `watchdogFuettern()`, `watchdogAusgeloest()` | ISR-basierter Loop-Wächter (30s) |
| **System** | `init()`, `neustart()`, `zeitMs()`, `freierHeap()`, `resetGrund()`, `cpuFreigeben()` | Grundlegende System-Operationen |
| **Flash** | `flashInit()`, `flashFormatieren()` | LittleFS-Dateisystem |
| **WiFi** | `wlanVerbunden()`, `wlanVerbinden()`, `wlanTrennen()`, `wlanSsid()`, `wlanRssi()`, `wlanIp()` | Verbindungsmanagement |
| **WiFi-Scan** | `wlanScanStarten()`, `wlanScanErgebnis()`, `wlanScanSsid()`, `wlanScanRssi()`, `wlanScanLoeschen()` | Asynchroner Netzwerk-Scan |
| **UDP** | `udpStarten()`, `udpPaketVerfuegbar()`, `udpLesen()`, `udpAntworten()`, `udpFlush()` | Empfang + automatisches ACK an Absender |
| **mDNS** | `mdnsStarten()`, `mdnsUpdate()` | Netzwerk-Sichtbarkeit als `alarm-receiver.local` |
| **Telnet** | `telnetStarten()`, `telnetVerfuegbar()`, `telnetLesen()`, `telnetSchreiben()`, `telnetStoppen()` | Remote-Debug-Konsole |

**Besonderheit `udpLesen()`:** Speichert automatisch die Absender-IP und Port, sodass `udpAntworten()` das ACK direkt an den richtigen Sender schickt — ohne manuelle IP-Verwaltung.

### Security-Namespace — `Security::`

Plattformunabhängige Sicherheitsfunktionen:

| Funktion | Beschreibung |
|----------|--------------|
| `berechneHMAC()` | HMAC-SHA256 über BearSSL C-API, 64 Hex-Zeichen in Stack-Buffer |
| `sichererVergleich()` | Constant-Time XOR-Akkumulation mit `volatile` |
| `nurZiffern()` | isdigit-Validierung vor `strtoul()` |
| `pruefeReplay()` | Sliding-Window (25 Pakete) mit Bitmap |
| `speichereSequenz()` | Window-Basis in Flash sichern (alle 5 Inkremente) |
| `ladeSequenz()` | Window-Basis aus Flash wiederherstellen |

---

## Sicherheits-Features

### 1. HMAC-SHA256 Authentifizierung

```
Empfangenes Paket:  "NICE_TRY_WIRESHARK_USER:42:a1b2c3d4..."
                     ├── Befehl ──────────────┤├─ Seq ┤├── Signatur ──┤

Validierung:
  1. Payload rekonstruieren:  "NICE_TRY_WIRESHARK_USER:42"
  2. HMAC lokal berechnen:    SHA256(payload, shared_secret)
  3. Constant-Time Vergleich: berechnete_sig == empfangene_sig
```

- **char-Array Parsing** mit `strtok()` — kein Heap auf dem Hot-Path
- **Stack-Buffer** für alle Zwischenergebnisse (512 Byte Empfangspuffer)

### 2. Sliding-Window Replay-Schutz

```
Sequenznummern:  ... 38  39  40  41  42  ←── replayWindowBase = 42
                  │   │   │   │   │
Bitmap:          [1] [0] [1] [1] [1]     ←── 25 Bit breit
                  ↑               ↑
                  alt             aktuell
                  (gesehen)       (gesehen)

Neues Paket mit Seq 43:
  → Base verschiebt sich auf 43
  → Bitmap wird nach links geschoben
  → Bit 0 wird gesetzt (als "gesehen" markiert)

Replay-Versuch mit Seq 41:
  → diff = 43 - 41 = 2
  → Bit 2 prüfen → bereits gesetzt → ABGELEHNT
```

| Parameter | Wert | Beschreibung |
|-----------|------|--------------|
| Fenster-Größe | 25 | Akzeptiert Pakete innerhalb der letzten 25 Sequenznummern |
| Persistierung | alle 5 | Flash-Write nur alle 5 Inkremente (Flash-Wear-Schutz) |
| Flash-Datei | `/seq.dat` | Überlebt Reboots — keine Replay-Lücke nach Neustart |

### 3. Constant-Time HMAC-Vergleich

```c
// Standard-strcmp: Bricht beim ersten Unterschied ab
//   → Angreifer misst Antwortzeit
//   → Kann Signatur Byte für Byte erraten

// Constant-Time: Prüft ALLE 64 Bytes, immer gleiche Laufzeit
volatile uint8_t ergebnis = 0;
for (size_t i = 0; i < 64; i++)
    ergebnis |= berechnet[i] ^ empfangen[i];
return ergebnis == 0;
```

### 4. isdigit-Validierung

```c
// VORHER (V10): Direkt strtoul() auf ungefilterten Input
unsigned long seq = strtoul(seqString, NULL, 10);  // UB bei "abc"!

// JETZT (V12): Erst validieren, dann konvertieren
if (!Security::nurZiffern(seqString)) return 0;     // Müll verwerfen
unsigned long seq = strtoul(seqString, NULL, 10);    // Sicher
```

### 5. DoS Rate-Limiting

```
UDP-Pakete pro Minute:  max 60 (= 1/Sekunde)
                        │
                        └── Überschritten? → Paket verwerfen, Zähler-Reset nach 60s
```

### 6. Traffic Obfuscation

| Im Netzwerk sichtbar | Tatsächliche Bedeutung |
|----------------------|------------------------|
| `NICE_TRY_WIRESHARK_USER` | `ALARM_ON` |
| `ENCRYPTION_IS_YOUR_FRIEND` | `ALARM_OFF` |

### 7. Telnet-Härtung

- Max. 3 Fehlversuche → 5 Minuten Sperre (Verbindung getrennt)
- Easter Egg: Konami-Code (nur zum Spaß, keine Funktion)

---

## UDP-Paketverarbeitung

Kompletter Validierungspfad eines eingehenden Pakets:

```
UDP-Paket empfangen
    │
    ▼
┌─ Rate-Limit prüfen ─────────────────────┐
│  > 60 Pakete/Min? → Verwerfen           │
└──────────────────────────┬───────────────┘
                           │
                           ▼
┌─ char-Array Parsing ────────────────────┐
│  strtok(":") → Befehl, Seq, Signatur    │
│  Genau 3 Felder? (kein Extra-Feld)      │
└──────────────────────────┬───────────────┘
                           │
                           ▼
┌─ isdigit-Validierung ──────────────────┐
│  Sequenz-String nur Ziffern?            │
└──────────────────────────┬──────────────┘
                           │
                           ▼
┌─ HMAC berechnen & vergleichen ─────────┐
│  Payload = "BEFEHL:SEQ"                 │
│  HMAC = SHA256(payload, secret)         │
│  Signatur == 64 Zeichen?                │
│  Constant-Time Vergleich                │
└──────────────────────────┬──────────────┘
                           │
                           ▼
┌─ Replay-Window prüfen ────────────────┐
│  Sequenz schon gesehen?                │
│  Sequenz zu alt (außerhalb Fenster)?   │
└──────────────────────────┬─────────────┘
                           │
                           ▼
┌─ Sequenz in Flash sichern ────────────┐
│  (alle 5 Inkremente)                   │
└──────────────────────────┬─────────────┘
                           │
                           ▼
┌─ Befehl ausführen ───────────────────┐
│  CMD_ALARM_AN  → alarmAktiv = true   │
│  CMD_ALARM_AUS → alarmAktiv = false  │
│  ACK senden: "ACK_SECURE:<SEQ>"      │
└──────────────────────────────────────┘
```

**Sicherheitsprinzip:** Jeder Prüfschritt, der fehlschlägt, verwirft das Paket **stillschweigend** — kein Logging, keine Fehlermeldung. Dadurch erhält ein Angreifer keinerlei Feedback.

---

## Priority-Mode (Alarm)

Der Kern-Designunterschied zu einer naiven Implementierung:

```
┌─────────────────────────────────────────────────────────┐
│                     ZUSTAND_BEREIT                      │
│                                                         │
│  UDP ─ Heartbeat ─ Telnet ─ WLAN-Scan ─ mDNS ─ LED      │
│  Alle Tasks laufen parallel im Loop                     │
└─────────────────────────────────────────────────────────┘

                    │ ALARM_ON
                    ▼

┌─────────────────────────────────────────────────────────┐
│                     ZUSTAND_ALARM                       │
│                                                         │
│  ✅ aktualisiereAlarmHardware()   (200ms LED/Summer)    │
│  ✅ verarbeiteUdpEmpfang()        (kann ALARM_OFF)      │
│  ✅ verarbeiteTaster()             (kann deaktivieren)  │
│  ✅ aktualisiereWlanLed()          (Status-Anzeige)     │
│                                                         │
│  ❌ verarbeiteHeartbeat()          PAUSIERT             │
│  ❌ pruefeTelnetZugang()           PAUSIERT             │
│  ❌ verwalteWlanVerbindung()       PAUSIERT             │
│  ❌ mdnsUpdate()                   PAUSIERT             │
└─────────────────────────────────────────────────────────┘
```

**Warum?** HTTP-Requests (Heartbeat) können bis zu 2 Sekunden blockieren. In dieser Zeit würden die Alarm-LEDs und Summer "einfrieren". Durch das Pausieren aller Netzwerk-Tasks im Alarm-Zustand wird eine **stotterfreie 5-Hz-Signalausgabe** garantiert.

---

## Hardware & Pinbelegung

**Board:** NodeMCU V2 (ESP8266, ESP-12E Modul)

| Pin | GPIO | Funktion | Beschreibung |
|-----|------|----------|--------------|
| `D1` | GPIO5 | LED Rot | Alarm-LED 1 (wechselt mit Gelb im 200ms-Takt) |
| `D2` | GPIO4 | LED Gelb | Alarm-LED 2 (wechselt mit Rot im 200ms-Takt) |
| `D3` | GPIO0 | WLAN-LED | HIGH = verbunden, Blinken (500ms) = nicht verbunden |
| `D5` | GPIO14 | Summer 1 | Akustischer Alarm (wechselt mit Summer 2) |
| `D6` | GPIO12 | Summer 2 | Akustischer Alarm (wechselt mit Summer 1) |
| `D7` | GPIO13 | Taster | Kurzdruck (<1s) = Toggle, Langdruck (>10s) = Reset |

### Alarm-Signalmuster (200ms Takt, 5 Hz)

```
Phase A:  LED_ROT=AN   LED_GELB=AUS   SUMMER_1=AN   SUMMER_2=AN
             ↕ 200ms      ↕ 200ms        ↕ 200ms       ↕ 200ms
Phase B:  LED_ROT=AUS  LED_GELB=AN    SUMMER_1=AN   SUMMER_2=AN
```

### Schaltplan (vereinfacht) 
Hinweis: Reale Widerstandswerte bitte den Schaltplänen unter ```hardware/schematics``` entnehmen.

```
                NodeMCU V2
         ┌──────────────────┐
         │              D1  ├──── [R 220Ω] ──── LED (Rot) ──── GND
         │              D2  ├──── [R 220Ω] ──── LED (Gelb) ──── GND
         │              D3  ├──── [R 220Ω] ──── LED (WLAN) ──── GND
         │              D5  ├──── [R] ──── Piezo-Summer 1 ──── GND
         │              D6  ├──── [R] ──── Piezo-Summer 2 ──── GND
         │              D7  ├──── Taster ──── GND
         │                  │     (INPUT_PULLUP)
         └──────────────────┘
```

### Taster-Funktionen

| Aktion | Dauer | Wirkung |
|--------|-------|---------|
| Kurzdruck | < 1 Sekunde | Alarm ein/ausschalten (Toggle) |
| Langdruck | > 10 Sekunden | Factory Reset (bei Loslassen) |
| Mitteldruck | 1–10 Sekunden | Keine Aktion (Sicherheitspuffer) |

**Safe Reset:** Der Reset wird erst beim **Loslassen** des Tasters ausgelöst — nicht beim Erreichen der 10-Sekunden-Schwelle. Das verhindert versehentliche Resets.

---

## Konfiguration

### WiFiManager Captive Portal

Beim ersten Start öffnet der Empfänger einen Access Point:

```
SSID: "Alarm-Empfaenger-SETUP"
```

Parameter im Portal:

| Parameter | JSON-Key | Beschreibung |
|-----------|----------|--------------|
| Telnet PW | `tpass` | Passwort für Debug-Konsole |
| HMAC Secret | `token` | Shared Secret (muss mit Sender übereinstimmen!) |
| mDNS Name | `name` | Hostname im lokalen Netz (Standard: `alarm-receiver`) |
| API Server IP | `apiip` | Backend-Server für Heartbeat & Logging |
| Backup SSID | `bssid` | Fallback-WLAN |
| Backup PW | `bpass` | Passwort für Fallback-WLAN |
| Haupt SSID | `hssid` | Primäres WLAN |
| Haupt PW | `hpass` | Passwort für Haupt-WLAN |
| AP Passwort | `appw` | Passwort für Setup-AP |

### Persistierte Dateien

```
/config.json     → Alle Konfigurationsparameter
/seq.dat         → Replay-Window-Basis (Sequenznummer)
```

---

## Watchdog-System

```
Hardware-Timer (Ticker)
        │
        │ jede 1s: watchdogZaehler++
        │
        ▼
  watchdogZaehler >= 30?
        │
    Ja: mussNeustarten = true → loop() führt ESP.restart() aus
```

- **Timeout:** 30 Sekunden ohne `watchdogFuettern()`
- **Wird gestoppt** bei: Factory Reset, WLAN-Wechsel

---

## WLAN-Failover

```
Hauptnetz verfügbar?
    │
    Ja ──── Normal betreiben
    │
    Nein ── Alle 20s: Backup-WLAN versuchen
                │
                └── Verbunden mit Backup?
                        │
                        Ja ── Alle 30s: Nach Hauptnetz scannen
                        │        │
                        │        └── 3x stabil & RSSI > -75dBm?
                        │                │
                        │                Ja ── Hauptnetz-Wechsel
                        │
                        Nein ── Weiter versuchen
```

**Wichtig:** Im ALARM-Zustand wird `verwalteWlanVerbindung()` komplett pausiert — keine Scans, keine Reconnects. Hardware-Priorität geht vor.

---

## Heartbeat & Remote-Befehle

Alle 2 Sekunden (im BEREIT-Zustand) sendet der Empfänger Telemetrie per HTTP POST:

```json
{
  "source": "receiver",
  "ip": "192.168.178.42",
  "status_msg": "Bereit",
  "alarm_state": false,
  "rssi": -52,
  "heap": 38240
}
```

### Server-Antwort (optional)

Der Backend-Server kann Befehle zurücksenden:

| JSON-Feld | Wert | Wirkung |
|-----------|------|---------|
| `logging_active` | `true/false` | API-Logging ein-/ausschalten |
| `command` | `"REBOOT"` | Neustart auslösen |
| `command` | `"RESET"` | Factory Reset (Flash löschen) |
| `command` | `"ALARM_ON"` | Alarm über API aktivieren |
| `new_config.mssid` | `"NeuesWLAN"` | Haupt-SSID ändern + Neustart |

### Fehlerfall

Bei HTTP-Fehler wird das Heartbeat-Intervall automatisch auf 60 Sekunden erhöht, um den Server nicht zu überlasten.

---

## Telnet-Debug-Konsole

Verbindung über Port 23:

```bash
telnet <empfaenger-ip>
```

| Eingabe | Wirkung |
|---------|---------|
| `<Passwort>` | Login |
| `up up down down left right left right b a` | Easter Egg |
| *(beliebig falsch, 3x)* | 5 Minuten Sperre |

Nach Login: Alle `sendeProtokoll()`-Meldungen werden live gestreamt.

---

## Abhängigkeiten

| Bibliothek | Version | Verwendung |
|------------|---------|------------|
| ESP8266WiFi | (Core) | WiFi-Stack |
| WiFiManager | ≥2.0 | Captive Portal |
| ArduinoJson | ≥6.0 | JSON für Config & API |
| LittleFS | (Core) | Flash-Dateisystem |
| ESP8266mDNS | (Core) | Namensauflösung |
| ESP8266HTTPClient | (Core) | REST API |
| TelnetStream | ≥1.0 | Remote-Debug |
| Ticker | (Core) | Watchdog-Timer |
| BearSSL | (Core) | HMAC-SHA256 |

---

## Schnellstart

1. **Bibliotheken installieren** (Arduino IDE):
   - WiFiManager (by tzapu/tablatronix)
   - ArduinoJson (by Benoît Blanchon)
   - TelnetStream (by Juraj Andrássy)

2. **Board:** `NodeMCU 1.0 (ESP-12E Module)`, Flash: `4MB (FS:2MB)`

3. **Hardware anschließen:**
   - 2x LED (Rot an D1, Gelb an D2) mit 220Ω Vorwiderständen
   - 1x WLAN-LED an D3
   - 2x Piezo-Summer an D5 und D6
   - 1x Taster an D7 (gegen GND, interner Pullup)

4. **Sketch hochladen** → Mit `Alarm-Empfaenger-SETUP` WLAN verbinden

5. **Im Captive Portal:**
   - Gleiches HMAC Secret wie beim Sender eingeben
   - WLAN-Zugangsdaten und API-Server konfigurieren

6. **Test:**
   - Sender: `ALARM_ON` über Serial Monitor senden
   - Empfänger: LEDs blinken abwechselnd, Summer ertönen
   - Sender: `ALARM_OFF` → Alarm stoppt

7. **Factory Reset:** Taster >10s drücken, dann loslassen
