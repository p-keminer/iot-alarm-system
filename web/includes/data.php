<?php
// ============================================================
// includes/data.php - Diagnose- und Telemetrie-Daten laden
// ============================================================
// Lädt Gerätestatus, Telemetrie-CSV und System-Logs für
// die Diagnose-Ansicht und die Chart-Darstellung.
// Nur für eingeloggte Benutzer ausführen.
// ============================================================

$statusFile = 'data/status.json';   // Geräte-Statusdaten (von ESP-Nodes gemeldet)
$logFile    = 'data/log.txt';       // System-Log (textbasiert)
$csvFile    = 'data/telemetry.csv'; // Telemetrie-Zeitreihen (RSSI, Heap, etc.)

// Geräte-Status laden
$diagStatus = [];
if (file_exists($statusFile)) {
    $diagStatus = json_decode(file_get_contents($statusFile), true);
}

// === TELEMETRIE-DATEN FÜR CHARTS AUFBEREITEN ===
// Struktur: Für jedes Gerät (sender/receiver/camera) werden
// RSSI, Heap und Zeitstempel in Arrays gesammelt
$chartData = [
    'sender'   => ['rssi' => [], 'heap' => [], 'time' => []],
    'receiver' => ['rssi' => [], 'heap' => [], 'time' => []],
    'camera'   => ['rssi' => [], 'heap' => [], 'time' => []]
];

if (file_exists($csvFile)) {
    $lines = file($csvFile);
    // Nur die letzten 100 Zeilen für Performance
    $lines = array_slice($lines, -100);

    foreach ($lines as $line) {
        // CSV-Format: timestamp,source,rssi,heap
        $parts = explode(",", trim($line));
        if (count($parts) >= 4) {
            $timestamp = (int)$parts[0];
            $source    = trim($parts[1]);
            $rssi      = (int)$parts[2];
            $heap      = (int)$parts[3];

            // Nur bekannte Quellen akzeptieren
            if (isset($chartData[$source])) {
                $chartData[$source]['time'][] = $timestamp;
                $chartData[$source]['rssi'][] = $rssi;
                $chartData[$source]['heap'][] = $heap;
            }
        }
    }
}

// System-Logs laden (letzte 50 Zeilen)
$systemLogs = [];
if (file_exists($logFile)) {
    $systemLogs = array_slice(file($logFile), -50);
}
