<a id="top"></a>

<div align="center">

[![Deutsch](https://img.shields.io/badge/🇩🇪_Deutsch-24292f?style=for-the-badge)](#deutsch)
[![English](https://img.shields.io/badge/🇬🇧_English-24292f?style=for-the-badge)](#english)

</div>

---

<a id="deutsch"></a>
<a id="de-tests"></a>

# Tests

Automatisierte Nachweise und reale Hardwaretests sind getrennt. Ein gruener Lauf ist kein Safety-
oder Security-Zertifikat.

<a id="de-automatisiert"></a>

## Automatisierte Pruefungen

`Verify` nutzt Ubuntu 24.04, Python 3.13, Node.js 24 und die gepinnte
[Arduino-Toolchain](BUILD.md#de-versionen). Aus dem Repository-Root:

```bash
bash tests/ci/compile_firmware.sh
bash tests/ci/run_host_firmware_tests.sh
python3 -m unittest discover -v -s tests/runtime_deploy -p 'test_*.py'
bash tests/ci/check_php.sh
find . -path ./.git -prune -o -type f -name '*.js' -print0 \
  | xargs -0 -n1 node --check
python3 tests/ci/check_markdown_links.py
yamllint --strict -c tests/ci/yamllint-actions.yml .github/workflows
git diff --check
```

Der Workflow `Secret Scan` prueft die vollstaendige Historie mit Gitleaks und `.gitleaks.toml`.

<a id="de-bestaetigt"></a>

## Bestaetigter Stand

| Bereich | Ergebnis und Abdeckung |
|---|---|
| Firmware | `6/6`: Sender Release `39 %` Flash/`43 %` RAM, Debug `39 %`/`44 %`; Empfaenger Release/Debug `39 %`/`43 %`; Uno Default/Beispiel `23 %`/`12 %`; ESP-IRAM `93 %` |
| Host-C++ | `2/2`: Uno-FSM fail-secure; Empfaenger verwirft Sequenz `99` nach `100` |
| Runtime Ubuntu | `17/17`: Serial, STATUS, IPC/TTL/Replay, Arm/Disarm, Recorder, aktiver Loeschschutz, Logrotation |
| Runtime Windows | `16` bestanden, `1` `AF_UNIX`-Test uebersprungen |
| PHP `8.4` | `21` Dateien lintfrei; `6` Suiten: Audit (`40`), Bootstrap, Brute Force (`80`), HTTP-E2E, Protokoll, paralleles Status-RMW (`40`) |
| JavaScript | `2/2` Dateien mit `node --check` |
| Markdown | `9/9` Dokumente: Pfade, Grossschreibung, DE/EN-Anker und GIF-Embeds |
| Fail2ban | IPv4, IPv6 und Tier 2 matchen; Injection nicht; kein installierter Jail-Livetest |

<a id="de-deployment"></a>

## Deployment pruefen

```bash
sudo nginx -t
sudo systemctl daemon-reload
sudo systemctl restart iot-alarm-monitor.service
sudo systemctl status nginx php8.4-fpm iot-alarm-monitor.service
sudo systemctl show iot-alarm-monitor.service \
  -p User -p Group -p SupplementaryGroups
sudo stat -c '%U %G %a %n' /run/iot-alarm-monitor/control.sock
id www-data
sudo -l -U www-data
sudo fail2ban-client -t
sudo fail2ban-client status iot-login
```

Erwartet: Dienst `iot-alarm-monitor:www-data` mit `SupplementaryGroups=dialout`; Socket
`iot-alarm-monitor:www-data`, Modus `660`; fuer `www-data` weder `dialout` noch `sudo`. Im Journal
muessen Uno-`STATUS`-Handshake und periodische Antworten erscheinen.

<a id="de-hardware"></a>

## Manueller Hardwaretest

1. Leere UID-Whitelist: jede Karte wird abgelehnt.
2. Private Test-UID: lokal und per Dashboard scharf/unscharf; Erfolg nur mit bestaetigtem Snapshot.
3. Jeden Reed-Kreis einzeln oeffnen; beide muessen scharf Alarm ausloesen.
4. Uno in `UNSCHARF`, `SCHARF`, `ALARM` neu starten; EEPROM wiederherstellen, korrupte nichtleere
   Testdaten muessen fail-secure Alarm ausloesen.
5. USB-Serial trennen/verbinden; aktive Alarmaufnahme bleibt bis zum validierten Reconnect-Snapshot.
6. Sender/Empfaenger: Alarm, signiertes ACK, Duplikat, Neustart und offene Dashboard-Zustellung.
7. Auf Test-Flash provisionierten LittleFS-Security-Record entfernen/beschaedigen: Sender sperrt
   Remote-Pfade, Empfaenger aktiviert zusaetzlich physische Alarmausgaenge.
8. Aufnahmegrenzen, voller Datentraeger, Kameraausfall und Loeschschutz der aktiven Datei pruefen.

<a id="de-nachweisgrenze"></a>

## Nachweisgrenze

Serial und ffmpeg sind simuliert. Nicht abgedeckt: Langzeitbetrieb, Versorgungsmessung,
Funkstoerung, SD-Karten-Stromausfall und unabhaengiger Pentest. Fotos und fuenf Demo-GIFs zeigen den
Prototyp, sind aber kein formaler Nachweis.

<div align="center">

[![Nach oben](https://img.shields.io/badge/⬆_Nach_oben-24292f?style=for-the-badge)](#top)

</div>

---

<a id="english"></a>
<a id="en-tests"></a>

# Tests

Automated evidence and real hardware tests remain separate. A green run is not a safety or security
certificate.

<a id="en-automated"></a>

## Automated Checks

`Verify` uses Ubuntu 24.04, Python 3.13, Node.js 24, and the pinned
[Arduino toolchain](BUILD.md#en-versions). From the repository root:

```bash
bash tests/ci/compile_firmware.sh
bash tests/ci/run_host_firmware_tests.sh
python3 -m unittest discover -v -s tests/runtime_deploy -p 'test_*.py'
bash tests/ci/check_php.sh
find . -path ./.git -prune -o -type f -name '*.js' -print0 \
  | xargs -0 -n1 node --check
python3 tests/ci/check_markdown_links.py
yamllint --strict -c tests/ci/yamllint-actions.yml .github/workflows
git diff --check
```

The `Secret Scan` workflow scans the complete history with Gitleaks and `.gitleaks.toml`.

<a id="en-confirmed"></a>

## Confirmed Status

| Area | Result and coverage |
|---|---|
| Firmware | `6/6`: sender release `39%` flash/`43%` RAM, debug `39%`/`44%`; receiver release/debug `39%`/`43%`; Uno default/example `23%`/`12%`; ESP IRAM `93%` |
| Host C++ | `2/2`: Uno FSM fail-secure; receiver rejects sequence `99` after `100` |
| Runtime Ubuntu | `17/17`: serial, STATUS, IPC/TTL/replay, arm/disarm, recorder, active-file guard, log rotation |
| Runtime Windows | `16` passed, `1` `AF_UNIX` test skipped |
| PHP `8.4` | `21` files lint clean; `6` suites: audit (`40`), bootstrap, brute force (`80`), HTTP E2E, protocol, concurrent status RMW (`40`) |
| JavaScript | `2/2` files passed `node --check` |
| Markdown | `9/9` documents: paths, exact case, DE/EN anchors, and GIF embeds |
| Fail2ban | IPv4, IPv6, and tier 2 match; injection does not; no installed-jail live test |

<a id="en-deployment"></a>

## Verify the Deployment

```bash
sudo nginx -t
sudo systemctl daemon-reload
sudo systemctl restart iot-alarm-monitor.service
sudo systemctl status nginx php8.4-fpm iot-alarm-monitor.service
sudo systemctl show iot-alarm-monitor.service \
  -p User -p Group -p SupplementaryGroups
sudo stat -c '%U %G %a %n' /run/iot-alarm-monitor/control.sock
id www-data
sudo -l -U www-data
sudo fail2ban-client -t
sudo fail2ban-client status iot-login
```

Expected: service `iot-alarm-monitor:www-data` with `SupplementaryGroups=dialout`; socket
`iot-alarm-monitor:www-data`, mode `660`; neither `dialout` nor `sudo` for `www-data`. The journal must
show a Uno `STATUS` handshake and periodic responses.

<a id="en-hardware"></a>

## Manual Hardware Test

1. Empty UID whitelist: every card is rejected.
2. Private test UID: arm/disarm locally and through dashboard; success requires a confirmed snapshot.
3. Open each reed circuit separately; either must trigger an armed system.
4. Restart Uno in `UNSCHARF`, `SCHARF`, `ALARM`; restore EEPROM, while corrupt non-empty test state
   must fail secure into alarm.
5. Disconnect/reconnect USB serial; active alarm recording remains through validated reconnect.
6. Sender/receiver: alarm, signed ACK, duplicate, restart, and pending dashboard delivery.
7. On test flash remove/corrupt a provisioned LittleFS security record: sender disables remote paths;
   receiver also activates physical alarm outputs.
8. Test recording limits, full disk, camera failure, and active-file deletion protection.

<a id="en-evidence-boundary"></a>

## Evidence Boundary

Serial and ffmpeg are simulated. Not covered: endurance, supply measurement, radio interference,
SD-card power loss, or independent penetration testing. Photos and five demo GIFs show the prototype
but are not formal evidence.

<div align="center">

[![Back to top](https://img.shields.io/badge/⬆_Back_to_top-24292f?style=for-the-badge)](#top)

</div>
