<a id="top"></a>

<div align="center">

[![Deutsch](https://img.shields.io/badge/🇩🇪_Deutsch-24292f?style=for-the-badge)](#deutsch)
[![English](https://img.shields.io/badge/🇬🇧_English-24292f?style=for-the-badge)](#english)

</div>

---

<a id="deutsch"></a>

<a id="de-security"></a>

# Security

Dieses Projekt ist ein nicht zertifizierter Lernprototyp, kein Sicherheitsprodukt. Reviews und lokale Tests reduzieren konkrete Risiken, garantieren aber weder Vertraulichkeit noch Verfuegbarkeit oder Alarmwirkung. Ein unabhaengiger formaler Penetrationstest fand nicht statt.

<a id="de-trust-boundary"></a>

## Trust Boundary

```text
Internet, WAN, Gast-/Campus-WLAN
  -> kein Zugriff auf Dashboard, ESP-API, UDP, mDNS oder Setup-AP

Browser-/Admin-Netz
  -> HTTPS zu nginx :443

Isoliertes IoT-WLAN
  -> ESP-HTTP-API auf eigenem HTTP-Port
  -> UDP 4211 -> 4210 zwischen Sender und Empfaenger

Raspberry Pi lokal
  -> PHP-FPM, Laufzeitdaten, Alarm-Monitor und USB-Serial
```

Die ESP-API besitzt firmwarebedingt kein TLS. Sie gehoert in ein isoliertes, vertrauenswuerdiges VLAN ohne Portweiterleitung und ohne Zugriff aus WAN, Gast-, Campus- oder allgemeinen Clientnetzen.

<a id="de-secrets"></a>

## Secrets und Ersteinrichtung

- Eine frische Webinstallation erzeugt ein zufaelliges Admin-Passwort, eine sechsstellige Alarm-PIN, je Geraet einen 32-stelligen API-Token und 40-stelligen Delivery-HMAC sowie ein getrenntes 40-stelliges UDP-HMAC-Secret.
- Im kanonischen Deployment setzt `IOT_ALARM_BOOTSTRAP_FILE` eine `0600`-Datei ausserhalb des Webroots. Ohne Override faellt der Code auf das von nginx gesperrte Dotfile `web/data/.bootstrap-credentials` zurueck; die externe Variante bleibt Pflicht fuer den Produktivaufbau.
- Bootstrap-Datei erst nach Aenderung von Passwort/PIN und vollstaendiger Provisionierung sicher loeschen.
- Jeder ESP erhaelt sein eigenes API-/Delivery-Paar; Sender und Empfaenger teilen nur das UDP-Secret. Setup-AP- und Telnet-Passwoerter entstehen zufaellig und werden nur seriell ausgegeben.
- Echte RFID-UIDs liegen nur in ignorierter `uid_whitelist.local.h`; ohne sie bleibt die Whitelist fail-closed.
- `web/data/`, `*.local.*`, Zugangsdaten, Token und Bootstrap-Dateien gehoeren nie in Git, Screenshots oder andere oeffentlich zugaengliche Inhalte.
- Bei Migration darf ein geeignetes Alt-HMAC nur UDP-Secret werden; globale Alt-Token/-HMACs und ungueltige Zustellrecords werden entfernt beziehungsweise fail-closed abgewiesen.

<a id="de-kontrollen"></a>

## Implementierte Kontrollen

### Funkpfad

- UDP-Kommandos tragen HMAC-SHA256 und eine strikt steigende, persistente Sequenz.
- Der Empfaenger prueft Format, bekannten Befehl, Quellport, HMAC und High-Watermark. Nur ein frisches authentifiziertes Paket bindet die Peer-IP, verbraucht Rate-Limit und darf wirken.
- Das exakte Duplikat der hoechsten Sequenz kann erneut signiert bestaetigt werden, bindet aber keinen neuen Peer und wirkt nicht erneut; jede niedrigere Sequenz wird verworfen.
- Sequenz und neuer Alarmzustand werden vor der Hardwarewirkung in LittleFS gespeichert. ACKs sind HMAC-signiert und sequenzgebunden; auch der Sender bindet den Peer erst nach einem gueltigen aktuellen ACK.
- Kurze/leere Secrets sperren den Pfad. Die lesbaren Wire-Namen sind nur Obfuscation, keine Verschluesselung.

### Server-zu-ESP-Zustellung

- API-Zulassung und Delivery-Kryptografie nutzen pro Quelle getrennte Secrets; das UDP-Secret ist davon unabhaengig.
- Kommando- und Konfigurationsqueues bleiben bis zum exakt passenden Geraete-ACK erhalten. Pro Quelle sind zusammen maximal 16 Records offen; Alarmzustand und Konfiguration koaleszieren, `REBOOT` bleibt FIFO.
- `ALARMv2` bindet zufaellige ID, monotone Sequenz, Typ, Nonce und Payload per HMAC; ACKs nutzen getrenntes `ALARMv2ACK`-Material.
- Vor Wirkung schreibt der ESP ein LittleFS-Apply-Journal. Nach Neustart wird eine offene Operation wiederhergestellt oder ohne doppelte Wirkung erneut bestaetigt.
- Fehlender oder korrupter Sicherheitszustand sperrt auf bereits provisionierten ESPs Remote-Pfade; der Empfaenger aktiviert zusaetzlich fail-secure seine Alarmaktoren.
- Konfiguration wird feldweise erlaubt und mit einem HMAC-abgeleiteten Schluesselstrom maskiert. Diese projektspezifische Konstruktion ist weder TLS noch standardisiertes AEAD.

### Dashboard und Raspberry Pi

- nginx liefert den Browser nur per HTTPS aus, bindet die ESP-API an IP und CIDR des IoT-Subnetzes und blockiert Laufzeit-, Include-, View-, Deploy- und Dotfile-Pfade.
- Browsermutationen verlangen Session und CSRF, kritische Aktionen zusaetzlich die Alarm-PIN im selben Request. Passwoerter und PINs liegen nur gehasht vor.
- Auditansicht und CSV-Export benoetigen eine serverseitige, fuenf Minuten gueltige und an den aktuellen PIN-Hash gebundene Freigabe.
- `system_reset` verlangt PIN und erfolgreiche Daemon-IPC. Bei Erfolg leert es Monitor-Log, Delivery-Queues, `status.json` und `api.log`; `delivery_state.json`, Einstellungen/Credentials, Auditlog, Telemetrie, Aufnahmen und Rate-Limits bleiben erhalten.
- Progressive Login-/PIN-Sperren, API-Rate-Limits und Fail2ban bilden getrennte Schutzschichten; oeffentliche API-Antworten enthalten keine Hashes, Token oder HMAC-Secrets.
- Laufzeitdateien verwenden Locks oder atomaren Replace. CSV-Export neutralisiert fuehrende Tabellenformeln.
- `iot-alarm-monitor` besitzt exklusiv Serial, Recorder und deren Loeschpfade. Nur sein gehaerteter systemd-Prozess erhaelt `dialout`; `www-data` weder `dialout` noch `sudo`.
- PHP sendet kurzlebige JSON-v1-Anfragen an `/run/iot-alarm-monitor/control.sock`. Serial gilt erst nach strengem `STATUS`-Handshake als Uno; Befehle brauchen passende Statusbestaetigung.
- Telnet ist im normalen ESP-Build mit `ALARM_ENABLE_TELNET=0` deaktiviert.

<a id="de-restgrenzen"></a>

## Verbleibende Risiken

| Grenze | Auswirkung und Minderung |
|---|---|
| ESP-API ueber HTTP | Token, Telemetrie und Namen sind im IoT-WLAN mitlesbar; nur ein isoliertes IoT-Netz verwenden. |
| Geteiltes UDP-Secret | Auslesen eines ESP kompromittiert den Funkpfad beider Knoten; beide neu provisionieren. |
| Eigene Config-Kryptografie | Keine formale AEAD-Analyse; nicht als TLS-Ersatz bewerten. |
| Debug-Telnet | Klartext; nur explizit bauen und auf einen Wartungsclient begrenzen. |
| ESP-IRAM | Bestaetigte Builds belegen `93 %`; nach jeder Aenderung neu bauen. |
| Browser-CDNs/TLS-Vertrauen | Externe Supply Chain und selbstsignierte Zertifikate; Ressourcen lokal hosten und vertrauenswuerdige CA nutzen. |
| Funk, Strom, physischer Zugriff | Jamming, Brownouts, Ausfall und Schluesselzugriff bleiben Hardwaregrenzen. |
| Kamera und Logs | Zugriff, verschluesselte Backups und externe Aufbewahrungs-/Loeschregeln sind erforderlich. |

<a id="de-deployment-checkliste"></a>

## Deployment-Checkliste

1. IoT-VLAN ohne WAN-Portweiterleitung und mit enger CIDR-Freigabe erstellen.
2. nginx-Platzhalter ersetzen, `nginx -t` ausfuehren und HTTPS-Vertrauen einrichten.
3. Externen Bootstrap-Pfad setzen, Zufallswerte uebernehmen, Passwort/PIN ersetzen und Datei sicher loeschen.
4. Code `root:root` read-only, Laufzeitdaten restriktiv und Alarm-Monitor nach [Build](docs/BUILD.md#de-web-deployment) installieren; PHP kein `dialout`/`sudo` geben.
5. Fail2ban mit ausschliesslich IPv4-/IPv6-Loopback in `ignoreip` testen.
6. Betriebssystem und Bibliotheken aktuell halten und [Tests](docs/TESTING.md#deutsch) nach Aenderungen wiederholen.

<a id="de-rotation"></a>

## Bei Verdacht auf Kompromittierung

Netz isolieren, Admin-Passwort und Alarm-PIN ersetzen. Fuer den betroffenen Knoten API-Token und Delivery-HMAC rotieren; bei kompromittiertem Sender oder Empfaenger zusaetzlich UDP-Secret tauschen und beide ESPs neu provisionieren. Queues, Apply-Journal und Backups pruefen.

<div align="center">

[![Nach oben](https://img.shields.io/badge/⬆_Nach_oben-24292f?style=for-the-badge)](#top)

</div>

---

<a id="english"></a>

<a id="en-security"></a>

# Security

This project is an uncertified learning prototype, not a security product. Reviews and local tests reduce specific risks but do not guarantee confidentiality, availability, or alarm operation. No independent formal penetration test took place.

<a id="en-trust-boundary"></a>

## Trust Boundary

```text
Internet, WAN, guest/campus WiFi
  -> no access to dashboard, ESP API, UDP, mDNS, or setup AP

Browser/admin network
  -> HTTPS to nginx :443

Isolated IoT WiFi
  -> ESP HTTP API on a dedicated HTTP port
  -> UDP 4211 -> 4210 between sender and receiver

Raspberry Pi locally
  -> PHP-FPM, runtime data, alarm monitor, and USB serial
```

The ESP API cannot use TLS with the current firmware. It belongs in an isolated trusted VLAN without port forwarding or access from WAN, guest, campus, or general client networks.

<a id="en-secrets"></a>

## Secrets and First-Time Setup

- A fresh web installation creates a random admin password, six-digit alarm PIN, separate 32-character API token and 40-character delivery HMAC per device, plus a separate 40-character UDP HMAC secret.
- In the canonical deployment, `IOT_ALARM_BOOTSTRAP_FILE` selects a `0600` file outside the web root. Without an override, code falls back to nginx-blocked `web/data/.bootstrap-credentials`; the external location remains mandatory for production deployment.
- Delete the bootstrap file securely only after changing password/PIN and completing provisioning.
- Each ESP receives its own API/delivery pair; sender and receiver share only the UDP secret. Setup-AP and Telnet passwords are random and printed only over serial.
- Real RFID UIDs live only in ignored `uid_whitelist.local.h`; the whitelist fails closed without it.
- `web/data/`, `*.local.*`, credentials, tokens, and bootstrap files never belong in Git, screenshots, or other publicly accessible material.
- During migration, a suitable legacy HMAC may become only the UDP secret; global legacy tokens/HMACs and invalid delivery records are removed or rejected fail-closed.

<a id="en-controls"></a>

## Implemented Controls

### Radio path

- UDP commands carry HMAC-SHA256 and a strictly increasing persistent sequence.
- The receiver checks format, known command, source port, HMAC, and high-water mark. Only a fresh authenticated packet binds the peer IP, consumes rate limit, and may take effect.
- The exact duplicate of the highest sequence may receive another signed ACK but cannot bind a new peer or act again; every lower sequence is rejected.
- Sequence and new alarm state are stored in LittleFS before hardware effect. ACKs are HMAC-signed and sequence-bound; the sender also binds its peer only after a valid current ACK.
- Short or empty secrets disable the path. Readable wire names are obfuscation, not encryption.

### Server-to-ESP delivery

- API admission and delivery cryptography use source-specific secrets; the UDP secret is independent.
- Command and configuration queues remain until the exact device ACK arrives. At most 16 records are pending per source; alarm state/configuration coalesce while `REBOOT` remains FIFO.
- `ALARMv2` binds random ID, monotonic sequence, type, nonce, and payload with HMAC; ACKs use separate `ALARMv2ACK` material.
- Before any effect, the ESP writes a LittleFS apply journal. After restart, an open operation is restored or acknowledged again without duplicate effect.
- Missing or corrupt security state disables remote paths on an already provisioned ESP; the receiver additionally fail-secure drives its alarm outputs.
- Configuration is field-allowlisted and masked with an HMAC-derived keystream. This project-specific construction is neither TLS nor standardized AEAD.

### Dashboard and Raspberry Pi

- nginx serves the browser only over HTTPS, binds the ESP API to the IoT subnet address/CIDR, and blocks runtime, include, view, deploy, and dotfile paths.
- Browser mutations require session and CSRF; critical actions also carry the alarm PIN in the same request. Passwords and PINs are stored only as hashes.
- Audit view and CSV export require a server-side grant lasting five minutes and bound to the current PIN hash.
- `system_reset` requires the PIN and successful daemon IPC. On success it clears the monitor log, delivery queues, `status.json`, and `api.log`; `delivery_state.json`, settings/credentials, audit log, telemetry, recordings, and rate limits remain.
- Progressive login/PIN lockouts, API rate limits, and Fail2ban are separate layers; public API responses expose no hashes, tokens, or HMAC secrets.
- Runtime files use locks or atomic replacement. CSV export neutralizes leading spreadsheet formulas.
- `iot-alarm-monitor` exclusively owns serial, the recorder, and their deletion paths. Only its hardened systemd process receives `dialout`; `www-data` receives neither `dialout` nor `sudo`.
- PHP sends short-lived JSON v1 requests to `/run/iot-alarm-monitor/control.sock`. Serial counts as the Uno only after a strict `STATUS` handshake; commands require matching status confirmation.
- Telnet is disabled in normal ESP builds with `ALARM_ENABLE_TELNET=0`.

<a id="en-residual-risks"></a>

## Residual Risks

| Boundary | Impact and mitigation |
|---|---|
| ESP API over HTTP | Tokens, telemetry, and names are visible on the IoT WiFi; use only an isolated IoT network. |
| Shared UDP secret | Extracting either ESP compromises both nodes' radio path; provision both again. |
| Custom config cryptography | No formal AEAD analysis; do not treat it as a TLS replacement. |
| Debug Telnet | Plaintext; build explicitly and restrict to one maintenance client. |
| ESP IRAM | Confirmed builds use `93%`; rebuild after every change. |
| Browser CDNs/TLS trust | External supply chain and self-signed certificates; host locally and use a trusted CA. |
| Radio, power, physical access | Jamming, brownouts, outages, and key extraction remain hardware boundaries. |
| Camera and logs | Access control, encrypted backups, and external retention/deletion rules are required. |

<a id="en-deployment-checklist"></a>

## Deployment Checklist

1. Create an IoT VLAN without WAN forwarding and with a narrow CIDR allowlist.
2. Replace nginx placeholders, run `nginx -t`, and establish HTTPS trust.
3. Set an external bootstrap path, apply random values, replace password/PIN, and securely delete the file.
4. Deploy code read-only as `root:root`, restrict runtime data, and install the alarm monitor from [Build](docs/BUILD.md#en-web-deployment); give PHP no `dialout` or `sudo`.
5. Test Fail2ban with only IPv4/IPv6 loopback in `ignoreip`.
6. Keep the OS and libraries current and repeat [Tests](docs/TESTING.md#english) after changes.

<a id="en-rotation"></a>

## Suspected Compromise

Isolate the network and replace the admin password and alarm PIN. Rotate the affected node's API token and delivery HMAC; for a compromised sender or receiver, also replace the UDP secret and provision both ESPs again. Inspect queues, apply journal, and backups.

<div align="center">

[![Back to top](https://img.shields.io/badge/⬆_Back_to_top-24292f?style=for-the-badge)](#top)

</div>
