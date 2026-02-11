# ESP8266 UDP Alarm-System — Sender V13

**Kryptografisch gesicherter Alarm-Sender mit HAL/FSM-Architektur**

> Sendet HMAC-signierte UDP-Befehle bei seriellem Kommando, wartet auf validiertes ACK vom Empfänger und steuert Status-LEDs. Alle Hardware-Zugriffe sind über eine Abstraktionsschicht (HAL) gekapselt, die Programmlogik läuft als endlicher Automat (FSM).

---

## Inhaltsverzeichnis

1. [Architektur-Überblick](#architektur-überblick)
2. [FSM-Zustandsdiagramm](#fsm-zustandsdiagramm)
3. [Schichten-Modell](#schichten-modell)
4. [Sicherheits-Features](#sicherheits-features)
5. [UDP-Transaktionsablauf](#udp-transaktionsablauf)
6. [Hardware & Pinbelegung](#hardware--pinbelegung)
7. [Konfiguration](#konfiguration)
8. [Watchdog-System](#watchdog-system)
9. [WLAN-Failover](#wlan-failover)
10. [Telnet-Debug-Konsole](#telnet-debug-konsole)
11. [Abhängigkeiten](#abhängigkeiten)
12. [Schnellstart](#schnellstart)

---

## Architektur-Überblick

```
┌──────────────────────────────────────────────────────┐
│  FSM (Finite State Machine)                          │
│  fsmUpdate() → Dispatcher für alle Zustände          │
│  INIT → WLAN_VERBINDEN → BEREIT ↔ SENDEN             │
│                              ↓                       │
│                         WERKSRESET                   │
├──────────────────────────────────────────────────────┤
│  Service-Schicht                                     │
│  starteUdpTransaktion() · pruefeUdpAntwort()         │
│  verarbeiteHeartbeat()  · verarbeiteSerielleBefehle() │
│  verwalteWlanVerbindung() · pruefeTelnetZugang()     │
├──────────────────────────────────────────────────────┤
│  Security (plattformunabhängig)                      │
│  berechneHMAC()  · sichererVergleich()               │
│  nurZiffern()    · speichereSequenz/ladeSequenz()    │
├──────────────────────────────────────────────────────┤
│  HAL (Hardware Abstraction Layer)                    │
│  GPIO · WiFi · UDP · Flash · Watchdog · mDNS         │
│  Telnet · Seriell · System                           │
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
                         │ gpioInit, Flash, Config, Sequenz laden
                         ▼
                ┌─────────────────┐
                │ WLAN_VERBINDEN  │
                │ WiFiManager     │
                │ Captive Portal  │
                └────────┬────────┘
                         │ Dienste starten (mDNS, Telnet, UDP)
                         ▼
              ┌──────────────────────┐
              │       BEREIT         │◄─────────────────────┐
              │                      │                      │
              │ • Serielle Befehle   │    ACK empfangen     │
              │ • Heartbeat an API   │    oder Timeout      │
              │ • Telnet-Zugang      │                      │
              │ • WLAN-Failover      │                      │
              │ • mDNS-Update        │                      │
              │ • Reset-Taster       │                      │
              └───┬──────────┬───────┘                      │
    Taster >10s   │          │ ALARM_ON/OFF                 │
                  │          │ über Serial                  │
                  ▼          ▼                              │
         ┌────────────┐   ┌───────────────────┐             │
         │ WERKSRESET  │  │     SENDEN        │─────────────┘
         │             │  │                   │
         │ Flash       │  │ PRIORITY MODE:    │
         │ formatieren │  │ • UDP Retry (10x) │
         │ WiFi löschen│  │ • ACK-Validierung │
         │ Neustart    │  │ • Reset-Taster    │
         └─────────────┘  │                   │
                          │ PAUSIERT:         │
                          │ • Heartbeat       │
                          │ • WLAN-Scan       │
                          │ • Telnet          │
                          └───────────────────┘
```

### Zustandsübergänge im Detail

| Von | Nach | Auslöser |
|-----|------|----------|
| INIT | WLAN_VERBINDEN | Automatisch nach Initialisierung |
| WLAN_VERBINDEN | BEREIT | WLAN verbunden oder Offline-Start |
| BEREIT | SENDEN | Serieller Befehl `ALARM_ON` oder `ALARM_OFF` |
| SENDEN | BEREIT | Validiertes ACK empfangen |
| SENDEN | BEREIT | Timeout nach 10 erfolglosen Versuchen |
| BEREIT/SENDEN | WERKSRESET | Reset-Taster >10 Sekunden gedrückt |
| WERKSRESET | *(Neustart)* | Automatisch nach Flash-Löschung |

---

## Schichten-Modell

### HAL-Namespace — `HAL::`

Alle direkten Hardware-Zugriffe sind hier gekapselt:

| Kategorie | Funktionen | Beschreibung |
|-----------|-----------|--------------|
| **GPIO** | `gpioInit()`, `alarmLed()`, `wlanLed()`, `wlanLedToggle()`, `resetTasterGedrueckt()` | Pin-Steuerung, Taster-Abfrage |
| **Watchdog** | `watchdogStarten()`, `watchdogStoppen()`, `watchdogFuettern()`, `watchdogAusgeloest()` | ISR-basierter Loop-Wächter (30s) |
| **System** | `init()`, `neustart()`, `zeitMs()`, `freierHeap()`, `resetGrund()`, `cpuFreigeben()` | Grundlegende System-Operationen |
| **Flash** | `flashInit()`, `flashFormatieren()` | LittleFS-Dateisystem |
| **WiFi** | `wlanVerbunden()`, `wlanVerbinden()`, `wlanTrennen()`, `wlanSsid()`, `wlanRssi()`, `wlanIp()` | Verbindungsmanagement |
| **WiFi-Scan** | `wlanScanStarten()`, `wlanScanErgebnis()`, `wlanScanSsid()`, `wlanScanRssi()`, `wlanScanLoeschen()` | Asynchroner Netzwerk-Scan |
| **UDP** | `udpStarten()`, `udpPaketVerfuegbar()`, `udpLesen()`, `udpSenden()` | Paket-Kommunikation |
| **mDNS** | `mdnsStarten()`, `mdnsUpdate()`, `zielIpAktualisieren()`, `zielIpGueltig()` | Namensauflösung des Empfängers |
| **Telnet** | `telnetStarten()`, `telnetVerfuegbar()`, `telnetLesen()`, `telnetSchreiben()`, `telnetStoppen()` | Remote-Debug-Konsole |
| **Seriell** | `seriellVerfuegbar()`, `seriellLesen()` | Befehlseingabe |

### Security-Namespace — `Security::`

Plattformunabhängige Sicherheitsfunktionen:

| Funktion | Beschreibung |
|----------|--------------|
| `berechneHMAC()` | HMAC-SHA256 über BearSSL C-API, Ergebnis als 64 Hex-Zeichen in Stack-Buffer |
| `sichererVergleich()` | Constant-Time XOR-Akkumulation mit `volatile` — verhindert Timing-Angriffe |
| `nurZiffern()` | isdigit-Validierung vor `strtoul()` — verhindert undefiniertes Verhalten |
| `speichereSequenz()` | Sequenznummer in `/seq.dat` persistieren (überlebt Reboot) |
| `ladeSequenz()` | Sequenznummer aus Flash wiederherstellen |

---

## Sicherheits-Features

### 1. HMAC-SHA256 Signierung

Jeder UDP-Befehl wird kryptografisch signiert:

```
Payload:    "NICE_TRY_WIRESHARK_USER:42"     (Obfuscated Befehl + Sequenznummer)
HMAC:       SHA256(payload, shared_secret)     (64 Hex-Zeichen)
UDP-Paket:  "NICE_TRY_WIRESHARK_USER:42:a1b2c3..."
```

- **BearSSL C-API** statt Arduino-Wrapper (kein Heap)
- Alle Berechnungen auf **Stack-Buffern** (char-Arrays, keine Arduino Strings)
- Shared Secret wird über WiFiManager konfiguriert

### 2. Traffic Obfuscation

| Klartext-Befehl | Obfuscated Payload |
|------------------|--------------------|
| `ALARM_ON` | `NICE_TRY_WIRESHARK_USER` |
| `ALARM_OFF` | `ENCRYPTION_IS_YOUR_FRIEND` |

Ein Angreifer sieht im Netzwerk-Traffic keine erkennbaren Kommandos.

### 3. Anti-Replay (Sequenznummer-Persistierung)

```
Sequenz: 0 → 1 → 2 → 3 → ...
                ↓
         /seq.dat (LittleFS)
```

- Jede Inkrementierung wird **sofort** in Flash geschrieben
- Nach Reboot: Zähler startet beim letzten gespeicherten Wert
- Empfänger akzeptiert keine bereits gesehenen Nummern

### 4. Constant-Time ACK-Vergleich

```c
// NICHT so (Timing-Seitenkanal):
if (strcmp(empfangen, erwartet) == 0)  // Bricht beim ersten Unterschied ab

// SONDERN so (Constant-Time):
volatile uint8_t ergebnis = 0;
for (size_t i = 0; i < laenge; i++)
    ergebnis |= a[i] ^ b[i];         // Prüft ALLE Bytes
return ergebnis == 0;
```

### 5. Telnet Brute-Force-Schutz

- Max. 3 Fehlversuche
- Danach 5 Minuten Sperre (Verbindung wird getrennt)
- Auto-Logout nach 5 Minuten Inaktivität

---

## UDP-Transaktionsablauf

```
    SENDER                                    EMPFÄNGER
      │                                          │
      │  1. Serial: "ALARM_ON"                   │
      │  2. sequenceNumber++                     │
      │  3. Sequenz → Flash (/seq.dat)           │
      │  4. Payload = "NICE_TRY...:42"           │
      │  5. HMAC = SHA256(payload, secret)       │
      │                                          │
      │──── "NICE_TRY...:42:a1b2c3..."  ────────>│
      │                                          │  6. HMAC prüfen
      │                                          │  7. Replay-Window prüfen
      │                                          │  8. Befehl ausführen
      │<──── "ACK_SECURE:42"  ───────────────────│
      │                                          │
      │  9. Constant-Time ACK-Vergleich          │
      │ 10. LED setzen                           │
      │ 11. → ZUSTAND_BEREIT                     │
      │                                          │

             Bei Timeout (1s ohne ACK):
      │──── Wiederholung 1/10 ──────────────────>│
      │──── Wiederholung 2/10 ──────────────────>│
      │  ...                                     │
      │──── Wiederholung 10/10 ─────────────────>│
      │  FEHLER: Timeout → ZUSTAND_BEREIT        │
```

---

## Hardware & Pinbelegung

**Board:** NodeMCU V2 (ESP8266, ESP-12E Modul)

| Pin | GPIO | Funktion | Beschreibung |
|-----|------|----------|--------------|
| `LED_BUILTIN` | GPIO2 | Alarm-LED | **Invertiert:** LOW = an, HIGH = aus |
| `D5` | GPIO14 | WLAN-LED | HIGH = an (verbunden), Blinken = nicht verbunden |
| `D3` | GPIO0 | Reset-Taster | INPUT_PULLUP, >10s gedrückt = Factory Reset |

### Schaltplan (vereinfacht)
Hinweis: Reale Widerstandswerte bitte den Schaltplänen unter ```hardware/schematics``` entnehmen.

```
                NodeMCU V2
         ┌──────────────────┐
         |              RX  ├──── Spannungsteiler ──── UNO R3
         │              D5  ├──── [R 220Ω] ──── LED (WLAN) ──── GND
         │                  │
         │       BUILTIN    ├──── (Onboard LED, invertiert)
         │                  │
         │              D3  ├──── Taster ──── GND
         │                  │     (INPUT_PULLUP)
         └──────────────────┘
```

---

## Konfiguration

### WiFiManager Captive Portal

Beim ersten Start (oder nach Factory Reset) öffnet der Sender einen Access Point:

```
SSID: "Alarm-Sender-Konfig"
PW:   (konfigurierbar)
```

Über das Captive Portal werden folgende Parameter konfiguriert:

| Parameter | JSON-Key | Beschreibung |
|-----------|----------|--------------|
| API Server IP | `api` | Backend für Heartbeat & Logging |
| HMAC Secret | `token` | Shared Secret für UDP-Signierung (max. 40 Zeichen) |
| Telnet PW | `tpass` | Passwort für Debug-Konsole |
| Empfänger mDNS | `ziel` | Hostname des Empfängers (Standard: `alarm-receiver`) |
| Backup SSID | `bssid` | Fallback-WLAN bei Ausfall des Hauptnetzes |
| Backup PW | `bpass` | Passwort für Backup-WLAN |
| Haupt SSID | `hssid` | Primäres WLAN |
| Haupt PW | `hpass` | Passwort für Haupt-WLAN |
| AP Passwort | `appw` | Passwort für den Konfigurations-Access-Point |

### Persistierung

```
/config.json     → Alle Konfigurationsparameter (JSON)
/seq.dat         → Aktuelle Sequenznummer (Klartext)
```

Beide Dateien liegen im LittleFS-Flash-Dateisystem und überleben Reboots.

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
    Ja: mussNeustarten = true
        │
        ▼
  loop() prüft → ESP.restart()
```

- **Timeout:** 30 Sekunden ohne `watchdogFuettern()`
- **Rate-Limited:** Füttern max. alle 100ms (verhindert versehentliches Dauerfüttern)
- **Wird gestoppt** bei: Factory Reset, WLAN-Wechsel

---

## WLAN-Failover

```
Hauptnetz verfügbar?
    │
    Ja ──── Verbunden mit Hauptnetz ──── Normal betreiben
    │
    Nein ── Alle 20s: Backup-WLAN versuchen
                │
                └── Verbunden mit Backup?
                        │
                        Ja ── Alle 30s: Nach Hauptnetz scannen
                        │        │
                        │        └── 3x hintereinander gefunden & RSSI > -75dBm?
                        │                │
                        │                Ja ── Zurück zum Hauptnetz wechseln
                        │
                        Nein ── Weiter versuchen
```

**Wichtig:** Während einer aktiven UDP-Transaktion (`ZUSTAND_SENDEN`) werden keine WLAN-Scans durchgeführt — der ACK darf nicht verzögert werden.

---

## Telnet-Debug-Konsole

Verbindung über Port 23 (Standard-Telnet):

```bash
telnet <sender-ip>
```

| Eingabe | Wirkung |
|---------|---------|
| `<Passwort>` | Login (max. 3 Versuche, dann 5min Sperre) |
| `logout` | Session beenden |
| `up up down down left right left right b a` | Easter Egg |

Nach dem Login werden alle `sendeProtokoll()`-Nachrichten auch an die Telnet-Session gestreamt.

---

## Abhängigkeiten

| Bibliothek | Version | Verwendung |
|------------|---------|------------|
| ESP8266WiFi | (Core) | WiFi-Stack |
| WiFiManager | ≥2.0 | Captive Portal für Erstkonfiguration |
| ArduinoJson | ≥6.0 | JSON-Serialisierung für Config & API |
| LittleFS | (Core) | Flash-Dateisystem |
| ESP8266mDNS | (Core) | Lokale Namensauflösung |
| ESP8266HTTPClient | (Core) | REST API Kommunikation |
| TelnetStream | ≥1.0 | Remote-Debug-Konsole |
| Ticker | (Core) | Hardware-Timer für Watchdog |
| BearSSL | (Core) | HMAC-SHA256 Kryptografie |

---

## Schnellstart

1. **Bibliotheken installieren** (Arduino IDE Library Manager):
   - WiFiManager (by tzapu/tablatronix)
   - ArduinoJson (by Benoît Blanchon)
   - TelnetStream (by Juraj Andrássy)

2. **Board auswählen:**
   - Board: `NodeMCU 1.0 (ESP-12E Module)`
   - Flash Size: `4MB (FS:2MB OTA:~1019KB)`
   - Upload Speed: `115200`

3. **Sketch hochladen** und Serial Monitor öffnen (9600 Baud)

4. **Ersteinrichtung:**
   - Mit dem WLAN `Alarm-Sender-Konfig` verbinden
   - Im Captive Portal HMAC Secret und Netzwerk-Daten eingeben
   - Speichern → Sender verbindet sich automatisch

5. **Befehle senden** (Serial Monitor, NL):
   ```
   ALARM_ON     → Alarm aktivieren
   ALARM_OFF    → Alarm deaktivieren
   ```

6. **Factory Reset:** Reset-Taster >10 Sekunden gedrückt halten, dann loslassen
