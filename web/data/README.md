<a id="top"></a>

<div align="center">

[![Deutsch](https://img.shields.io/badge/🇩🇪_Deutsch-24292f?style=for-the-badge)](#deutsch)
[![English](https://img.shields.io/badge/🇬🇧_English-24292f?style=for-the-badge)](#english)

</div>

---

<a id="deutsch"></a>

<a id="de-laufzeitdaten"></a>

# Laufzeitdaten

Dieses Verzeichnis enthaelt den lokalen Zustand einer Installation. Ausser dieser README wird nichts daraus eingecheckt.

| Pfad | Inhalt |
|---|---|
| `settings.json` | Hashes, getrennte Geraete-Token/-HMACs, UDP-Secret und Einstellungen |
| `settings.bootstrap.lock` | Prozess-Lock fuer Bootstrap und Secret-Rotation |
| `.bootstrap-credentials` | Bootstrap-Fallback, nur wenn kein externer Pfad gesetzt ist |
| `status.json` | letzter ESP-/Kamerastatus, nicht der Uno-Zustand |
| `commands.json`, `update_*.json` | offene Node-Kommandos und -Konfigurationen |
| `delivery_state.json`, `delivery_queue.lock` | monotone Zaehler und Zustell-Lock |
| `alarm_monitor.json` | alleinige Dashboard-Quelle fuer Uno, Serial und Aufnahme |
| `telemetry.csv` | Telemetrie-Zeitreihe |
| `log.txt`, `log.txt.1`, `log.txt.2` | groessenbegrenzt rotierter Alarm-Monitor-Log |
| `api.log`, `user_logs.json` | begrenzter API-/Geraete-Log und Administrator-Auditlog |
| `login_attempts.json` | Login-Brute-Force-Tracking |
| `alarm_pin_attempts.json` | getrenntes Alarm-PIN-Lockout-Ledger |
| `ratelimit/` | API-Ratenzaehler pro Quell-IP |
| `recordings/` | lokale Kameraaufnahmen |

Im kanonischen Deployment zeigt `IOT_ALARM_BOOTSTRAP_FILE` vor dem ersten Aufruf auf eine `0600`-Datei ausserhalb des Webroots. Ohne diesen Wert verwendet die Anwendung `.bootstrap-credentials` in diesem von nginx gesperrten Verzeichnis. Nach Passwort-/PIN-Wechsel und ESP-Provisionierung sicher loeschen.

Das Datenverzeichnis ist setgid `2770`, `recordings/` ist `0750`, regulaere Runtime-Dateien sind `0640` und der Bootstrap-Fallback ist `0600`. Der Daemon besitzt den Serial-/Recorderpfad; sein Socket liegt separat unter `/run/iot-alarm-monitor/control.sock` als `0660` fuer die Gruppe `www-data`. Vollstaendige Einrichtung: [Build](../../docs/BUILD.md#de-web-deployment).

Die Daten koennen Secrets, IP-Adressen, Zeitstempel und Aufnahmen enthalten. Backups gleich streng schuetzen, nicht oeffentlich weitergeben und nach der Aufbewahrungsfrist loeschen. Siehe [Security](../../SECURITY.md#deutsch).

<div align="center">

[![Nach oben](https://img.shields.io/badge/⬆_Nach_oben-24292f?style=for-the-badge)](#top)

</div>

---

<a id="english"></a>

<a id="en-runtime-data"></a>

# Runtime Data

This directory contains the local state of an installation. Nothing except this README is committed.

| Path | Contents |
|---|---|
| `settings.json` | hashes, separate device tokens/HMACs, UDP secret, and settings |
| `settings.bootstrap.lock` | process lock for bootstrap and secret rotation |
| `.bootstrap-credentials` | bootstrap fallback only when no external path is set |
| `status.json` | latest ESP/camera status, not Uno state |
| `commands.json`, `update_*.json` | pending node commands and configurations |
| `delivery_state.json`, `delivery_queue.lock` | monotonic counters and delivery lock |
| `alarm_monitor.json` | sole dashboard source for Uno, serial, and recording state |
| `telemetry.csv` | telemetry time series |
| `log.txt`, `log.txt.1`, `log.txt.2` | size-bounded rotating alarm-monitor log |
| `api.log`, `user_logs.json` | bounded API/device log and administrator audit log |
| `login_attempts.json` | login brute-force tracking |
| `alarm_pin_attempts.json` | separate alarm-PIN lockout ledger |
| `ratelimit/` | API rate counters by source IP |
| `recordings/` | local camera recordings |

In the canonical deployment, `IOT_ALARM_BOOTSTRAP_FILE` points to a `0600` file outside the web root before the first request. Without that value, the application uses `.bootstrap-credentials` in this nginx-blocked directory. Delete it securely after password/PIN changes and ESP provisioning.

The data directory is setgid `2770`, `recordings/` is `0750`, regular runtime files are `0640`, and the bootstrap fallback is `0600`. The daemon owns serial and recording resources; its separate socket is `/run/iot-alarm-monitor/control.sock`, mode `0660`, for group `www-data`. See [Build](../../docs/BUILD.md#en-web-deployment) for complete setup.

The data may contain secrets, IP addresses, timestamps, and recordings. Protect backups equally, do not share them publicly, and delete them after their retention period. See [Security](../../SECURITY.md#english).

<div align="center">

[![Back to top](https://img.shields.io/badge/⬆_Back_to_top-24292f?style=for-the-badge)](#top)

</div>
