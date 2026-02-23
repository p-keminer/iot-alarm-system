<?php
// ============================================================
// includes/config.php - Einstellungen & Brute-Force-Konfiguration
// ============================================================
// Lädt die Dashboard-Konfiguration aus data/settings.json und
// definiert die Brute-Force-Schutz-Parameter.
// ============================================================

// === PHP FEHLERBEHANDLUNG ===
// Fehler werden geloggt, aber NICHT im Browser angezeigt (Sicherheit)
ini_set('display_errors', 0);
ini_set('display_startup_errors', 0);
ini_set('log_errors', 1);
error_reporting(E_ALL);

// ============================================================
// EINSTELLUNGEN LADEN
// ============================================================
$confFile = 'data/settings.json';

// Standardwerte falls keine Konfiguration existiert
$defaults = [
    "password"       => password_hash("CHANGE_ME", PASSWORD_BCRYPT),  // BCrypt-Hash des Admin-Passworts
    "refresh_rate"   => 2000,                                         // Dashboard-Aktualisierung in ms
    "site_title"     => "IoT Control Center",                         // Seitentitel
    "timeout_active" => true,                                         // Auto-Logout aktiviert
    "timeout_minutes"=> 5,                                            // Timeout nach X Minuten Inaktivität
    "esp_token"      => bin2hex(random_bytes(16)),                    // Token für ESP-API-Authentifizierung
    "camera_port"    => 8082                                          // Port des Kamera-Streams
];

// Konfiguration laden oder Standardwerte verwenden
$settings = [];
if (file_exists($confFile)) {
    $settings = json_decode(file_get_contents($confFile), true);
}
// Falls Konfiguration fehlt oder ungültig → Defaults schreiben
if (!$settings || !isset($settings['password'])) {
    $settings = $defaults;
    if (!is_dir('data')) mkdir('data', 0750, true);
    file_put_contents($confFile, json_encode($settings));
}

// ============================================================
// BRUTE-FORCE SCHUTZ - Konfiguration
// ============================================================
// Progressives Lockout-System:
// - Stufe 1: Nach 5 Fehlversuchen → 5 Minuten Sperre
// - Stufe 2: Nach 10 Fehlversuchen → 15 Minuten Sperre
// - Stufe 3: Nach 15 Fehlversuchen → 60 Minuten Sperre
$bruteForceConfig = [
    'lockout_tiers' => [
        ['attempts' => 5,  'lockout_seconds' => 300],   // 5 Versuche → 5 Min Sperre
        ['attempts' => 10, 'lockout_seconds' => 900],   // 10 Versuche → 15 Min Sperre
        ['attempts' => 15, 'lockout_seconds' => 3600],  // 15 Versuche → 60 Min Sperre
    ],
    'attempt_window' => 3600,  // Zeitfenster: Fehlversuche älter als 1h werden vergessen
    'file'           => 'data/login_attempts.json'  // Datei für Fehlversuch-Tracking
];
