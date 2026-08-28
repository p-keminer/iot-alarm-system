<a id="top"></a>

<div align="center">

[![Deutsch](https://img.shields.io/badge/🇩🇪_Deutsch-24292f?style=for-the-badge)](#deutsch)
[![English](https://img.shields.io/badge/🇬🇧_English-24292f?style=for-the-badge)](#english)

</div>

---

<a id="deutsch"></a>

<a id="de-safety"></a>

# Safety

Dieses Repository beschreibt einen nicht zertifizierten Lern- und Demonstrationsprototyp.

<a id="de-grenzen"></a>

## Grenzen

- Das System ist keine zertifizierte Alarmanlage.
- Strom-, WLAN-, Software-, Sensor-, Speicher- und Verdrahtungsfehler koennen LEDs, Summer, Webstatus und Kamera ausfallen lassen.
- Ein offener Reed-Kreis loest scharf Alarm aus. Das behandelt Kabelbruch konservativ, ist aber keine zertifizierte Sabotageerkennung.
- Der Uno stellt Scharf-/Alarmzustand aus EEPROM wieder her; ein Neustart schaltet nicht unscharf.
- Bei unlesbarem Sicherheitszustand aktiviert der Empfaenger fail-secure seine Aktoren. Der lokale Uno-Pfad bleibt auch bei Ausfall von Funk oder Dashboard massgeblich.

<a id="de-kamera"></a>

## Kamera und Datenschutz

Nur eigene oder ausdruecklich freigegebene Bereiche aufnehmen. Aufnahmen und Logs koennen Personen, Zeitstempel, IP-Adressen und Anwesenheitsmuster enthalten; lokale Gesetze, Informationspflichten, Zugriffsschutz, verschluesselte Backups und Aufbewahrungsfristen beachten.

Netzwerk- und Zugriffsgrenzen: [Security](SECURITY.md#deutsch).

<div align="center">

[![Nach oben](https://img.shields.io/badge/⬆_Nach_oben-24292f?style=for-the-badge)](#top)

</div>

---

<a id="english"></a>

<a id="en-safety"></a>

# Safety

This repository documents an uncertified learning and demonstration prototype.

<a id="en-limitations"></a>

## Limitations

- The system is not a certified alarm system.
- Power, WiFi, software, sensor, storage, and wiring faults can disable LEDs, buzzers, web status, or the camera.
- Either open reed circuit triggers an armed system. This treats a broken wire conservatively but is not certified tamper detection.
- The Uno restores armed/alarm state from EEPROM; a restart does not disarm it.
- Unreadable security state makes the receiver fail-secure drive its actuators. The local Uno path remains authoritative if radio or dashboard access fails.

<a id="en-camera"></a>

## Camera and Privacy

Record only your own areas or areas with explicit permission. Recordings and logs may contain people, timestamps, IP addresses, and presence patterns; observe local law, notification duties, access control, encrypted backups, and retention periods.

Network and access boundaries: [Security](SECURITY.md#english).

<div align="center">

[![Back to top](https://img.shields.io/badge/⬆_Back_to_top-24292f?style=for-the-badge)](#top)

</div>
