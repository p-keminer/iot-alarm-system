<a id="top"></a>

<div align="center">

[![Deutsch](https://img.shields.io/badge/🇩🇪_Deutsch-24292f?style=for-the-badge)](#deutsch)
[![English](https://img.shields.io/badge/🇬🇧_English-24292f?style=for-the-badge)](#english)

</div>

---

<a id="deutsch"></a>
<a id="de-build"></a>

# Build und Installation

Diese Anleitung gilt fuer die drei aktiven Firmwareziele und das kanonische
Raspberry-Pi-Deployment. Historische Sketche sind kein Build-Input.

<a id="de-versionen"></a>

## Verifizierte Versionen

Arduino CLI `1.5.1`; ESP8266-Core `3.1.2`; AVR Boards `1.8.8`;
ArduinoJson `7.4.3`; WiFiManager `2.0.17`; TelnetStream `1.3.0`;
NetApiHelpers `1.0.3`; MFRC522 `1.4.12`; PHP `8.4.25`; Node.js `24.14`.

Neuere Versionen sind nicht Teil des Nachweises. Bekannte ArduinoJson-/
ESP8266-Deprecation-Warnungen sind keine Build-Fehler.

<a id="de-toolchain"></a>

## Arduino-Toolchain

```bash
arduino-cli config init
arduino-cli core update-index
arduino-cli core install arduino:avr@1.8.8
arduino-cli core install esp8266:esp8266@3.1.2 --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli lib install "ArduinoJson@7.4.3"
arduino-cli lib install "WiFiManager@2.0.17"
arduino-cli lib install "TelnetStream@1.3.0"
arduino-cli lib install "NetApiHelpers@1.0.3"
arduino-cli lib install "MFRC522@1.4.12"
```

<a id="de-firmware-builds"></a>

## Firmware bauen

```bash
arduino-cli compile --fqbn arduino:avr:uno firmware/elegoo_uno_r3/sketchR3/alarm_system
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 firmware/esp8266/sender
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 firmware/esp8266/receiver
```

Zum Upload `--port <PORT> --upload` anhaengen.

<a id="de-rfid"></a>

## Lokale RFID-UIDs

Der Uno-Default baut fail-closed und akzeptiert keine Karte. Fuer reale Hardware
`uid_whitelist.local.h.example` nach `uid_whitelist.local.h` kopieren und UID-Bytes
samt Laenge lokal einsetzen. Die ignorierte `*.local.h` nie committen. Die
unveraenderte Vorlage kompiliert absichtlich nicht; unscharf mit eigener Karte testen.

<a id="de-esp-provisionierung"></a>

## ESP-Ersteinrichtung

Ohne Haupt-SSID startet `Alarm-Sender-Konfig` bzw. `Alarm-Empfaenger-Konfig`;
zufaellige WPA2-Setup-AP-/Telnet-Passwoerter erscheinen nur seriell. Im isolierten
Netz setzen: dasselbe UDP-HMAC-Secret (`>=32` Zeichen) auf beiden ESPs, je Geraet
einen eigenen 32-stelligen API-Token und Delivery-HMAC (`>=32`), ausserdem API-IP,
Haupt-/Backup-WLAN und Debug-Telnet-Passwort.

Sender- und Empfaengerwerte nie vertauschen. Beide gemeinsam auf `V15`
aktualisieren; signierte UDP-ACKs und `ALARMv2` sind mit Alt-Firmware nicht
voll kompatibel. Bei Migration bleibt ein geeignetes Alt-HMAC nur als
UDP-Secret; globale Alt-Token/-Delivery-Schluessel werden entfernt. Intakte
WLAN-Werte koennen bleiben; fehlende Konfiguration oder Delivery-HMAC erfordert
neue Provisionierung und legt dauerhafte UDP-/API-High-Watermarks an.

<a id="de-web-deployment"></a>

## Raspberry-Pi-Deployment

Browserzugriff laeuft per HTTPS ueber `nginx`; nur `api.php` besitzt zusaetzlich
einen auf das IoT-Subnetz beschraenkten HTTP-Listener fuer die ESPs.

### Pakete und Dateien

```bash
sudo apt update
sudo apt install nginx php8.4-fpm python3-serial ffmpeg rsync fail2ban apache2-utils
sudo install -d -o root -g root -m 0755 /var/www/html
sudo rsync -a --delete --chown=root:root --exclude data/ --exclude deploy/ --exclude README.md web/ /var/www/html/
sudo useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin --gid www-data iot-alarm-monitor
sudo install -d -o iot-alarm-monitor -g www-data -m 2770 /var/www/html/data
sudo install -d -o iot-alarm-monitor -g www-data -m 0750 /var/www/html/data/recordings
```

`www-data` erhaelt weder `dialout` noch `sudo`; Serial-Port und Recorder
gehoeren nur dem Dienstbenutzer.

### PHP-FPM und einmaliger Bootstrap

```bash
sudo install -d -o www-data -g www-data -m 0700 /var/lib/iot-alarm
sudoedit /etc/php/8.4/fpm/pool.d/www.conf
```

Die folgenden Zeilen **innerhalb des vorhandenen `[www]`-Pools** eintragen:

```ini
env[IOT_ALARM_BOOTSTRAP_FILE] = /var/lib/iot-alarm/bootstrap.txt
env[IOT_ALARM_DATA_DIR] = /var/www/html/data
env[ALARM_IPC_SOCKET] = /run/iot-alarm-monitor/control.sock
```

```bash
sudo php-fpm8.4 -t
sudo systemctl restart php8.4-fpm
```

Beim ersten HTTPS-Aufruf erzeugt PHP Admin-Passwort, Alarm-PIN,
geraetespezifische API-/Delivery-Secrets und das gemeinsame UDP-Secret. Die
Bootstrap-Datei bleibt `0600`: lokal ablesen, Passwort/PIN ersetzen, ESPs
provisionieren und erst dann sicher loeschen. Sie darf nie per HTTP erreichbar
sein.

### TLS und nginx

```bash
sudo openssl req -x509 -nodes -days 365 -newkey rsa:3072 -keyout /etc/ssl/private/iot-dashboard.key -out /etc/ssl/certs/iot-dashboard.crt
sudo htpasswd -c /etc/nginx/.htpasswd-cam camera
```

In `web/deploy/nginx/iot-dashboard.conf` `<PI_IP_OR_HOSTNAME>`, `<PI_LAN_IP>`,
`<API_PORT>`, `<TRUSTED_LAN_CIDR>` und `<CAM_PORT>` ersetzen. Bei anderem
PHP-FPM-Socket auch `fastcgi_pass` anpassen.

```bash
sudo install -m 0644 web/deploy/nginx/iot-dashboard.conf /etc/nginx/sites-available/iot-dashboard
sudo ln -s /etc/nginx/sites-available/iot-dashboard /etc/nginx/sites-enabled/iot-dashboard
sudo rm -f /etc/nginx/sites-enabled/default
sudo nginx -t
sudo systemctl reload nginx
```

Fuer den regulaeren Betrieb ein vertrauenswuerdiges Zertifikat verwenden.

### Alarm-Monitor und Fail2ban

```bash
sudo install -m 0644 web/alarm_monitor.service /etc/systemd/system/iot-alarm-monitor.service
sudo install -m 0640 -o root -g root web/deploy/systemd/iot-alarm-monitor.env.example /etc/default/iot-alarm-monitor
sudo systemctl daemon-reload
sudo systemctl enable --now iot-alarm-monitor.service
```

In `/etc/default/iot-alarm-monitor` einen `/dev/serial/by-id/...`-Pfad nutzen.
Abweichende Daten-/Socketpfade muessen in PHP-FPM und Dienst uebereinstimmen;
bei anderem Datenpfad auch `ReadWritePaths=` per Unit-Override anpassen. Der
Dienst besitzt Serial, ffmpeg, Loeschpfade und Runtime-Log exklusiv; die Kamera
erwartet einen lokal gebundenen MJPEG-Stream.

Im aktiven `[www]`-Pool ausserdem setzen:

```ini
php_admin_flag[log_errors] = on
php_admin_value[error_log] = /var/log/fpm-php.www.log
```

```bash
sudo touch /var/log/fpm-php.www.log
sudo chown www-data:adm /var/log/fpm-php.www.log
sudo chmod 0640 /var/log/fpm-php.www.log
sudo php-fpm8.4 -t
sudo systemctl restart php8.4-fpm
sudo install -m 0644 web/deploy/fail2ban/filter.d/iot-login.conf /etc/fail2ban/filter.d/iot-login.conf
sudo install -m 0644 web/deploy/fail2ban/jail.d/iot-login.conf /etc/fail2ban/jail.d/iot-login.conf
sudo fail2ban-client -t
sudo systemctl restart fail2ban
sudo fail2ban-client status iot-login
```

Nur Loopback bleibt in `ignoreip`; keine Admin-/IoT-Netze pauschal ausnehmen.

<a id="de-naechste-schritte"></a>

## Danach

[Hardware](HARDWARE.md#deutsch) pruefen, [Security](../SECURITY.md#deutsch)
umsetzen und [Tests](TESTING.md#deutsch) ausfuehren.

<div align="center">

[![Nach oben](https://img.shields.io/badge/⬆_Nach_oben-24292f?style=for-the-badge)](#top)

</div>

---

<a id="english"></a>
<a id="en-build"></a>

# Build and Installation

This guide covers the three active firmware targets and the canonical
Raspberry Pi deployment. Historical sketches are not build inputs.

<a id="en-versions"></a>

## Verified Versions

Arduino CLI `1.5.1`; ESP8266 core `3.1.2`; AVR Boards `1.8.8`;
ArduinoJson `7.4.3`; WiFiManager `2.0.17`; TelnetStream `1.3.0`;
NetApiHelpers `1.0.3`; MFRC522 `1.4.12`; PHP `8.4.25`; Node.js `24.14`.

Newer versions are outside this evidence. Known ArduinoJson/ESP8266
deprecation warnings are not build failures.

<a id="en-toolchain"></a>

## Arduino Toolchain

```bash
arduino-cli config init
arduino-cli core update-index
arduino-cli core install arduino:avr@1.8.8
arduino-cli core install esp8266:esp8266@3.1.2 --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli lib install "ArduinoJson@7.4.3"
arduino-cli lib install "WiFiManager@2.0.17"
arduino-cli lib install "TelnetStream@1.3.0"
arduino-cli lib install "NetApiHelpers@1.0.3"
arduino-cli lib install "MFRC522@1.4.12"
```

<a id="en-firmware-builds"></a>

## Build the Firmware

```bash
arduino-cli compile --fqbn arduino:avr:uno firmware/elegoo_uno_r3/sketchR3/alarm_system
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 firmware/esp8266/sender
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 firmware/esp8266/receiver
```

Append `--port <PORT> --upload` to upload.

<a id="en-rfid"></a>

## Local RFID UIDs

The Uno default builds fail-closed and accepts no card. For real hardware, copy
`uid_whitelist.local.h.example` to `uid_whitelist.local.h` and insert locally read
UID bytes and length. Never commit the ignored `*.local.h`. The unchanged template
deliberately fails to compile; test disarmed with your own card.

<a id="en-esp-provisioning"></a>

## ESP First-Time Setup

Without a primary SSID, `Alarm-Sender-Konfig` or `Alarm-Empfaenger-Konfig` starts;
random WPA2 setup-AP/Telnet passwords are printed only over serial. On an isolated
network set: the same UDP HMAC secret (`>=32` characters) on both ESPs; a separate
32-character API token and delivery HMAC (`>=32`) per device; API IP, primary/
backup WiFi, and debug-Telnet password.

Never swap sender and receiver values. Upgrade both to `V15`; signed UDP ACKs
and `ALARMv2` are not fully compatible with older firmware. During migration,
a suitable legacy HMAC remains only as UDP secret; global legacy tokens and
delivery keys are removed. Intact WiFi values may stay; missing configuration
or delivery HMAC requires provisioning again and creates durable UDP/API high-water marks.

<a id="en-web-deployment"></a>

## Raspberry Pi Deployment

Browser access uses HTTPS through `nginx`; only `api.php` also has an HTTP
listener restricted to the IoT subnet for the ESPs.

### Packages and files

```bash
sudo apt update
sudo apt install nginx php8.4-fpm python3-serial ffmpeg rsync fail2ban apache2-utils
sudo install -d -o root -g root -m 0755 /var/www/html
sudo rsync -a --delete --chown=root:root --exclude data/ --exclude deploy/ --exclude README.md web/ /var/www/html/
sudo useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin --gid www-data iot-alarm-monitor
sudo install -d -o iot-alarm-monitor -g www-data -m 2770 /var/www/html/data
sudo install -d -o iot-alarm-monitor -g www-data -m 0750 /var/www/html/data/recordings
```

Grant `www-data` neither `dialout` nor `sudo`; only the service account owns
serial and recorder resources.

### PHP-FPM and one-time bootstrap

```bash
sudo install -d -o www-data -g www-data -m 0700 /var/lib/iot-alarm
sudoedit /etc/php/8.4/fpm/pool.d/www.conf
```

Add these lines **inside the existing `[www]` pool**:

```ini
env[IOT_ALARM_BOOTSTRAP_FILE] = /var/lib/iot-alarm/bootstrap.txt
env[IOT_ALARM_DATA_DIR] = /var/www/html/data
env[ALARM_IPC_SOCKET] = /run/iot-alarm-monitor/control.sock
```

```bash
sudo php-fpm8.4 -t
sudo systemctl restart php8.4-fpm
```

The first HTTPS request creates an admin password, alarm PIN, device-specific
API/delivery secrets, and the shared UDP secret. The bootstrap file stays
`0600`: read it locally, replace password/PIN, provision the ESPs, then delete
it securely. It must never be served over HTTP.

### TLS and nginx

```bash
sudo openssl req -x509 -nodes -days 365 -newkey rsa:3072 -keyout /etc/ssl/private/iot-dashboard.key -out /etc/ssl/certs/iot-dashboard.crt
sudo htpasswd -c /etc/nginx/.htpasswd-cam camera
```

Replace `<PI_IP_OR_HOSTNAME>`, `<PI_LAN_IP>`, `<API_PORT>`,
`<TRUSTED_LAN_CIDR>`, and `<CAM_PORT>` in
`web/deploy/nginx/iot-dashboard.conf`. Adapt `fastcgi_pass` for another FPM
socket.

```bash
sudo install -m 0644 web/deploy/nginx/iot-dashboard.conf /etc/nginx/sites-available/iot-dashboard
sudo ln -s /etc/nginx/sites-available/iot-dashboard /etc/nginx/sites-enabled/iot-dashboard
sudo rm -f /etc/nginx/sites-enabled/default
sudo nginx -t
sudo systemctl reload nginx
```

Use a trusted certificate for regular operation.

### Alarm monitor and Fail2ban

```bash
sudo install -m 0644 web/alarm_monitor.service /etc/systemd/system/iot-alarm-monitor.service
sudo install -m 0640 -o root -g root web/deploy/systemd/iot-alarm-monitor.env.example /etc/default/iot-alarm-monitor
sudo systemctl daemon-reload
sudo systemctl enable --now iot-alarm-monitor.service
```

Use a `/dev/serial/by-id/...` path in `/etc/default/iot-alarm-monitor`.
Custom data/socket paths must match in PHP-FPM and the service; for another
data path, also override `ReadWritePaths=`. The daemon exclusively owns serial,
ffmpeg, deletion paths, and its runtime log; the camera expects a locally bound
MJPEG stream.

Also add to the active `[www]` pool:

```ini
php_admin_flag[log_errors] = on
php_admin_value[error_log] = /var/log/fpm-php.www.log
```

```bash
sudo touch /var/log/fpm-php.www.log
sudo chown www-data:adm /var/log/fpm-php.www.log
sudo chmod 0640 /var/log/fpm-php.www.log
sudo php-fpm8.4 -t
sudo systemctl restart php8.4-fpm
sudo install -m 0644 web/deploy/fail2ban/filter.d/iot-login.conf /etc/fail2ban/filter.d/iot-login.conf
sudo install -m 0644 web/deploy/fail2ban/jail.d/iot-login.conf /etc/fail2ban/jail.d/iot-login.conf
sudo fail2ban-client -t
sudo systemctl restart fail2ban
sudo fail2ban-client status iot-login
```

Keep only loopback in `ignoreip`; never blanket-exempt admin/IoT networks.

<a id="en-next-steps"></a>

## Next Steps

Check [Hardware](HARDWARE.md#english), apply
[Security](../SECURITY.md#english), and run [Tests](TESTING.md#english).

<div align="center">

[![Back to top](https://img.shields.io/badge/⬆_Back_to_top-24292f?style=for-the-badge)](#top)

</div>
