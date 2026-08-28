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
$configuredDataDir = getenv('IOT_ALARM_DATA_DIR');
$settingsDataDir = (is_string($configuredDataDir) && $configuredDataDir !== '')
    ? rtrim($configuredDataDir, '/\\')
    : dirname(__DIR__) . DIRECTORY_SEPARATOR . 'data';
$confFile = $settingsDataDir . DIRECTORY_SEPARATOR . 'settings.json';

/** Persistiert Rename/Unlink-Metadaten; auf Linux ist ein Fehlschlag kritisch. */
function fsyncDirectoryAfterMutation($directory) {
    if (!function_exists('fsync')) return;
    $handle = @fopen($directory, 'rb');
    $ok = $handle !== false && @fsync($handle);
    if ($handle !== false) fclose($handle);
    if (!$ok && PHP_OS_FAMILY === 'Linux') {
        throw new RuntimeException('Verzeichnis-Metadaten koennen nicht synchronisiert werden.');
    }
}

function writeDurableAtomicFile($file, $contents, $mode, $errorMessage) {
    $dir = dirname($file);
    if (!is_dir($dir) && !mkdir($dir, 0750, true) && !is_dir($dir)) {
        throw new RuntimeException($errorMessage);
    }
    $tmp = $file . '.tmp.' . bin2hex(random_bytes(4));
    $fp = fopen($tmp, 'xb');
    if (!$fp) throw new RuntimeException($errorMessage);
    // Rechte muessen vor dem ersten geheimen Byte gelten. Ein nachtraegliches
    // chmod laesst sonst bei einer permissiven umask ein kurzes 0644-Fenster.
    if (!@chmod($tmp, $mode)) {
        fclose($fp);
        @unlink($tmp);
        throw new RuntimeException($errorMessage);
    }
    try {
        $offset = 0;
        $length = strlen($contents);
        while ($offset < $length) {
            $written = fwrite($fp, substr($contents, $offset));
            if ($written === false || $written === 0) throw new RuntimeException($errorMessage);
            $offset += $written;
        }
        if (!fflush($fp)) throw new RuntimeException($errorMessage);
        if (function_exists('fsync') && !fsync($fp)) throw new RuntimeException($errorMessage);
    } catch (Throwable $e) {
        fclose($fp);
        @unlink($tmp);
        throw $e;
    }
    fclose($fp);
    if (!rename($tmp, $file)) {
        @unlink($tmp);
        throw new RuntimeException($errorMessage);
    }
    fsyncDirectoryAfterMutation($dir);
}

/** Schreibt Settings erst nach vollstaendigem, fsync-gesichertem Temp-Write. */
function writeSettingsAtomic($file, $data) {
    $json = json_encode($data, JSON_UNESCAPED_SLASHES);
    if ($json === false) throw new RuntimeException('Konfiguration kann nicht kodiert werden.');
    writeDurableAtomicFile($file, $json, 0640, 'Konfiguration kann nicht sicher geschrieben werden.');
}

/**
 * Ein expliziter Pfad ausserhalb des Web-Roots bleibt bevorzugt. Ohne
 * Deployment-Override liegt der Nachweis im ohnehin vom Webserver gesperrten
 * data-Verzeichnis; dieses ist bei einer frischen Installation bereits fuer
 * www-data beschreibbar. Der Dateiname ist zusaetzlich ein Dotfile.
 */
function bootstrapCredentialFile() {
    $configured = getenv('IOT_ALARM_BOOTSTRAP_FILE');
    if (is_string($configured) && $configured !== '') return $configured;
    $dataDir = getenv('IOT_ALARM_DATA_DIR');
    if (!is_string($dataDir) || $dataDir === '') $dataDir = dirname(__DIR__) . DIRECTORY_SEPARATOR . 'data';
    return rtrim($dataDir, '/\\') . DIRECTORY_SEPARATOR . '.bootstrap-credentials';
}

function writeBootstrapCredentials($credentials) {
    if (empty($credentials)) return;
    $file = bootstrapCredentialFile();
    $existing = [];
    if (file_exists($file)) {
        $content = file_get_contents($file);
        if ($content === false) throw new RuntimeException('Bootstrap-Datei kann nicht gelesen werden.');
        foreach (preg_split('/\R/', $content) as $line) {
            if (preg_match('/^(Admin password|Alarm PIN|Sender API token|Sender delivery HMAC|Receiver API token|Receiver delivery HMAC|Camera API token|Camera delivery HMAC|UDP HMAC secret): (.+)$/', $line, $m)) {
                $existing[$m[1]] = $m[2];
            }
        }
    }
    $credentials = array_replace($existing, $credentials);
    $lines = [
        'IoT Alarm System - einmalige Ersteinrichtung',
        'Datei nach erfolgreicher Einrichtung sicher loeschen.',
        ''
    ];
    foreach ($credentials as $label => $value) {
        $lines[] = $label . ': ' . $value;
    }
    $dir = dirname($file);
    if (!is_dir($dir) || !is_writable($dir)) {
        throw new RuntimeException('Bootstrap-Verzeichnis ist nicht beschreibbar.');
    }
    writeDurableAtomicFile(
        $file,
        implode(PHP_EOL, $lines) . PHP_EOL,
        0600,
        'Bootstrap-Zugangsdaten koennen nicht sicher gespeichert werden.'
    );
}

/** Liest nur vorhandene Bootstrap-Labels; geheime Werte verlassen die Datei nie. */
function bootstrapCredentialLabels() {
    $file = bootstrapCredentialFile();
    if (!file_exists($file)) return [];
    $content = file_get_contents($file);
    if ($content === false) throw new RuntimeException('Bootstrap-Datei kann nicht gelesen werden.');
    $labels = [];
    foreach (preg_split('/\R/', $content) as $line) {
        if (preg_match('/^(Admin password|Alarm PIN|Sender API token|Sender delivery HMAC|Receiver API token|Receiver delivery HMAC|Camera API token|Camera delivery HMAC|UDP HMAC secret): (.+)$/', $line, $m)) {
            $labels[$m[1]] = true;
        }
    }
    return $labels;
}

// Sichere, installationsspezifische Standardwerte. Es gibt bewusst kein
// bekanntes Betriebs-Passwort und keinen bekannten Alarm-PIN mehr.
$defaults = [
    'refresh_rate'           => 2000,
    'site_title'             => 'IoT Control Center',
    'timeout_active'         => true,
    'timeout_minutes'        => 5,
    'camera_port'            => 8082,
    'bootstrap_admin_pending'=> false,
    'bootstrap_pin_pending'  => false
];

if (!is_dir(dirname($confFile)) && !mkdir(dirname($confFile), 0750, true) && !is_dir(dirname($confFile))) {
    throw new RuntimeException('Konfigurationsverzeichnis kann nicht angelegt werden.');
}
$bootstrapLock = fopen(dirname($confFile) . DIRECTORY_SEPARATOR . 'settings.bootstrap.lock', 'c+');
if (!$bootstrapLock || !flock($bootstrapLock, LOCK_EX)) {
    if ($bootstrapLock) fclose($bootstrapLock);
    throw new RuntimeException('Bootstrap-Lock kann nicht gesetzt werden.');
}
@chmod(dirname($confFile) . DIRECTORY_SEPARATOR . 'settings.bootstrap.lock', 0640);

try {
    // Read/Generate/Write liegt vollständig unter demselben Prozess-Lock.
    $settings = [];
    if (file_exists($confFile)) {
        $rawSettings = file_get_contents($confFile);
        if ($rawSettings === false) throw new RuntimeException('Konfiguration kann nicht gelesen werden.');
        $settings = json_decode($rawSettings, true);
        if (!is_array($settings) || json_last_error() !== JSON_ERROR_NONE) {
            throw new RuntimeException('Konfiguration enthaelt ungueltiges JSON; keine automatische Ueberschreibung.');
        }
    }
    $settings = array_replace($defaults, $settings);
    $bootstrap = [];
    $changed = false;
    $bootstrapLabels = bootstrapCredentialLabels();

    if (empty($settings['password']) || !is_string($settings['password']) ||
        password_verify('CHANGE_ME', $settings['password']) ||
        (!empty($settings['bootstrap_admin_pending']) && empty($bootstrapLabels['Admin password']))) {
        $initialAdminPassword = bin2hex(random_bytes(12));
        $settings['password'] = password_hash($initialAdminPassword, PASSWORD_BCRYPT);
        $settings['bootstrap_admin_pending'] = true;
        $bootstrap['Admin password'] = $initialAdminPassword;
        $changed = true;
    }

    if (empty($settings['alarm_pin']) || !is_string($settings['alarm_pin']) ||
        password_verify('CHANGE_ME', $settings['alarm_pin']) ||
        (!empty($settings['bootstrap_pin_pending']) && empty($bootstrapLabels['Alarm PIN']))) {
        $initialAlarmPin = str_pad((string)random_int(0, 999999), 6, '0', STR_PAD_LEFT);
        $settings['alarm_pin'] = password_hash($initialAlarmPin, PASSWORD_BCRYPT);
        $settings['bootstrap_pin_pending'] = true;
        $bootstrap['Alarm PIN'] = $initialAlarmPin;
        $changed = true;
    }

    // Alte schnelle PIN-Verifier werden aktiv entfernt; nur bcrypt bleibt at rest.
    if (array_key_exists('alarm_pin_sha256', $settings)) {
        unset($settings['alarm_pin_sha256']);
        $changed = true;
    }

    $deviceLabels = [
        'sender'   => ['Sender API token',   'Sender delivery HMAC'],
        'receiver' => ['Receiver API token', 'Receiver delivery HMAC'],
        'camera'   => ['Camera API token',   'Camera delivery HMAC']
    ];
    foreach ($deviceLabels as $device => $labels) {
        $tokenKey = $device . '_api_token';
        $hmacKey = $device . '_hmac_secret';
        if (empty($settings[$tokenKey]) || !is_string($settings[$tokenKey]) || strlen($settings[$tokenKey]) < 32) {
            $settings[$tokenKey] = bin2hex(random_bytes(16));
            $bootstrap[$labels[0]] = $settings[$tokenKey];
            $changed = true;
        }
        if (empty($settings[$hmacKey]) || !is_string($settings[$hmacKey]) || strlen($settings[$hmacKey]) < 32) {
            $settings[$hmacKey] = bin2hex(random_bytes(20));
            $bootstrap[$labels[1]] = $settings[$hmacKey];
            $changed = true;
        }
    }

    if (empty($settings['udp_hmac_secret']) || !is_string($settings['udp_hmac_secret']) || strlen($settings['udp_hmac_secret']) < 32) {
        // Das bisherige HMAC kann fuer den ausschliesslich Sender<->Receiver
        // genutzten UDP-Kanal migriert werden. HTTP-Delivery bekommt bewusst
        // neue, source-getrennte Schluessel.
        $legacyHmac = $settings['hmac_secret'] ?? '';
        $settings['udp_hmac_secret'] = is_string($legacyHmac) && strlen($legacyHmac) >= 32
            ? $legacyHmac
            : bin2hex(random_bytes(20));
        $bootstrap['UDP HMAC secret'] = $settings['udp_hmac_secret'];
        $changed = true;
    }

    // Globale Alt-Credentials duerfen nach der Migration nicht mehr als
    // Cross-Source-Fallback im Betrieb verbleiben.
    foreach (['esp_token', 'hmac_secret'] as $legacyKey) {
        if (array_key_exists($legacyKey, $settings)) {
            unset($settings[$legacyKey]);
            $changed = true;
        }
    }

    if ($changed || !file_exists($confFile)) {
        // Fail-safe Reihenfolge: Ohne lesbare Bootstrap-Datei werden die neuen
        // Hashes niemals aktiviert. Ein Settings-Fehler kann nicht aussperren.
        writeBootstrapCredentials($bootstrap);
        writeSettingsAtomic($confFile, $settings);
    }
} finally {
    flock($bootstrapLock, LOCK_UN);
    fclose($bootstrapLock);
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
    'file'           => $settingsDataDir . DIRECTORY_SEPARATOR . 'login_attempts.json'
];

// Der kurze Alarm-PIN erhaelt ein eigenes, deutlich strengeres und dauerhaftes
// Ledger. Damit teilt er weder Kontingent noch Sperrstatus mit dem Login.
$alarmPinBruteForceConfig = [
    'lockout_tiers' => [
        ['attempts' => 3,  'lockout_seconds' => 300],
        ['attempts' => 6,  'lockout_seconds' => 1800],
        ['attempts' => 10, 'lockout_seconds' => 86400],
    ],
    'attempt_window' => 86400,
    'file'           => $settingsDataDir . DIRECTORY_SEPARATOR . 'alarm_pin_attempts.json'
];
