# Data Directory

Dieses Verzeichnis wird automatisch beim ersten Start des Dashboards erstellt und enthält alle Laufzeitdaten.

## Wichtig

**Die Dateien in diesem Ordner werden NICHT auf GitHub committet!**

Sie enthalten sensible Informationen wie:
- IP-Adressen
- Login-Daten
- Telemetrie-Informationen
- Session-Daten

## Dateistruktur

### Konfiguration

**settings.json** - Dashboard-Konfiguration

```json
{
  "password": "CHANGE_ME",
  "refresh_rate": 2000,
  "site_title": "IoT Control Center",
  "timeout_active": false,
  "timeout_minutes": 5
}
```

### Geräte-Status

**status.json** - Aktueller Status aller verbundenen ESP-Nodes

Enthält:
- Letzter Kontakt-Zeitstempel
- IP-Adressen
- Online/Offline Status
- RSSI, Heap, Uptime

### Telemetrie und Logs

**telemetry.csv** - Zeitreihen-Daten für Charts (RSSI, Heap)

```
timestamp,source,rssi,heap
1707408234,sender,-65,45000
```

**log.txt** - System-Kommunikationslogs

Enthält:
- Nachrichten von ESP-Nodes
- Verbindungsstatus
- Fehler und Warnungen

**user_logs.json** - Audit-Log (Admin-Aktivitäten)

Protokolliert:
- Login/Logout Events
- Konfigurationsänderungen
- Gesendete Befehle
- IP-Adressen und User-Agents

### Steuerung

**commands.json** - Pending Commands für ESP-Nodes
- Wird automatisch geleert nach Ausführung

**update_[node].json** - Remote-Konfigurationen für ESPs
- Temporäre Dateien für OTA-Config-Updates

## Sicherheit

Beim ersten Start wird automatisch ein Platzhalter-Passwort (`CHANGE_ME`) gesetzt.

**WICHTIG: Ändere das Passwort sofort nach dem ersten Login!**

Gehe zu: Settings → Change Admin Password

## Wartung

### Log-Rotation

- `telemetry.csv` wird automatisch archiviert bei über 1MB
- `user_logs.json` behält nur die letzten 100 Einträge
- `log.txt` kann über das Dashboard geleert werden

### Backup

Wichtige Daten können über das Dashboard exportiert werden:
- Diagnose → Export Telemetry (CSV)
- Audit → Export Logs

## Automatische Initialisierung

Wenn das Dashboard zum ersten Mal gestartet wird, werden alle notwendigen Dateien automatisch mit Standardwerten erstellt:

```php
// In index.php & api.php
if (!is_dir('data')) mkdir('data', 0777, true);
```

## Berechtigungen

Stelle sicher, dass der Webserver Schreibrechte hat:

```bash
sudo chown -R www-data:www-data data/
sudo chmod 775 data/
```

## Verzeichnisinhalt (Beispiel)

```
data/
├── README.md              (Diese Datei)
├── settings.json          (Dashboard-Config)
├── status.json            (Gerätestatus)
├── user_logs.json         (Admin-Logs)
├── commands.json          (Pending Commands)
├── telemetry.csv          (Telemetrie-Historie)
├── log.txt                (System-Logs)
└── update_*.json          (Temporäre Config-Updates)
```

## Datenschutz

Alle Dateien in diesem Verzeichnis enthalten potenziell personenbezogene Daten (IP-Adressen, Gerätenamen, Zeitstempel).

Die `.gitignore` Datei im Repository stellt sicher, dass diese Daten nicht versehentlich veröffentlicht werden.
