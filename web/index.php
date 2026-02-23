<?php
// ============================================================
// index.php - IoT Control Center Dashboard
// ============================================================
// Hauptdatei des IoT-Dashboards. EnthÃ¤lt:
// - Session-Management mit Security-Hardening
// - Login-System mit Brute-Force-Schutz (progressiv)
// - Auto-Logout bei InaktivitÃ¤t
// - Dashboard mit Echtzeit-GerÃ¤testatus
// - Live-Kommunikations-Logs
// - System-Diagnose mit Telemetrie-Charts
// - Konfigurationsseite fÃ¼r Dashboard & ESP-Nodes
// - Audit-Log (nur Admin)
// - Alarm-Aufnahmen-Verwaltung
// ============================================================

// Session Security ist in /etc/php/8.4/fpm/pool.d/www.conf konfiguriert
session_start();

function loadUserLogs() {
    $file = 'data/user_logs.json';
    if (!file_exists($file)) return [];
    $data = json_decode(file_get_contents($file), true);
    return is_array($data) ? $data : [];
}

// === PHP FEHLERBEHANDLUNG ===
// Fehler werden geloggt, aber NICHT im Browser angezeigt (Sicherheit)
ini_set('display_errors', 0);
ini_set('display_startup_errors', 0);
ini_set('log_errors', 1);
error_reporting(E_ALL);

// ============================================================
// EINSTELLUNGEN LADEN
// ============================================================
// Konfigurationsdatei mit allen Dashboard-Einstellungen
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
// Falls Konfiguration fehlt oder ungültig ? Defaults schreiben
if (!$settings || !isset($settings['password'])) {
    $settings = $defaults;
    if (!is_dir('data')) mkdir('data', 0750, true);
    file_put_contents($confFile, json_encode($settings));
}

// ============================================================
// BRUTE-FORCE SCHUTZ - Konfiguration
// ============================================================
// Progressives Lockout-System:
// - Stufe 1: Nach 5 Fehlversuchen ? 5 Minuten Sperre
// - Stufe 2: Nach 10 Fehlversuchen ? 15 Minuten Sperre
// - Stufe 3: Nach 15 Fehlversuchen ? 60 Minuten Sperre
$bruteForceConfig = [
    'lockout_tiers' => [
        ['attempts' => 5,  'lockout_seconds' => 300],   // 5 Versuche ? 5 Min Sperre
        ['attempts' => 10, 'lockout_seconds' => 900],   // 10 Versuche ? 15 Min Sperre
        ['attempts' => 15, 'lockout_seconds' => 3600],  // 15 Versuche ? 60 Min Sperre
    ],
    'attempt_window' => 3600,  // Zeitfenster: Fehlversuche älter als 1h werden vergessen
    'file'           => 'data/login_attempts.json'  // Datei für Fehlversuch-Tracking
];

// ============================================================
// BRUTE-FORCE HILFSFUNKTIONEN
// ============================================================

/**
 * Prüft den Brute-Force-Status für eine IP-Adresse.
 * 
 * @param string $ip           - Die zu prüfende IP-Adresse
 * @param array  $bfConfig     - Brute-Force-Konfiguration
 * @return array               - [blocked => bool, remaining_attempts => int, 
 *                                lockout_remaining => int, total_attempts => int, tier => int]
 */
function checkBruteForce($ip, $bfConfig) {
    $attemptFile = $bfConfig['file'];
    $now = time();
    
    // Fehlversuch-Daten laden
    $attempts = [];
    if (file_exists($attemptFile)) {
        $attempts = json_decode(file_get_contents($attemptFile), true) ?: [];
    }
    
    // Keine Einträge für diese IP ? nicht gesperrt
    if (!isset($attempts[$ip])) {
        return [
            'blocked'             => false,
            'remaining_attempts'  => $bfConfig['lockout_tiers'][0]['attempts'],
            'lockout_remaining'   => 0,
            'total_attempts'      => 0,
            'tier'                => 0
        ];
    }
    
    // Veraltete Einträge entfernen (älter als attempt_window)
    $attempts[$ip] = array_values(array_filter($attempts[$ip], function($entry) use ($now, $bfConfig) {
        $timestamp = is_array($entry) ? $entry['time'] : $entry;
        return ($now - $timestamp) < $bfConfig['attempt_window'];
    }));
    
    // Aktualisierte Daten speichern
    if (empty($attempts[$ip])) {
        unset($attempts[$ip]);
        file_put_contents($attemptFile, json_encode($attempts));
        return [
            'blocked'             => false,
            'remaining_attempts'  => $bfConfig['lockout_tiers'][0]['attempts'],
            'lockout_remaining'   => 0,
            'total_attempts'      => 0,
            'tier'                => 0
        ];
    }
    file_put_contents($attemptFile, json_encode($attempts));
    
    $totalAttempts = count($attempts[$ip]);
    $lastAttemptTime = 0;
    
    // Letzten Fehlversuch-Zeitpunkt ermitteln
    foreach ($attempts[$ip] as $entry) {
        $t = is_array($entry) ? $entry['time'] : $entry;
        if ($t > $lastAttemptTime) $lastAttemptTime = $t;
    }
    
    // Prüfe jede Lockout-Stufe (von höchster zu niedrigster)
    $tiers = $bfConfig['lockout_tiers'];
    $currentTier = 0;
    for ($i = count($tiers) - 1; $i >= 0; $i--) {
        if ($totalAttempts >= $tiers[$i]['attempts']) {
            $currentTier = $i + 1;
            $lockoutEnd = $lastAttemptTime + $tiers[$i]['lockout_seconds'];
            
            // Ist die Sperre noch aktiv?
            if ($now < $lockoutEnd) {
                return [
                    'blocked'            => true,
                    'remaining_attempts' => 0,
                    'lockout_remaining'  => $lockoutEnd - $now,  // Sekunden bis Entsperrung
                    'total_attempts'     => $totalAttempts,
                    'tier'               => $currentTier
                ];
            }
            break;
        }
    }
    
    // Nicht gesperrt ? berechne verbleibende Versuche bis nächste Stufe
    $nextTierAttempts = $bfConfig['lockout_tiers'][0]['attempts'];  // Default: erste Stufe
    foreach ($tiers as $tier) {
        if ($totalAttempts < $tier['attempts']) {
            $nextTierAttempts = $tier['attempts'];
            break;
        }
    }
    $remaining = max(0, $nextTierAttempts - $totalAttempts);
    
    return [
        'blocked'            => false,
        'remaining_attempts' => $remaining,
        'lockout_remaining'  => 0,
        'total_attempts'     => $totalAttempts,
        'tier'               => $currentTier
    ];
}

/**
 * Registriert einen fehlgeschlagenen Login-Versuch für eine IP.
 * 
 * @param string $ip        - IP-Adresse des Angreifers
 * @param array  $bfConfig  - Brute-Force-Konfiguration
 */
function recordFailedAttempt($ip, $bfConfig) {
    $attemptFile = $bfConfig['file'];
    $attempts = [];
    if (file_exists($attemptFile)) {
        $attempts = json_decode(file_get_contents($attemptFile), true) ?: [];
    }
    if (!isset($attempts[$ip])) {
        $attempts[$ip] = [];
    }
    // Neuen Fehlversuch mit Zeitstempel und User-Agent speichern
    $attempts[$ip][] = [
        'time'  => time(),
        'agent' => $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown'
    ];
    if (!is_dir('data')) mkdir('data', 0750, true);
    file_put_contents($attemptFile, json_encode($attempts));
}

/**
 * Löscht alle Fehlversuche einer IP nach erfolgreichem Login.
 * 
 * @param string $ip        - IP-Adresse
 * @param array  $bfConfig  - Brute-Force-Konfiguration
 */
function clearFailedAttempts($ip, $bfConfig) {
    $attemptFile = $bfConfig['file'];
    $attempts = [];
    if (file_exists($attemptFile)) {
        $attempts = json_decode(file_get_contents($attemptFile), true) ?: [];
    }
    unset($attempts[$ip]);
    file_put_contents($attemptFile, json_encode($attempts));
}

// ============================================================
// LOGIN-VERARBEITUNG
// ============================================================
// ============================================================
// LOGIN-STATUS AUS SESSION LESEN (von keks.php gesetzt)
// ============================================================
$loginFailed = isset($_SESSION['login_failed']) ? $_SESSION['login_failed'] : false;
$bruteForceStatus = isset($_SESSION['brute_force_status']) ? $_SESSION['brute_force_status'] : null;

// Session-Flags zurÃ¼cksetzen (nur einmal anzeigen)
unset($_SESSION['login_failed']);
unset($_SESSION['brute_force_status']);

// ============================================================
// SESSION TIMEOUT CHECK
// ============================================================
// Prüft ob die aktive Session wegen Inaktivität abgelaufen ist
if (isset($_SESSION['loggedin']) && $_SESSION['loggedin'] === true) {
    if ($settings['timeout_active'] && isset($_SESSION['last_activity'])) {
        $timeout_seconds = ($settings['timeout_minutes'] ?? 5) * 60;
        
        if (time() - $_SESSION['last_activity'] > $timeout_seconds) {
            // Timeout erreicht ? Auto-Logout durchführen
            $logs = loadUserLogs();

            // Audit-Log-Eintrag für Auto-Logout
            
            $ip = $_SERVER['REMOTE_ADDR'];
            $agent = $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown';
            $device = gethostbyaddr($ip);
            if ($device == $ip) $device = "Unknown Device";
            
            // Sitzungsdauer berechnen
            $sessionDuration = isset($_SESSION['login_time']) ? (time() - $_SESSION['login_time']) : 0;
            $durationStr = $sessionDuration > 0 ? gmdate("H:i:s", $sessionDuration) : "0s";
            
            $newLog = [
                'timestamp'   => time(),
                'date'        => date("d.m.Y H:i:s"),
                'ip'          => $ip,
                'device_name' => $device,
                'user_agent'  => $agent,
                'action'      => 'AUTO-LOGOUT',
                'details'     => "Timeout nach Inaktivität (Sitzungsdauer: $durationStr)",
                'session_id'  => session_id()
            ];
            array_unshift($logs, $newLog);
            $logs = array_slice($logs, 0, 100);
            if (!is_dir('data')) mkdir("data", 0750, true);
            file_put_contents("data/user_logs.json", json_encode($logs));
            
            // Session zerstören und zur Login-Seite weiterleiten
            session_destroy();
            header("Location: index.php?timeout=1");
            exit;
        }
    }
    // Aktivitäts-Timestamp aktualisieren bei jedem Seitenaufruf
    $_SESSION['last_activity'] = time();
}

// ============================================================
// MANUELLER LOGOUT
// ============================================================
if (isset($_GET['logout'])) {
    // Audit-Log-Eintrag für manuellen Logout
    $logs = loadUserLogs();	    
    $ip = $_SERVER['REMOTE_ADDR'];
    $agent = $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown';
    $device = gethostbyaddr($ip);
    if ($device == $ip) $device = "Unknown Device";
    
    $sessionDuration = isset($_SESSION['login_time']) ? (time() - $_SESSION['login_time']) : 0;
    $durationStr = $sessionDuration > 0 ? gmdate("H:i:s", $sessionDuration) : "0s";
    
    $newLog = [
        'timestamp'   => time(),
        'date'        => date("d.m.Y H:i:s"),
        'ip'          => $ip,
        'device_name' => $device,
        'user_agent'  => $agent,
        'action'      => 'LOGOUT',
        'details'     => "Sitzungsdauer: $durationStr",
        'session_id'  => session_id()
    ];
    array_unshift($logs, $newLog);
    $logs = array_slice($logs, 0, 100);
    file_put_contents("data/user_logs.json", json_encode($logs));
    
    session_destroy();
    header("Location: index.php");
    exit;
}

// ============================================================
// LOGIN-MASKE (wird angezeigt wenn NICHT eingeloggt)
// ============================================================
if (!isset($_SESSION['loggedin']) || $_SESSION['loggedin'] !== true) {
    
    // Timeout-Warnmeldung (nach Session-Ablauf)
    $timeoutMsg = isset($_GET['timeout']) 
        ? '<div class="alert-warning"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><path d="M12 6v6l4 2"/></svg><span>Session expired due to inactivity</span></div>' 
        : '';
    
    // Fehlermeldung bei falschem Passwort
    $failedMsg = '';
    if (isset($loginFailed) && $loginFailed) {
        if ($bruteForceStatus && $bruteForceStatus['blocked']) {
            // === GESPERRT: Lockout-Meldung mit Countdown ===
            $lockoutMin = ceil($bruteForceStatus['lockout_remaining'] / 60);
            $lockoutSec = $bruteForceStatus['lockout_remaining'];
            $tierText = '';
            if ($bruteForceStatus['tier'] >= 3) {
                $tierText = ' (Stufe 3 - Maximale Sperre)';
            } elseif ($bruteForceStatus['tier'] >= 2) {
                $tierText = ' (Stufe 2)';
            }
            $failedMsg = '<div class="alert-warning" style="background:#fee2e2;border-color:#ef4444;color:#991b1b;">'
                . '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">'
                . '<path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/></svg>'
                . '<div><strong>Account temporarily locked' . htmlspecialchars($tierText) . '</strong><br>'
                . '<span style="font-size:13px;">Too many failed attempts (' . $bruteForceStatus['total_attempts'] . '). '
                . 'Try again in <span id="lockout-timer" data-seconds="' . $lockoutSec . '">' . $lockoutMin . ' min</span>.</span></div></div>';
        } else {
            // === Normaler Fehlversuch mit Restversuche-Anzeige ===
            $remainingText = '';
            if ($bruteForceStatus && $bruteForceStatus['remaining_attempts'] <= 3) {
                $remainingText = '<br><span style="font-size:12px;">? ' . $bruteForceStatus['remaining_attempts'] . ' attempt(s) remaining before lockout</span>';
            }
            $failedMsg = '<div class="alert-warning" style="background:#fee2e2;border-color:#ef4444;color:#991b1b;">'
                . '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">'
                . '<circle cx="12" cy="12" r="10"/><path d="M15 9l-6 6M9 9l6 6"/></svg>'
                . '<div>Invalid password' . $remainingText . '</div></div>';
        }
    }
    
    // Prüfe ob das Login-Formular disabled sein soll (bei Lockout)
    $formDisabled = ($bruteForceStatus && $bruteForceStatus['blocked']) ? 'disabled' : '';
    $btnExtraStyle = ($bruteForceStatus && $bruteForceStatus['blocked']) ? 'opacity:0.5;cursor:not-allowed;' : '';
    
    // === LOGIN HTML AUSGABE ===
    echo '<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>Login</title>'
        . '<link rel="preconnect" href="https://fonts.googleapis.com">'
        . '<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">'
        . '<style>'
        . '*{margin:0;padding:0;box-sizing:border-box;}'
        . 'body{font-family:"Inter",sans-serif;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;}'
        . '.login-container{background:rgba(255,255,255,0.95);backdrop-filter:blur(10px);padding:40px;border-radius:16px;box-shadow:0 20px 60px rgba(0,0,0,0.3);width:100%;max-width:400px;}'
        . '.login-header{text-align:center;margin-bottom:32px;}'
        . '.login-header svg{width:48px;height:48px;margin-bottom:16px;color:#667eea;}'
        . '.login-header h1{font-size:24px;font-weight:700;color:#1a1a1a;margin-bottom:8px;}'
        . '.login-header p{color:#666;font-size:14px;}'
        . '.alert-warning{display:flex;align-items:center;gap:12px;padding:12px 16px;background:#fff3cd;border:1px solid #ffc107;border-radius:8px;color:#856404;font-size:14px;margin-bottom:20px;}'
        . '.alert-warning svg{flex-shrink:0;}'
        . '.form-group{margin-bottom:20px;}'
        . '.form-label{display:block;font-weight:500;font-size:14px;color:#333;margin-bottom:8px;}'
        . '.form-input{width:100%;padding:12px 16px;border:2px solid #e0e0e0;border-radius:8px;font-size:15px;font-family:inherit;transition:border-color 0.2s;}'
        . '.form-input:focus{outline:none;border-color:#667eea;}'
        . '.form-input:disabled{background:#f3f4f6;cursor:not-allowed;}'
        . '.btn-primary{width:100%;padding:14px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);color:white;border:none;border-radius:8px;font-size:15px;font-weight:600;cursor:pointer;transition:transform 0.2s,box-shadow 0.2s;}'
        . '.btn-primary:hover:not(:disabled){transform:translateY(-2px);box-shadow:0 10px 20px rgba(102,126,234,0.3);}'
        . '.btn-primary:active:not(:disabled){transform:translateY(0);}'
        . '.btn-primary:disabled{opacity:0.5;cursor:not-allowed;}'
        // Lockout-Timer Animation
        . '@keyframes pulse-red{0%,100%{opacity:1;}50%{opacity:0.6;}}'
        . '.lockout-active{animation:pulse-red 2s infinite;}'
        . '</style></head><body>'
        . '<div class="login-container">'
        . '<div class="login-header">'
        . '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/></svg>'
        . '<h1>Secure Login</h1>'
        . '<p>Access Control Center</p>'
        . '</div>'
        . $timeoutMsg . $failedMsg
        . '<form method="post" action="keks.php" id="login-form">'
        . '<div class="form-group">'
        . '<label class="form-label">Password</label>'
        . '<input type="password" name="password" class="form-input" placeholder="Enter password" required autofocus ' . $formDisabled . '>'
        . '</div>'
        . '<button type="submit" class="btn-primary" id="login-btn" ' . $formDisabled . ' style="' . $btnExtraStyle . '">Sign In</button>'
        . '</form></div>'
        // === LOCKOUT COUNTDOWN JAVASCRIPT ===
        . '<script>'
        . '(function(){'
        . '  var timerEl = document.getElementById("lockout-timer");'
        . '  if (!timerEl) return;'  // Kein Lockout aktiv ? nichts tun
        . '  var seconds = parseInt(timerEl.getAttribute("data-seconds")) || 0;'
        . '  if (seconds <= 0) return;'
        . '  var form = document.getElementById("login-form");'
        . '  var btn = document.getElementById("login-btn");'
        . '  var input = form ? form.querySelector("input[name=password]") : null;'
        // Countdown-Timer aktualisiert jede Sekunde
        . '  var interval = setInterval(function(){'
        . '    seconds--;'
        . '    if (seconds <= 0) {'
        . '      clearInterval(interval);'
        . '      timerEl.textContent = "now";'
        // Formular wieder aktivieren nach Ablauf
        . '      if (input) { input.disabled = false; input.focus(); }'
        . '      if (btn) { btn.disabled = false; btn.style.opacity = "1"; btn.style.cursor = "pointer"; }'
        . '      return;'
        . '    }'
        // Zeitanzeige formatieren (mm:ss)
        . '    var m = Math.floor(seconds / 60);'
        . '    var s = seconds % 60;'
        . '    timerEl.textContent = m + ":" + (s < 10 ? "0" : "") + s;'
        . '  }, 1000);'
        . '})();'
        . '</script>'
        . '</body></html>';
    exit;
}

// ============================================================
// AB HIER: NUR FÜR EINGELOGGTE BENUTZER
// ============================================================

// === DIAGNOSE-DATEN LADEN ===
$statusFile = 'data/status.json';    // Geräte-Statusdaten (von ESP-Nodes gemeldet)
$logFile    = 'data/log.txt';        // System-Log (textbasiert)
$csvFile    = 'data/telemetry.csv';  // Telemetrie-Zeitreihen (RSSI, Heap, etc.)

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
?>

<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <!-- Seitentitel aus Konfiguration -->
    <title><?php echo htmlspecialchars($settings['site_title']); ?></title>
    <!-- CSRF-Token als Meta-Tag für JavaScript-Zugriff -->
    <meta name="csrf-token" content="<?php echo htmlspecialchars($_SESSION['csrf_token'] ?? ''); ?>">
    <!-- Google Fonts: Inter für modernes UI -->
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <!-- Lucide Icons: SVG-Icon-Bibliothek -->
    <script src="https://unpkg.com/lucide@latest"></script>
    <!-- Chart.js: Für Telemetrie-Diagramme -->
    <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.7/dist/chart.umd.min.js"></script>
    
    <style>
        /* ============================================================
           CSS VARIABLEN - Dark Theme Farbpalette
           ============================================================ */
        :root {
            --bg-primary: #0f172a;       /* Haupthintergrund (dunkelblau) */
            --bg-secondary: #1e293b;     /* Karten-Hintergrund */
            --bg-tertiary: #334155;      /* Buttons/Inputs Hintergrund */
            --text-primary: #f1f5f9;     /* Haupttext (hell) */
            --text-secondary: #94a3b8;   /* Sekundärtext (grau) */
            --text-tertiary: #64748b;    /* Tertiärtext (dunkelgrau) */
            --accent-blue: #3b82f6;      /* Primärfarbe (Buttons, Links) */
            --accent-green: #10b981;     /* Erfolg/Online Status */
            --accent-yellow: #f59e0b;    /* Warnung */
            --accent-red: #ef4444;       /* Fehler/Danger */
            --accent-purple: #8b5cf6;    /* Akzent (Kamera) */
            --border-color: #334155;     /* Rahmenfarbe */
            --shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.3);
            --shadow-lg: 0 10px 15px -3px rgba(0, 0, 0, 0.4);
        }
        
        /* === RESET & BASIS === */
        * { margin: 0; padding: 0; box-sizing: border-box; }
        
        body {
            font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
            background: var(--bg-primary);
            color: var(--text-primary);
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            -webkit-font-smoothing: antialiased;
        }
        
        /* ============================================================
           HEADER - Fixierte Navigation oben
           ============================================================ */
        .header {
            background: var(--bg-secondary);
            border-bottom: 1px solid var(--border-color);
            padding: 0 24px;
            height: 64px;
            display: flex;
            align-items: center;
            justify-content: space-between;
            position: sticky;              /* Bleibt beim Scrollen oben */
            top: 0;
            z-index: 100;
            backdrop-filter: blur(8px);    /* Glaseffekt */
        }
        
        .header-left {
            display: flex;
            align-items: center;
            gap: 32px;
        }
        
        /* Logo mit Icon */
        .logo {
            display: flex;
            align-items: center;
            gap: 12px;
            font-size: 18px;
            font-weight: 700;
            color: var(--text-primary);
        }
        
        .logo svg {
            width: 24px;
            height: 24px;
            color: var(--accent-blue);
        }
        
        /* ============================================================
           NAVIGATION - Tab-Buttons
           ============================================================ */
        .nav-tabs {
            display: flex;
            gap: 4px;
        }
        
        .tab-btn {
            background: none;
            border: none;
            color: var(--text-secondary);
            padding: 8px 16px;
            border-radius: 6px;
            cursor: pointer;
            font-size: 14px;
            font-weight: 500;
            transition: all 0.2s;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        
        .tab-btn:hover {
            background: var(--bg-tertiary);
            color: var(--text-primary);
        }
        
        /* Aktiver Tab hervorgehoben */
        .tab-btn.active {
            background: var(--accent-blue);
            color: white;
        }
        
        .tab-btn svg {
            width: 16px;
            height: 16px;
        }
        
        /* Logout-Button im Header */
        .btn-logout {
            padding: 8px 16px;
            background: transparent;
            border: 1px solid var(--border-color);
            border-radius: 6px;
            color: var(--text-secondary);
            text-decoration: none;
            font-size: 14px;
            font-weight: 500;
            transition: all 0.2s;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        
        .btn-logout:hover {
            border-color: var(--accent-red);
            color: var(--accent-red);
        }
        
        .btn-logout svg {
            width: 16px;
            height: 16px;
        }
        
        /* ============================================================
           HAUPTINHALT
           ============================================================ */
        .main-content {
            flex: 1;
            padding: 32px;
            overflow-y: auto;
        }
        
        .container {
            max-width: 1400px;
            margin: 0 auto;
        }
        
        /* Seitenüberschrift */
        .page-header {
            margin-bottom: 32px;
        }
        
        .page-title {
            font-size: 28px;
            font-weight: 700;
            color: var(--text-primary);
            margin-bottom: 8px;
        }
        
        .page-subtitle {
            font-size: 14px;
            color: var(--text-secondary);
        }
        
        /* Grid-Layouts für Karten */
        .grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(350px, 1fr));
            gap: 24px;
        }
        
        .grid-2 {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(500px, 1fr));
            gap: 24px;
        }
        
        /* ============================================================
           KARTEN - Geräte-Status & Inhaltsblöcke
           ============================================================ */
        .card {
            background: var(--bg-secondary);
            border: 1px solid var(--border-color);
            border-radius: 12px;
            padding: 24px;
            transition: all 0.3s;
            position: relative;
            overflow: hidden;
        }
        
        /* Farbiger Streifen oben an der Karte (zeigt Gerätestatus) */
        .card::before {
            content: '';
            position: absolute;
            top: 0;
            left: 0;
            right: 0;
            height: 3px;
            background: var(--border-color);    /* Standard: grau */
            transition: all 0.3s;
        }
        
        /* Farbiger Streifen wenn Gerät online */
        .card.online::before { background: var(--accent-green); }
        .card.sender.online::before { background: var(--accent-blue); }
        .card.receiver.online::before { background: var(--accent-yellow); }
        .card.camera.online::before { background: var(--accent-purple); }
        
        /* Hover-Effekt: leichtes Anheben */
        .card:hover {
            border-color: var(--bg-tertiary);
            transform: translateY(-2px);
            box-shadow: var(--shadow-lg);
        }
        
        .card-header {
            display: flex;
            align-items: center;
            justify-content: space-between;
            margin-bottom: 20px;
        }
        
        .card-title {
            display: flex;
            align-items: center;
            gap: 12px;
            font-size: 16px;
            font-weight: 600;
            color: var(--text-primary);
        }
        
        .card-title svg {
            width: 20px;
            height: 20px;
            color: var(--accent-blue);
        }
        
        /* Online/Offline LED-Anzeige (pulsierender Punkt) */
        .status-indicator {
            width: 10px;
            height: 10px;
            border-radius: 50%;
            background: var(--accent-red);       /* Standard: rot (offline) */
            box-shadow: 0 0 10px currentColor;
            animation: pulse 2s infinite;
        }
        
        .status-indicator.online {
            background: var(--accent-green);     /* Grün wenn online */
        }
        
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
        
        .card-content {
            margin-bottom: 20px;
        }
        
        /* Info-Zeilen (Key-Value Paare in Karten) */
        .info-row {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 8px 0;
            border-bottom: 1px solid var(--border-color);
            font-size: 14px;
        }
        
        .info-row:last-child {
            border-bottom: none;
        }
        
        .info-label {
            color: var(--text-secondary);
            font-weight: 500;
        }
        
        .info-value {
            color: var(--text-primary);
            font-weight: 600;
        }
        
        /* Status-Nachricht (blauer Balken) */
        .status-message {
            padding: 12px 16px;
            background: var(--bg-tertiary);
            border-radius: 8px;
            border-left: 3px solid var(--accent-blue);
            font-size: 14px;
            font-weight: 500;
            margin-bottom: 16px;
        }
        
        /* ============================================================
           BUTTONS
           ============================================================ */
        .btn-group {
            display: flex;
            gap: 8px;
            flex-wrap: wrap;
        }
        
        .btn {
            flex: 1;
            padding: 10px 16px;
            border: 1px solid var(--border-color);
            border-radius: 8px;
            background: var(--bg-tertiary);
            color: var(--text-primary);
            font-size: 13px;
            font-weight: 500;
            cursor: pointer;
            transition: all 0.2s;
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 8px;
            text-decoration: none;
        }
        
        .btn svg {
            width: 16px;
            height: 16px;
        }
        
        .btn:hover {
            background: var(--bg-primary);
            transform: translateY(-1px);
        }
        
        /* Button-Varianten */
        .btn-danger {
            border-color: var(--accent-red);
            color: var(--accent-red);
        }
        
        .btn-danger:hover {
            background: var(--accent-red);
            color: white;
        }
        
        .btn-primary {
            background: var(--accent-blue);
            border-color: var(--accent-blue);
            color: white;
        }
        
        .btn-primary:hover {
            background: #2563eb;
        }
        
        .btn-success {
            background: var(--accent-green);
            border-color: var(--accent-green);
            color: white;
        }
        
        .btn-success:hover {
            background: #059669;
        }
        
        /* ============================================================
           TOGGLE SWITCH (Ein/Aus Schalter)
           ============================================================ */
        .toggle-wrapper {
            flex: 1;
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 8px;
            padding: 10px 16px;
            background: var(--bg-tertiary);
            border: 1px solid var(--border-color);
            border-radius: 8px;
        }
        
        .toggle-wrapper svg {
            width: 16px;
            height: 16px;
            color: var(--text-secondary);
        }
        
        .switch {
            position: relative;
            width: 44px;
            height: 24px;
        }
        
        .switch input {
            opacity: 0;
            width: 0;
            height: 0;
        }
        
        /* Slider-Track */
        .slider {
            position: absolute;
            cursor: pointer;
            top: 0; left: 0; right: 0; bottom: 0;
            background: var(--bg-primary);
            border: 2px solid var(--border-color);
            transition: 0.3s;
            border-radius: 24px;
        }
        
        /* Slider-Knob */
        .slider:before {
            position: absolute;
            content: "";
            height: 16px;
            width: 16px;
            left: 2px;
            bottom: 2px;
            background: var(--text-secondary);
            transition: 0.3s;
            border-radius: 50%;
        }
        
        /* Aktivierter Zustand */
        input:checked + .slider {
            background: var(--accent-yellow);
            border-color: var(--accent-yellow);
        }
        
        input:checked + .slider:before {
            transform: translateX(20px);
            background: white;
        }
        
        /* ============================================================
           TERMINAL - Log-Anzeige im Konsolenstil
           ============================================================ */
        .terminal {
            background: #0a0e1a;
            border: 1px solid var(--border-color);
            border-radius: 8px;
            padding: 16px;
            height: 320px;
            overflow-y: auto;
            font-family: 'Courier New', monospace;
            font-size: 13px;
            color: var(--accent-green);        /* Grüne Schrift wie Terminal */
        }
        
        .terminal-header {
            font-weight: 600;
            color: var(--text-primary);
            margin-bottom: 12px;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        
        .terminal-header svg {
            width: 16px;
            height: 16px;
        }
        
        .log-line {
            padding: 4px 0;
            border-bottom: 1px solid rgba(255,255,255,0.05);
            opacity: 0.8;
        }
        
        .terminal-actions {
            margin-top: 12px;
        }
        
        /* ============================================================
           FORMULARE
           ============================================================ */
        /* View-Sections: Standardmäßig ausgeblendet (per JS gesteuert) */
        .view-section {
            display: none;
        }
        
        .form-group {
            margin-bottom: 20px;
        }
        
        .form-label {
            display: block;
            font-size: 13px;
            font-weight: 500;
            color: var(--text-secondary);
            margin-bottom: 8px;
        }
        
        .form-input,
        .form-select {
            width: 100%;
            padding: 10px 14px;
            background: var(--bg-primary);
            border: 1px solid var(--border-color);
            border-radius: 8px;
            color: var(--text-primary);
            font-size: 14px;
            font-family: inherit;
            transition: border-color 0.2s;
        }
        
        .form-input:focus,
        .form-select:focus {
            outline: none;
            border-color: var(--accent-blue);
        }
        
        .form-hint {
            font-size: 12px;
            color: var(--text-tertiary);
            margin-top: 6px;
        }
        
        /* ============================================================
           ALERT-BOXEN - Benachrichtigungen
           ============================================================ */
        .alert {
            padding: 14px 18px;
            border-radius: 8px;
            margin-bottom: 20px;
            display: flex;
            align-items: center;
            gap: 12px;
            font-size: 14px;
        }
        
        .alert svg {
            width: 20px;
            height: 20px;
            flex-shrink: 0;
        }
        
        .alert-info {
            background: rgba(59, 130, 246, 0.1);
            border: 1px solid var(--accent-blue);
            color: var(--accent-blue);
        }
        
        .alert-warning {
            background: rgba(245, 158, 11, 0.1);
            border: 1px solid var(--accent-yellow);
            color: var(--accent-yellow);
        }
        
        .alert-danger {
            background: rgba(239, 68, 68, 0.1);
            border: 1px solid var(--accent-red);
            color: var(--accent-red);
        }
        
        /* ============================================================
           DIAGNOSE - Statistik-Boxen
           ============================================================ */
        .stats-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 16px;
            margin-bottom: 24px;
        }
        
        .stat-box {
            background: var(--bg-secondary);
            border: 1px solid var(--border-color);
            padding: 20px;
            border-radius: 12px;
            text-align: center;
            border-left: 3px solid var(--accent-blue);
        }
        
        .stat-value {
            font-size: 2rem;
            font-weight: 700;
            color: var(--accent-blue);
            margin: 8px 0;
        }
        
        .stat-label {
            font-size: 13px;
            color: var(--text-secondary);
            font-weight: 500;
        }
        
        /* Chart-Container (feste Höhe für Chart.js) */
        .chart-container {
            position: relative;
            height: 300px;
            width: 100%;
        }
        
        /* Status-Badges (kleine farbige Labels) */
        .status-badge {
            display: inline-block;
            padding: 4px 10px;
            border-radius: 6px;
            font-size: 12px;
            font-weight: 600;
        }
        
        .badge-online  { background: rgba(16, 185, 129, 0.2); color: var(--accent-green); }
        .badge-offline { background: rgba(239, 68, 68, 0.2);  color: var(--accent-red); }
        .badge-good    { background: rgba(16, 185, 129, 0.2); color: var(--accent-green); }
        .badge-warning { background: rgba(245, 158, 11, 0.2); color: var(--accent-yellow); }
        .badge-critical{ background: rgba(239, 68, 68, 0.2);  color: var(--accent-red); }
        
        /* Tabellen-Styling */
        table {
            width: 100%;
            border-collapse: collapse;
        }
        
        th, td {
            text-align: left;
            padding: 12px;
            border-bottom: 1px solid var(--border-color);
            font-size: 14px;
        }
        
        th {
            color: var(--text-secondary);
            font-weight: 600;
            font-size: 13px;
            text-transform: uppercase;
        }
        
        tbody tr:hover {
            background: rgba(255, 255, 255, 0.03);
        }
        
        /* ============================================================
           AUDIT-LOG Einträge
           ============================================================ */
        .log-container {
            background: var(--bg-primary);
            border-radius: 8px;
            padding: 16px;
            max-height: 600px;
            overflow-y: auto;
        }
        
        .log-entry {
            padding: 16px;
            background: var(--bg-secondary);
            border-radius: 8px;
            border-left: 3px solid var(--accent-blue);
            margin-bottom: 12px;
        }
        
        .log-entry-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 8px;
        }
        
        .log-action {
            font-weight: 600;
            font-size: 14px;
        }
        
        .log-time {
            font-size: 12px;
            color: var(--text-tertiary);
        }
        
        .log-details {
            font-size: 13px;
            color: var(--text-secondary);
        }
        
        /* ============================================================
           RESPONSIVE DESIGN
           ============================================================ */
        @media (max-width: 768px) {
            .header-left {
                gap: 16px;
            }
            
            /* Navigation auf Mobile ausblenden */
            .nav-tabs {
                display: none;
            }
            
            /* Grids auf eine Spalte reduzieren */
            .grid, .grid-2 {
                grid-template-columns: 1fr;
            }
        }
    </style>
</head>
<body>

    <!-- ============================================================
         HEADER - Logo, Navigation, Logout
         ============================================================ -->
    <header class="header">
        <div class="header-left">
            <!-- Logo mit Schild-Icon -->
            <div class="logo">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/>
                </svg>
                <span id="site-title"><?php echo htmlspecialchars($settings['site_title']); ?></span>
            </div>
            
            <!-- Tab-Navigation -->
            <nav class="nav-tabs">
                <!-- Dashboard Tab -->
                <button id="btn-dash" class="tab-btn active" onclick="switchView('dashboard')">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="3" width="7" height="7"/><rect x="14" y="3" width="7" height="7"/><rect x="14" y="14" width="7" height="7"/><rect x="3" y="14" width="7" height="7"/></svg>
                    Dashboard
                </button>
                <!-- Logs Tab -->
                <button id="btn-logs" class="tab-btn" onclick="switchView('logs')">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>
                    Logs
                </button>
                <!-- Diagnose Tab -->
                <button id="btn-diag" class="tab-btn" onclick="switchView('diagnose')">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/></svg>
                    Diagnose
                </button>
                <!-- Settings Tab -->
                <button id="btn-conf" class="tab-btn" onclick="switchView('config')">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M12 1v6m0 6v6m-6-6h6m6 0h-6M4.2 4.2l4.2 4.2m7.2 0l4.2-4.2M4.2 19.8l4.2-4.2m7.2 0l4.2 4.2"/></svg>
                    Settings
                </button>
                <!-- Audit Tab (nur sichtbar wenn site_title = 'admin') -->
                <?php if(strtolower($settings['site_title']) === 'admin'): ?>
                <button id="btn-userlogs" class="tab-btn" onclick="switchView('userlogs')">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/></svg>
                    Audit
                </button>
                <?php endif; ?>
            </nav>
        </div>
        
        <!-- Logout-Button -->
        <a href="?logout" class="btn-logout">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/><polyline points="16 17 21 12 16 7"/><line x1="21" y1="12" x2="9" y2="12"/></svg>
            Logout
        </a>
    </header>

    <!-- ============================================================
         HAUPTINHALT - Alle Views/Tabs
         ============================================================ -->
    <main class="main-content">
        <div class="container">
            
            <!-- ========================================================
                 DASHBOARD VIEW - Geräteübersicht & Steuerung
                 ======================================================== -->
            <div id="view-dashboard">
                <div class="page-header">
                    <h1 class="page-title">Network Status</h1>
                    <p class="page-subtitle">Real-time monitoring of connected devices</p>
                </div>
                <div class="grid">
                    
                    <!-- === ESP SENDER KARTE === -->
                    <div id="card-sender" class="card sender">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="22" y1="2" x2="11" y2="13"/><polygon points="22 2 15 22 11 13 2 9 22 2"/></svg>
                                ESP Sender
                            </div>
                            <!-- Status-LED (wird per JS aktualisiert) -->
                            <div id="dot-sender" class="status-indicator"></div>
                        </div>
                        <div class="status-message" id="msg-sender">Waiting for connection...</div>
                        <div class="card-content">
                            <div class="info-row">
                                <span class="info-label">IP Address</span>
                                <span class="info-value" id="ip-sender">---</span>
                            </div>
                        </div>
                        <!-- Steuerungs-Buttons -->
                        <div class="btn-group">
                            <button class="btn btn-danger" onclick="sendCommand('sender', 'REBOOT')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg>
                                Reboot
                            </button>
                            <button class="btn btn-danger" onclick="sendCommand('sender', 'RESET')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="1 4 1 10 7 10"/><path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10"/></svg>
                                Reset
                            </button>
                        </div>
                    </div>

                    <!-- === ESP RECEIVER KARTE === -->
                    <div id="card-receiver" class="card receiver">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="17 11 12 6 7 11"/><polyline points="17 18 12 13 7 18"/></svg>
                                ESP Receiver
                            </div>
                            <div id="dot-receiver" class="status-indicator"></div>
                        </div>
                        <div class="status-message" id="msg-receiver">Waiting for connection...</div>
                        <div class="card-content">
                            <div class="info-row">
                                <span class="info-label">IP Address</span>
                                <span class="info-value" id="ip-receiver">---</span>
                            </div>
                        </div>
                        <div class="btn-group">
                            <!-- Alarm Toggle Switch -->
                            <div class="toggle-wrapper">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9"/><path d="M13.73 21a2 2 0 0 1-3.46 0"/></svg>
                                <label class="switch">
                                    <input type="checkbox" id="alarm-toggle" onchange="toggleAlarm(this)">
                                    <span class="slider"></span>
                                </label>
                            </div>
                            <button class="btn btn-danger" onclick="sendCommand('receiver', 'REBOOT')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg>
                                Reboot
                            </button>
                            <button class="btn btn-danger" onclick="sendCommand('receiver', 'RESET')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="1 4 1 10 7 10"/><path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10"/></svg>
                                Reset
                            </button>
                        </div>
                    </div>

                    <!-- === PICAM KARTE (Raspberry Pi Kamera) === -->
                    <div id="card-camera" class="card camera">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2z"/><circle cx="12" cy="13" r="4"/></svg>
                                PiCam
                            </div>
                            <div id="dot-camera" class="status-indicator online"></div>
                        </div>
                        <div class="status-message" id="msg-camera">Running</div>
                        <div class="card-content">
                            <div class="info-row">
                                <span class="info-label">IP Address</span>
                                <span class="info-value" id="ip-camera">---</span>
                            </div>
                            <div class="info-row">
                                <span class="info-label">CPU Temp</span>
                                <span class="info-value" id="pi-cpu-temp">---</span>
                            </div>
                            <div class="info-row">
                                <span class="info-label">CPU Load</span>
                                <span class="info-value" id="pi-cpu-load">---</span>
                            </div>
                            <div class="info-row">
                                <span class="info-label">Aufnahme</span>
                                <span class="info-value" id="pi-rec-status">---</span>
                            </div>
                        </div>
                        <div class="btn-group">
                            <!-- Kamera-Stream öffnen -->
                            <button class="btn btn-primary" onclick="openCameraStream()">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polygon points="5 3 19 12 5 21 5 3"/></svg>
                                Stream
                            </button>
                            <!-- Aufnahmen-Seite öffnen -->
                            <button class="btn btn-primary" onclick="switchView('recordings')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
                                Aufnahmen
                            </button>
                            <!-- Pi Reboot (mit doppelter Bestätigung) -->
                            <button class="btn btn-danger" onclick="rebootPi()">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg>
                                Reboot
                            </button>
                            <!-- Pi Shutdown (erfordert Alarm-PIN) -->
                            <button class="btn btn-danger" onclick="shutdownPi()">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>
                                Shutdown
                            </button>
                        </div>
                    </div>

                    <!-- === ALARMSTEUERUNG KARTE (Arduino Serial) === -->
                    <div id="card-alarm" class="card">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/></svg>
                                Alarmsteuerung
                            </div>
                            <div id="alarm-serial-status" style="font-size:12px;color:var(--text-secondary);">Arduino</div>
                        </div>
                        <div class="status-message" id="alarm-serial-msg">Bereit</div>
                        <div class="card-content">
                            <div class="info-row">
                                <span class="info-label">Schnittstelle</span>
                                <span class="info-value" id="alarm-serial-port">Auto-Detect</span>
                            </div>
                        </div>
                        <!-- Alarm Aktivieren/Deaktivieren (erfordert PIN) -->
                        <div class="btn-group">
                            <button class="btn btn-success" onclick="sendSerialCmd('1')" style="flex:1;">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/></svg>
                                Aktivieren
                            </button>
                            <button class="btn btn-danger" onclick="sendSerialCmd('0')" style="flex:1;">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>
                                Deaktivieren
                            </button>
                        </div>
                    </div>
                </div>
            </div>

            <!-- ========================================================
                 LOGS VIEW - Live Kommunikations-Terminals
                 ======================================================== -->
            <div id="view-logs" class="view-section">
                <div class="page-header">
                    <h1 class="page-title">Live Communication Logs</h1>
                    <p class="page-subtitle">Real-time device communication streams</p>
                </div>
                <div class="grid">
                    <!-- Sender Terminal -->
                    <div class="card">
                        <div class="terminal-header">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/></svg>
                            ESP Sender Terminal
                        </div>
                        <div id="log-sender" class="terminal"></div>
                        <div class="terminal-actions">
                            <button class="btn btn-danger" onclick="clearLog('log-sender')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                                Clear
                            </button>
                        </div>
                    </div>
                    
                    <!-- Receiver Terminal -->
                    <div class="card">
                        <div class="terminal-header">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/></svg>
                            ESP Receiver Terminal
                        </div>
                        <div id="log-receiver" class="terminal"></div>
                        <div class="terminal-actions">
                            <button class="btn btn-danger" onclick="clearLog('log-receiver')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                                Clear
                            </button>
                        </div>
                    </div>
                    
                    <!-- PiCam Terminal -->
                    <div class="card">
                        <div class="terminal-header">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/></svg>
                            PiCam Terminal
                        </div>
                        <div id="log-camera" class="terminal"></div>
                        <div class="terminal-actions">
                            <button class="btn btn-danger" onclick="clearLog('log-camera')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                                Clear
                            </button>
                        </div>
                    </div>
                </div>
            </div>

            <!-- ========================================================
                 DIAGNOSE VIEW - Telemetrie & System-Überwachung
                 ======================================================== -->
            <div id="view-diagnose" class="view-section">
                <div class="page-header">
                    <h1 class="page-title">System-Diagnose & Telemetrie</h1>
                    <p class="page-subtitle">Detaillierte Systemüberwachung und Performance-Analyse</p>
                </div>
                
                <!-- Auto-Refresh Toggle für Diagnose-Seite -->
                <div class="alert alert-info" style="display: flex; align-items: center; gap: 12px;">
                    <label class="switch">
                        <input type="checkbox" id="diag-auto-refresh" checked>
                        <span class="slider"></span>
                    </label>
                    <span>Auto-Refresh (alle 5 Sek.)</span>
                </div>
                
                <!-- Statistik-Übersicht (4 Boxen) -->
                <div class="stats-grid">
                    <?php 
                    // Online-Geräte zählen (Gerät gilt als online wenn letzter Heartbeat < 30s)
                    $onlineCount = 0;
                    $totalUptime = 0;
                    foreach($diagStatus as $data) {
                        if(isset($data['last_seen']) && time() - $data['last_seen'] < 30) $onlineCount++;
                        if(isset($data['uptime'])) $totalUptime += $data['uptime'];
                    }
                    ?>
                    <div class="stat-box">
                        <div class="stat-label">Online Geräte</div>
                        <div class="stat-value"><?php echo $onlineCount; ?> / <?php echo count($diagStatus); ?></div>
                    </div>
                    <div class="stat-box">
                        <div class="stat-label">Telemetrie-Einträge</div>
                        <div class="stat-value"><?php echo file_exists($csvFile) ? count(file($csvFile)) : 0; ?></div>
                    </div>
                    <div class="stat-box">
                        <div class="stat-label">System Logs</div>
                        <div class="stat-value"><?php echo count($systemLogs); ?></div>
                    </div>
                    <div class="stat-box" style="border-left-color: var(--accent-green);">
                        <div class="stat-label">Gesamt Uptime</div>
                        <div class="stat-value" style="color: var(--accent-green); font-size: 1.5rem;">
                            <?php echo gmdate("H:i:s", $totalUptime); ?>
                        </div>
                    </div>
                </div>
                
                <!-- Geräte-Status Tabelle mit detaillierten Telemetrie-Daten -->
                <div class="card" style="margin-bottom: 24px;">
                    <h3 style="margin-bottom: 16px; display: flex; align-items: center; gap: 8px;">
                        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/></svg>
                        Geräte-Status & Telemetrie
                    </h3>
                    <div style="overflow-x: auto;">
                        <table>
                            <thead>
                                <tr>
                                    <th>Gerät</th>
                                    <th>Status</th>
                                    <th>IP-Adresse</th>
                                    <th>Zuletzt gesehen</th>
                                    <th>Uptime</th>
                                    <th>Reset Grund</th>
                                    <th>Freier RAM</th>
                                    <th>Signal (RSSI)</th>
                                </tr>
                            </thead>
                            <tbody>
                                <?php if($diagStatus): foreach ($diagStatus as $name => $data): 
                                    // Online-Status: letzter Heartbeat < 30 Sekunden
                                    $isOnline = isset($data['last_seen']) && time() - $data['last_seen'] < 30;
                                    // RSSI-Qualität: > -60 = gut, > -75 = mittel, sonst = kritisch
                                    $rssi = $data['rssi'] ?? 0;
                                    $rssiClass = $rssi > -60 ? 'badge-good' : ($rssi > -75 ? 'badge-warning' : 'badge-critical');
                                ?>
                                <tr>
                                    <td style="font-weight:600;">
                                        <?php echo htmlspecialchars(ucfirst($name)); ?>
                                    </td>
                                    <td>
                                        <span class="status-badge <?php echo $isOnline ? 'badge-online' : 'badge-offline'; ?>">
                                            <?php echo $isOnline ? 'ONLINE' : 'OFFLINE'; ?>
                                        </span>
                                    </td>
                                    <td><?php echo htmlspecialchars($data['ip'] ?? 'N/A'); ?></td>
                                    <td style="font-size: 13px;">
                                        <?php 
                                        if(isset($data['last_seen'])) {
                                            $diff = time() - $data['last_seen'];
                                            echo date("d.m.Y H:i:s", $data['last_seen']);
                                            echo " <span style='color: var(--text-tertiary);'>(" . $diff . "s)</span>";
                                        } else {
                                            echo '-';
                                        }
                                        ?>
                                    </td>
                                    <td><?php echo isset($data['uptime']) ? gmdate("H:i:s", $data['uptime']) : '-'; ?></td>
                                    <td>
                                        <!-- Reset-Grund farblich markiert (grün = normal, rot = Watchdog/Crash) -->
                                        <span style="color: <?php echo in_array($data['reset_reason'] ?? '', ['External System', 'Power On']) ? 'var(--accent-green)' : 'var(--accent-red)'; ?>">
                                            <?php echo htmlspecialchars($data['reset_reason'] ?? 'unknown'); ?>
                                        </span>
                                    </td>
                                    <td><?php echo number_format($data['heap'] ?? 0); ?> B</td>
                                    <td>
                                        <span class="status-badge <?php echo $rssiClass; ?>">
                                            <?php echo $rssi; ?> dBm
                                        </span>
                                    </td>
                                </tr>
                                <?php endforeach; else: ?>
                                <tr><td colspan="8" style="text-align: center; color: var(--text-secondary);">Keine Geräte verbunden</td></tr>
                                <?php endif; ?>
                            </tbody>
                        </table>
                    </div>
                </div>
                
                <!-- Telemetrie-Charts (RSSI & Heap) -->
                <div class="grid-2" style="margin-top: 24px;">
                    <!-- RSSI Chart (WLAN-Signalstärke über Zeit) -->
                    <div class="card">
                        <h3 style="margin-bottom: 16px; display: flex; align-items: center; gap: 8px;">
                            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="2"/><path d="M16.24 7.76a6 6 0 0 1 0 8.49m-8.48-.01a6 6 0 0 1 0-8.49m11.31-2.82a10 10 0 0 1 0 14.14m-14.14 0a10 10 0 0 1 0-14.14"/></svg>
                            WLAN-Signal (RSSI)
                        </h3>
                        <div class="chart-container"><canvas id="rssiChart"></canvas></div>
                    </div>
                    <!-- Heap Chart (Verfügbarer RAM über Zeit) -->
                    <div class="card">
                        <h3 style="margin-bottom: 16px; display: flex; align-items: center; gap: 8px;">
                            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="4" y="4" width="16" height="16" rx="2" ry="2"/><rect x="9" y="9" width="6" height="6"/><line x1="9" y1="1" x2="9" y2="4"/><line x1="15" y1="1" x2="15" y2="4"/><line x1="9" y1="20" x2="9" y2="23"/><line x1="15" y1="20" x2="15" y2="23"/><line x1="20" y1="9" x2="23" y2="9"/><line x1="20" y1="14" x2="23" y2="14"/><line x1="1" y1="9" x2="4" y2="9"/><line x1="1" y1="14" x2="4" y2="14"/></svg>
                            Verfügbarer RAM (Heap)
                        </h3>
                        <div class="chart-container"><canvas id="heapChart"></canvas></div>
                    </div>
                </div>
                
                <!-- System-Logs Terminal -->
                <div class="card" style="margin-top: 24px;">
                    <h3 style="margin-bottom: 16px; display: flex; align-items: center; gap: 8px;">
                        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>
                        System-Logs (Letzte 50 Einträge)
                    </h3>
                    <div class="terminal" style="height: 400px;">
                        <?php if(!empty($systemLogs)): ?>
                            <?php foreach($systemLogs as $log): ?>
                                <div class="log-line"><?php echo htmlspecialchars($log); ?></div>
                            <?php endforeach; ?>
                        <?php else: ?>
                            <div style="color: var(--text-secondary); text-align: center; padding: 40px;">
                                Keine Logs verfügbar
                            </div>
                        <?php endif; ?>
                    </div>
                </div>
                
                <!-- Wartung & Daten-Export -->
                <div class="card" style="margin-top: 24px;">
                    <h3 style="margin-bottom: 16px; display: flex; align-items: center; gap: 8px;">
                        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M12 1v6m0 6v6"/></svg>
                        Wartung & Export
                    </h3>
                    <div class="btn-group">
                        <!-- CSV-Export der Telemetrie-Daten -->
                        <a href="api.php?action=export_telemetry" class="btn btn-primary">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
                            Telemetrie exportieren (CSV)
                        </a>
                        <button class="btn btn-danger" onclick="clearTelemetry()">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                            Telemetrie löschen
                        </button>
                        <button class="btn btn-danger" onclick="clearAllLogs()">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                            System-Logs löschen
                        </button>
                    </div>
                </div>
            </div>

            <!-- ========================================================
                 CONFIG VIEW - Dashboard & Node-Konfiguration
                 ======================================================== -->
            <div id="view-config" class="view-section">
                <div class="page-header">
                    <h1 class="page-title">System Configuration</h1>
                    <p class="page-subtitle">Manage dashboard and device settings</p>
                </div>
                <div class="grid">
                    <!-- Dashboard-Einstellungen -->
                    <div class="card">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="3" width="18" height="18" rx="2" ry="2"/><line x1="9" y1="3" x2="9" y2="21"/></svg>
                                Dashboard Settings
                            </div>
                        </div>
                        
                        <!-- Timeout-Status Anzeige -->
                        <div class="alert alert-info" id="timeout-status">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/></svg>
                            <div>
                                <div id="timeout-status-text" style="font-weight: 600;">Auto-Logout: Disabled</div>
                                <div id="timeout-info" style="font-size: 12px; opacity: 0.8; margin-top: 2px;">No automatic logout</div>
                            </div>
                        </div>
                        
                        <!-- Seitentitel -->
                        <div class="form-group">
                            <label class="form-label">Page Title</label>
                            <input type="text" id="cfg-title" class="form-input" value="<?php echo $settings['site_title']; ?>">
                        </div>
                        
                        <!-- Admin-Passwort ändern -->
                        <div class="form-group">
                            <label class="form-label">Change Admin Password</label>
                            <input type="password" id="cfg-pw" class="form-input" placeholder="Enter new password">
                        </div>
                        
                        <!-- Alarm-PIN ändern -->
                        <div class="form-group">
                            <label class="form-label">Alarm-PIN aendern</label>
                            <input type="password" id="cfg-alarm-pin" class="form-input" placeholder="Neuer Alarm-PIN (Standard: CHANGE_ME)">
                            <div class="form-hint">Separater PIN fuer Alarm Aktivieren/Deaktivieren</div>
                        </div>
                        
                        <!-- Aktualisierungsrate -->
                        <div class="form-group">
                            <label class="form-label">Refresh Rate (ms)</label>
                            <input type="number" id="cfg-refresh" class="form-input" value="<?php echo $settings['refresh_rate']; ?>">
                        </div>
                        
                        <!-- Timeout-Dauer -->
                        <div class="form-group">
                            <label class="form-label">Auto-Logout Timeout (Minutes)</label>
                            <input type="number" id="cfg-timeout-min" class="form-input" value="<?php echo $settings['timeout_minutes'] ?? 5; ?>" min="1" max="60">
                            <div class="form-hint">Inactivity period before automatic logout</div>
                        </div>
                        
                        <!-- Timeout aktivieren/deaktivieren -->
                        <div class="form-group">
                            <div class="toggle-wrapper">
                                <span>Enable Auto-Logout</span>
                                <label class="switch">
                                    <input type="checkbox" id="cfg-timeout-active" <?php echo ($settings['timeout_active'] ?? false) ? 'checked' : ''; ?>>
                                    <span class="slider"></span>
                                </label>
                            </div>
                        </div>
                        
                        <!-- Speichern-Button -->
                        <button class="btn btn-primary" onclick="saveConfig()" style="width: 100%;">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/><polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/></svg>
                            Save Settings
                        </button>
                        
                        <!-- Danger Zone: System Reset -->
                        <div class="alert alert-danger" style="margin-top: 24px;">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>
                            <span style="font-weight: 600;">Danger Zone</span>
                        </div>
                        <button class="btn btn-danger" onclick="resetSystem()" style="width: 100%;">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/><line x1="10" y1="11" x2="10" y2="17"/><line x1="14" y1="11" x2="14" y2="17"/></svg>
                            System Reset
                        </button>
                    </div>
                    
                    <!-- ESP Node Remote-Konfiguration -->
                    <div class="card">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="2"/><path d="M16.24 7.76a6 6 0 0 1 0 8.49m-8.48-.01a6 6 0 0 1 0-8.49m11.31-2.82a10 10 0 0 1 0 14.14m-14.14 0a10 10 0 0 1 0-14.14"/></svg>
                                Node Remote Config
                            </div>
                        </div>
                        
                        <div class="alert alert-info">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/><line x1="12" y1="8" x2="12.01" y2="8"/></svg>
                            <span>Only online devices can be configured</span>
                        </div>
                        
                        <!-- Zielgerät auswählen -->
                        <div class="form-group">
                            <label class="form-label">Target Device</label>
                            <select id="conf-target" class="form-select">
                                <option value="" disabled selected>Loading status...</option>
                            </select>
                        </div>
                        
                        <!-- API Server IP für ESP-Nodes -->
                        <div class="form-group">
                            <label class="form-label">API Server IP</label>
                            <input type="text" id="conf-apiip" class="form-input" placeholder="e.g. 192.168.1.50">
                        </div>
                        
                        <!-- Primäres WLAN -->
                        <div class="form-group">
                            <label class="form-label">Primary WLAN SSID</label>
                            <input type="text" id="conf-mssid" class="form-input" placeholder="Network name">
                        </div>
                        
                        <div class="form-group">
                            <label class="form-label">Primary WLAN Password</label>
                            <input type="password" id="conf-mpass" class="form-input" placeholder="Network password">
                        </div>
                        
                        <!-- Backup WLAN (Fallback) -->
                        <div class="form-group">
                            <label class="form-label">Backup WLAN SSID</label>
                            <input type="text" id="conf-bssid" class="form-input" placeholder="Backup network name">
                        </div>
                        
                        <div class="form-group">
                            <label class="form-label">Backup WLAN Password</label>
                            <input type="password" id="conf-bpass" class="form-input" placeholder="Backup password">
                        </div>
                        
                        <!-- Telnet-Zugang für Fernwartung -->
                        <div class="form-group">
                            <label class="form-label">Telnet Password</label>
                            <input type="password" id="conf-telnet" class="form-input" placeholder="Telnet access password">
                        </div>
                        
                        <!-- Konfiguration an ESP senden -->
                        <button class="btn btn-primary" onclick="sendNodeConfig()" style="width: 100%;">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="12" y1="5" x2="12" y2="19"/><polyline points="19 12 12 19 5 12"/></svg>
                            Send to ESP
                        </button>
                    </div>
                </div>
            </div>

            <!-- ========================================================
                 AUDIT LOG VIEW (nur für Admin sichtbar)
                 ======================================================== -->
            <?php if(strtolower($settings['site_title']) === 'admin'): ?>
            <div id="view-userlogs" class="view-section">
                <div class="page-header">
                    <h1 class="page-title">Security Audit Log</h1>
                    <p class="page-subtitle">Administrator activity tracking and monitoring</p>
                </div>
                
                <div class="card" style="margin-bottom: 24px;">
                    <div class="alert alert-danger">
                        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>
                        <div>
                            <div style="font-weight: 600;">Security Protocol Active</div>
                            <div style="font-size: 12px; opacity: 0.8; margin-top: 2px;">All administrative actions are logged and monitored</div>
                        </div>
                    </div>
                    <div class="btn-group">
                        <button class="btn btn-primary" onclick="exportUserLogs()">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
                            Export Logs
                        </button>
                        <button class="btn btn-danger" onclick="clearUserLogs()">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                            Clear Logs
                        </button>
                    </div>
                </div>
                
                <!-- Audit-Log Container (wird per AJAX gefüllt) -->
                <div class="card">
                    <div id="userlog-container" class="log-container">
                        <div style="text-align: center; padding: 40px; color: var(--text-secondary);">
                            <svg style="width: 48px; height: 48px; margin-bottom: 16px; opacity: 0.5;" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="12" y1="2" x2="12" y2="6"/><line x1="12" y1="18" x2="12" y2="22"/><line x1="4.93" y1="4.93" x2="7.76" y2="7.76"/><line x1="16.24" y1="16.24" x2="19.07" y2="19.07"/><line x1="2" y1="12" x2="6" y2="12"/><line x1="18" y1="12" x2="22" y2="12"/><line x1="4.93" y1="19.07" x2="7.76" y2="16.24"/><line x1="16.24" y1="7.76" x2="19.07" y2="4.93"/></svg>
                            <div>Loading audit logs...</div>
                        </div>
                    </div>
                </div>
            </div>

            <!-- ========================================================
                 RECORDINGS VIEW - Alarm-Kameraaufnahmen
                 ======================================================== -->
            <div id="view-recordings" class="view-section">
                <div class="page-header">
                    <h1 class="page-title">Alarm-Aufnahmen</h1>
                    <p class="page-subtitle">Automatische Kameraaufnahmen bei Alarm</p>
                </div>

                <div class="card" style="margin-bottom: 24px;">
                    <!-- Alarm-Monitor Status-Anzeige -->
                    <div class="alert alert-info" id="rec-monitor-status">
                        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/></svg>
                        <div>
                            <div style="font-weight: 600;" id="rec-monitor-text">Alarm-Monitor Status</div>
                            <div style="font-size: 12px; opacity: 0.8; margin-top: 2px;" id="rec-monitor-detail">Lade...</div>
                        </div>
                    </div>
                    <div class="btn-group">
                        <button class="btn btn-primary" onclick="loadRecordings()">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg>
                            Aktualisieren
                        </button>
                        <button class="btn btn-danger" onclick="deleteAllRecordings()">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                            Alle loeschen
                        </button>
                    </div>
                </div>

                <!-- Aufnahmen-Liste (wird per AJAX geladen) -->
                <div class="card">
                    <div id="recordings-container" style="min-height: 100px;">
                        <div style="text-align: center; padding: 40px; color: var(--text-secondary);">
                            Lade Aufnahmen...
                        </div>
                    </div>
                </div>
            </div>

            <?php endif; ?>

        </div>
    </main>


    <!-- ============================================================
         SCRIPT 1: Kritische Funktionen
         ============================================================
         Enthält: CSRF-Setup, API-Helper, Dashboard-Updates,
         Navigation, Gerätsteuerung, Konfiguration.
         Isoliert von Chart-Code damit Button-Fehler unabhängig sind.
         ============================================================ -->
    <script>
        // ============================================================
        // SECURITY: CSRF-Token & Secure API Helper
        // ============================================================
        // CSRF-Token aus Meta-Tag lesen (wurde beim Login generiert)
        var csrfMeta = document.querySelector('meta[name="csrf-token"]');
        var CSRF_TOKEN = csrfMeta ? csrfMeta.content : '';
        // Kamera-Stream Port aus PHP-Einstellungen
        var CAMERA_PORT = <?php echo (int)($settings['camera_port'] ?? 8082); ?>;

        /**
         * apiCall() - Zentraler API-Wrapper mit CSRF-Schutz
         * 
         * Sendet POST-Requests an api.php mit automatischem CSRF-Token.
         * Bei HTTP 403 (Session abgelaufen) ? automatischer Redirect zum Login.
         * 
         * @param {string} action  - API-Aktion (z.B. 'send_command', 'save_settings')
         * @param {Object} params  - Zusätzliche Parameter als Key-Value Paare
         * @returns {Promise}      - Fetch-Response Promise
         */
        function apiCall(action, params) {
            if (!params) params = {};
            params.csrf_token = CSRF_TOKEN;
            console.log('[API]', action, params);
            return fetch('api.php?action=' + action, {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/x-www-form-urlencoded',
                    'X-CSRF-TOKEN': CSRF_TOKEN
                },
                body: new URLSearchParams(params),
                credentials: 'same-origin'    // Cookies mitsenden
            }).then(function(response) {
                console.log('[API Response]', action, response.status);
                // Session abgelaufen ? zum Login weiterleiten
                if (response.status === 403) {
                    window.location.href = 'index.php?timeout=1';
                    return new Promise(function() {});  // Blockiert weitere Verarbeitung
                }
                return response;
            })['catch'](function(err) {
                console.error('[API Error]', action, err);
                throw err;
            });
        }

        // ============================================================
        // GLOBALE VARIABLEN
        // ============================================================
        var refreshRate = <?php echo (int)$settings['refresh_rate']; ?>;  // Dashboard-Aktualisierungsrate in ms
        var updateTimer = null;                                           // Timer-Handle für Dashboard-Loop
        var timeoutActive = <?php echo json_encode($settings['timeout_active'] ?? false); ?>;
        var timeoutMinutes = <?php echo (int)($settings['timeout_minutes'] ?? 5); ?>;

        // ============================================================
        // ACTIVITY TRACKING (für Session-Timeout)
        // ============================================================
        // Sendet bei Benutzeraktivität einen Ping an den Server
        // um die Session am Leben zu halten. Maximal alle 30 Sekunden.
        var lastPing = 0;
        function resetActivityTimer() {
            if (!timeoutActive) return;
            var now = Date.now();
            if (now - lastPing < 30000) return;  // Throttle: max alle 30s
            lastPing = now;
            fetch('api.php?action=ping_activity', {method: 'POST', credentials: 'same-origin'});
        }

        // Aktivitäts-Events registrieren (passiv für Performance)
        ['mousedown', 'keydown', 'scroll', 'touchstart'].forEach(function(event) {
            document.addEventListener(event, resetActivityTimer, {passive: true});
        });

        // ============================================================
        // GERÄTESTEUERUNG - ESP Befehle senden
        // ============================================================
        
        /**
         * sendCommand() - Befehl an ESP-Node senden
         * @param {string} target - 'sender' oder 'receiver'
         * @param {string} cmd    - Befehl: 'REBOOT', 'RESET', 'ALARM_ON', 'ALARM_OFF'
         */
        function sendCommand(target, cmd) {
            if(!confirm('Execute ' + cmd + ' on ' + target + '?')) return;
            console.log('[CMD] Sending', cmd, 'to', target);
            apiCall('send_command', {target: target, cmd: cmd})
                .then(function(r) { return r.json(); })
                .then(function(data) {
                    console.log('[CMD] Response data:', data);
                    alert(data.message || data.error || 'Unknown response');
                })
                ['catch'](function(err) {
                    console.error('[CMD] Error:', err);
                    alert('Request failed: ' + err);
                });
        }

        /**
         * toggleAlarm() - Alarm über ESP Receiver ein/ausschalten
         * @param {HTMLElement} checkbox - Alarm Toggle Checkbox
         */
        function toggleAlarm(checkbox) {
            var cmd = checkbox.checked ? "ALARM_ON" : "ALARM_OFF";
            console.log('[ALARM] Sending', cmd);
            apiCall('send_command', {target: 'receiver', cmd: cmd})
                .then(function(r) { return r.json(); })
                .then(function(data) { console.log('[ALARM] Response:', data); })
                ['catch'](function(err) { console.error('[ALARM] Error:', err); });
        }

        // ============================================================
        // LOG-VERWALTUNG
        // ============================================================
        
        /**
         * clearLog() - Terminal-Log eines Geräts löschen
         * @param {string} id - DOM-ID des Terminal-Elements
         */
        function clearLog(id) {
            var target = '';
            if (id === 'log-sender') target = 'sender';
            if (id === 'log-receiver') target = 'receiver';
            if (id === 'log-camera') target = 'camera';
            if (!target) return;
            if (!confirm(target + ' Logs wirklich loeschen?')) return;
            apiCall('clear_logs', {target: target})
                .then(function(r) { return r.json(); })
                .then(function(data) {
                    document.getElementById(id).innerHTML = '<div style="opacity:0.5;text-align:center;padding:20px;">Log cleared</div>';
                })
                ['catch'](function() { alert('Fehler beim Loeschen'); });
        }

        /** clearTelemetry() - Alle Telemetrie-CSV-Daten löschen (Charts werden zurückgesetzt) */
        function clearTelemetry() {
            if (!confirm('Telemetrie-Daten wirklich loeschen? Charts werden zurueckgesetzt.')) return;
            apiCall('clear_telemetry')
                .then(function(r) { return r.json(); })
                .then(function(data) { alert(data.message || data.error); location.reload(); })
                ['catch'](function() { alert('Fehler beim Loeschen'); });
        }

        /** clearAllLogs() - Alle System-Logs löschen */
        function clearAllLogs() {
            if (!confirm('ALLE System-Logs wirklich loeschen?')) return;
            apiCall('clear_all_logs')
                .then(function(r) { return r.json(); })
                .then(function(data) { alert(data.message || data.error); location.reload(); })
                ['catch'](function() { alert('Fehler beim Loeschen'); });
        }

        // ============================================================
        // KAMERA & RASPBERRY PI STEUERUNG
        // ============================================================
        
        /** openCameraStream() - Kamera-Stream in neuem Tab öffnen */
        function openCameraStream() {
            window.open('stream.php', '_blank');
        }

        /** rebootPi() - Raspberry Pi neustarten (doppelte Bestätigung) */
        function rebootPi() {
            if (!confirm('Raspberry Pi wirklich neustarten?')) return;
            if (!confirm('ACHTUNG: Dashboard wird kurzzeitig nicht erreichbar!')) return;
            apiCall('pi_reboot')
                .then(function(r) { return r.json(); })
                .then(function(data) { alert(data.message || data.error); })
                ['catch'](function() { alert('Request failed'); });
        }

        /** shutdownPi() - Pi komplett herunterfahren (erfordert Alarm-PIN) */
        function shutdownPi() {
            var pin = prompt('Alarm-PIN eingeben um Pi HERUNTERZUFAHREN:');
            if (pin === null || pin === '') return;
            if (!confirm('ACHTUNG: Pi wird KOMPLETT heruntergefahren! Nur durch physischen Neustart wieder erreichbar!')) return;
            apiCall('pi_shutdown', {alarm_pin: pin})
                .then(function(r) { return r.json(); })
                .then(function(data) { alert(data.message || data.error); })
                ['catch'](function() { alert('Request failed'); });
        }

        // ============================================================
        // AUFNAHMEN-VERWALTUNG
        // ============================================================
        
        /** loadRecordings() - Alarm-Aufnahmen laden und anzeigen */
        function loadRecordings() {
            var container = document.getElementById('recordings-container');
            if (!container) return;
            container.innerHTML = '<div style="text-align:center;padding:20px;color:var(--text-secondary);">Lade...</div>';
            
            // Aufnahmen-Liste von API laden
            fetch('api.php?action=get_recordings', {credentials: 'same-origin'})
                .then(function(r) {
                    if (r.status === 403) { window.location.href = 'index.php?timeout=1'; return null; }
                    return r.json();
                })
                .then(function(data) {
                    if (!data) return;
                    var recs = data.recordings || [];
                    if (recs.length === 0) {
                        container.innerHTML = '<div style="text-align:center;padding:40px;color:var(--text-secondary);">Keine Aufnahmen vorhanden</div>';
                        return;
                    }
                    // Aufnahmen-Tabelle generieren
                    var html = '<table style="width:100%;border-collapse:collapse;">';
                    html += '<tr style="border-bottom:1px solid var(--border);"><th style="text-align:left;padding:8px 12px;color:var(--text-secondary);font-size:12px;">Datei</th><th style="text-align:left;padding:8px 12px;color:var(--text-secondary);font-size:12px;">Datum</th><th style="text-align:right;padding:8px 12px;color:var(--text-secondary);font-size:12px;">Groesse</th><th style="padding:8px 12px;"></th></tr>';
                    recs.forEach(function(rec) {
                        html += '<tr style="border-bottom:1px solid rgba(51,65,85,0.3);">';
                        html += '<td style="padding:10px 12px;font-size:13px;">' + rec.name + '</td>';
                        html += '<td style="padding:10px 12px;font-size:13px;color:var(--text-secondary);">' + rec.date + '</td>';
                        html += '<td style="padding:10px 12px;font-size:13px;text-align:right;">' + rec.size_mb + ' MB</td>';
                        html += '<td style="padding:10px 12px;text-align:right;white-space:nowrap;">';
                        html += '<a href="api.php?action=download_recording&file=' + encodeURIComponent(rec.name) + '" class="btn btn-primary" style="padding:4px 10px;font-size:12px;display:inline-block;margin-right:4px;">Download</a>';
                        html += '<button onclick="deleteRecording(\'' + rec.name + '\')" class="btn btn-danger" style="padding:4px 10px;font-size:12px;">Loeschen</button>';
                        html += '</td></tr>';
                    });
                    html += '</table>';
                    container.innerHTML = html;
                })
                ['catch'](function(err) {
                    container.innerHTML = '<div style="text-align:center;padding:20px;color:#ef4444;">Fehler: ' + err + '</div>';
                });
            
            // Alarm-Monitor Status parallel laden
            fetch('api.php?action=get_alarm_status', {credentials: 'same-origin'})
                .then(function(r) { return r.json(); })
                .then(function(data) {
                    var textEl = document.getElementById('rec-monitor-text');
                    var detailEl = document.getElementById('rec-monitor-detail');
                    if (!textEl) return;
                    // Status-Texte zuordnen
                    var stateMap = {
                        'idle': 'Alarm-Monitor aktiv - Bereit',
                        'recording': 'AUFNAHME LAEUFT',
                        'stopped': 'Alarm-Monitor gestoppt',
                        'error': 'Fehler',
                        'not_running': 'Alarm-Monitor nicht gestartet'
                    };
                    textEl.textContent = stateMap[data.state] || data.state;
                    if (data.current_file) {
                        detailEl.textContent = 'Datei: ' + data.current_file;
                    } else if (data.error) {
                        detailEl.textContent = 'Fehler: ' + data.error;
                    } else {
                        detailEl.textContent = data.timestamp ? ('Letztes Update: ' + new Date(data.timestamp).toLocaleTimeString('de-DE')) : 'Warte auf Alarm-Signale...';
                    }
                })
                ['catch'](function() {});
        }

        /**
         * deleteRecording() - Einzelne Aufnahme löschen
         * @param {string} filename - Name der zu löschenden Datei
         */
        function deleteRecording(filename) {
            if (!confirm('Aufnahme "' + filename + '" wirklich loeschen?')) return;
            apiCall('delete_recording', {file: filename})
                .then(function(r) { return r.json(); })
                .then(function(data) {
                    if (data.status === 'ok') loadRecordings();
                    else alert(data.error || 'Fehler');
                })
                ['catch'](function() { alert('Fehler beim Loeschen'); });
        }

        /** deleteAllRecordings() - Alle Aufnahmen löschen */
        function deleteAllRecordings() {
            if (!confirm('ALLE Aufnahmen wirklich loeschen?')) return;
            apiCall('delete_all_recordings')
                .then(function(r) { return r.json(); })
                .then(function(data) { alert(data.message || data.error); loadRecordings(); })
                ['catch'](function() { alert('Fehler'); });
        }

        // ============================================================
        // ARDUINO SERIAL - Alarmanlage über serielle Schnittstelle
        // ============================================================
        
        /**
         * sendSerialCmd() - Befehl über serielle Schnittstelle an Arduino senden
         * @param {string} cmd - '1' = Alarm aktivieren, '0' = Alarm deaktivieren
         */
        function sendSerialCmd(cmd) {
            var label = (cmd === '1') ? 'AKTIVIEREN (Scharfschalten)' : 'DEAKTIVIEREN (Unscharfschalten)';
            // PIN-Abfrage vor Alarm-Steuerung
            var pin = prompt('Alarm-PIN eingeben um ' + label + ':');
            if (pin === null || pin === '') return;
            console.log('[ALARM-SERIAL] Sending', cmd);
            var msgEl = document.getElementById('alarm-serial-msg');
            var portEl = document.getElementById('alarm-serial-port');
            if (msgEl) msgEl.textContent = 'Pruefe PIN...';
            apiCall('serial_send', {cmd: cmd, alarm_pin: pin})
                .then(function(r) { return r.json(); })
                .then(function(data) {
                    console.log('[ALARM-SERIAL] Response:', data);
                    if (data.status === 'ok') {
                        if (msgEl) msgEl.textContent = (cmd === '1') ? 'SCHARF' : 'UNSCHARF';
                        if (portEl && data.port) portEl.textContent = data.port;
                    } else {
                        if (msgEl) msgEl.textContent = 'Fehler';
                    }
                    alert(data.message || data.error);
                })
                ['catch'](function(err) {
                    console.error('[ALARM-SERIAL] Error:', err);
                    if (msgEl) msgEl.textContent = 'Fehler!';
                    alert('Serieller Befehl fehlgeschlagen: ' + err);
                });
        }

        // ============================================================
        // NAVIGATION - Tab-Umschaltung
        // ============================================================
        
        /**
         * switchView() - Zwischen Dashboard/Logs/Diagnose/Config/etc. wechseln
         * @param {string} tabName - Name des Views (z.B. 'dashboard', 'logs')
         */
        function switchView(tabName) {
            // Alle verfügbaren Views
            var views = ['dashboard', 'logs', 'diagnose', 'config', 'userlogs', 'recordings'];
            // Mapping: View-Name ? Button-ID Suffix
            var btnMap = { 'dashboard': 'dash', 'config': 'conf', 'logs': 'logs', 'userlogs': 'userlogs', 'diagnose': 'diag' };
            
            // Alle Views ausblenden und Buttons deaktivieren
            views.forEach(function(v) {
                var viewEl = document.getElementById('view-' + v);
                if (viewEl) viewEl.style.display = 'none';
                var btn = document.getElementById('btn-' + btnMap[v]);
                if (btn) btn.classList.remove('active');
            });
            
            // Gewählten View einblenden und Button aktivieren
            var targetView = document.getElementById('view-' + tabName);
            if (targetView) targetView.style.display = 'block';
            var activeBtn = document.getElementById('btn-' + btnMap[tabName]);
            if (activeBtn) activeBtn.classList.add('active');
            
            // Tab-Auswahl im SessionStorage speichern (überlebt Seiten-Reloads)
            try { sessionStorage.setItem('activeTab', tabName); } catch(e) {}
            
            // View-spezifische Initialisierungen
            if (tabName === 'userlogs') loadUserLogsSimple();
            if (tabName === 'diagnose' && typeof initDiagnoseCharts === 'function') initDiagnoseCharts();
            if (tabName === 'recordings') loadRecordings();
            
            // Activity-Ping senden
            resetActivityTimer();
        }

        // ============================================================
        // KONFIGURATION SPEICHERN
        // ============================================================
        
        /** saveConfig() - Dashboard-Einstellungen an Server senden */
        function saveConfig() {
            var settings = {
                site_title:     document.getElementById('cfg-title').value,
                password:       document.getElementById('cfg-pw').value,
                alarm_pin:      document.getElementById('cfg-alarm-pin').value,
                refresh_rate:   document.getElementById('cfg-refresh').value,
                timeout_active: document.getElementById('cfg-timeout-active').checked,
                timeout_minutes:parseInt(document.getElementById('cfg-timeout-min').value)
            };
            apiCall('save_settings', {settings: JSON.stringify(settings)})
                .then(function(r) { return r.json(); })
                .then(function(data) { alert(data.message || data.error); location.reload(); })
                ['catch'](function() { alert('Request failed'); });
        }

        /** sendNodeConfig() - Remote-Konfiguration an ESP-Node senden */
        function sendNodeConfig() {
            var target = document.getElementById('conf-target').value;
            if (!target) { alert("No device selected!"); return; }
            
            // Nur ausgefüllte Felder senden
            var config = {};
            var api = document.getElementById('conf-apiip').value; if(api) config.apiip = api;
            var mssid = document.getElementById('conf-mssid').value; if(mssid) config.mssid = mssid;
            var mpass = document.getElementById('conf-mpass').value; if(mpass) config.mpass = mpass;
            var bssid = document.getElementById('conf-bssid').value; if(bssid) config.bssid = bssid;
            var bpass = document.getElementById('conf-bpass').value; if(bpass) config.bpass = bpass;
            var telnet = document.getElementById('conf-telnet').value; if(telnet) config.tpass = telnet;
            
            if (Object.keys(config).length === 0) { alert("No fields filled!"); return; }
            if (!confirm('Send configuration to ' + target + '? Device will restart.')) return;
            
            apiCall('save_node_config', {target: target, config: JSON.stringify(config)})
                .then(function(r) { return r.json(); })
                .then(function(data) { alert(data.message || data.error); })
                ['catch'](function() { alert('Request failed'); });
        }

        /** resetSystem() - Alle Daten löschen (doppelte Bestätigung) */
        function resetSystem() {
            if (!confirm("Delete all data?")) return;
            if (!confirm("CONFIRM: Erase all status, logs and commands?")) return;
            apiCall('system_reset')
                .then(function(r) { return r.json(); })
                .then(function(data) { alert(data.message || data.error); location.reload(); })
                ['catch'](function() { alert('Request failed'); });
        }

        // ============================================================
        // TIMEOUT STATUS ANZEIGE
        // ============================================================
        
        /** updateTimeoutStatus() - Zeigt aktuellen Auto-Logout Status in Config */
        function updateTimeoutStatus() {
            var indicator = document.getElementById('timeout-status');
            var statusText = document.getElementById('timeout-status-text');
            var infoText = document.getElementById('timeout-info');
            if (!indicator || !statusText || !infoText) return;
            
            if (timeoutActive) {
                indicator.className = 'alert alert-warning';
                statusText.textContent = 'Auto-Logout: Enabled';
                infoText.textContent = 'Automatic logout after ' + timeoutMinutes + ' minutes of inactivity';
            } else {
                indicator.className = 'alert alert-info';
                statusText.textContent = 'Auto-Logout: Disabled';
                infoText.textContent = 'No automatic logout';
            }
        }

        // ============================================================
        // DASHBOARD UPDATE LOOP
        // ============================================================
        
        /**
         * updateDashboard() - Holt alle aktuellen Daten von der API
         * und aktualisiert Dashboard-Elemente: Gerätestatus, Logs,
         * Konfiguration, Alarm-Toggle, Pi-Temperatur etc.
         */
        function updateDashboard() {
            fetch('api.php?get=all', {credentials: 'same-origin'})
                .then(function(response) {
                    if (response.status === 403) { window.location.href = 'index.php?timeout=1'; return null; }
                    return response.json();
                })
                .then(function(data) {
                    if (!data || !data.status) return;
                    
                    // ESP Sender & Receiver Status aktualisieren
                    updateNode('sender', data.status.sender || null);
                    updateNode('receiver', data.status.receiver || null);
                    
                    // PiCam Status: Kombination aus Camera-ESP und Pi-Daten
                    var camData = data.status.camera ? JSON.parse(JSON.stringify(data.status.camera)) : {};
                    var piData = data.status.pi || {};
                    // Fallback: Pi-IP verwenden wenn Kamera-IP fehlt
                    if (!camData.ip || camData.ip === '0.0.0.0') camData.ip = piData.ip || '---';
                    if (!camData.online && piData.online) {
                        camData.online = true;
                        camData.status = 'Stream bereit';
                    }
                    updateNode('camera', camData);

                    // === Pi Hardware-Metriken ===
                    var tempEl = document.getElementById('pi-cpu-temp');
                    var loadEl = document.getElementById('pi-cpu-load');
                    // CPU-Temperatur mit Farbcodierung (grün < 55°C < gelb < 70°C < rot)
                    if (piData.cpu_temp !== undefined && tempEl) {
                        var temp = parseFloat(piData.cpu_temp);
                        var color = temp > 70 ? '#ef4444' : (temp > 55 ? '#f59e0b' : '#10b981');
                        tempEl.innerHTML = '<span style="color:' + color + '">' + temp.toFixed(1) + ' °C</span>';
                    }
                    if (piData.cpu_load !== undefined && loadEl) {
                        loadEl.textContent = piData.cpu_load;
                    }

                    // === Aufnahme-Status ===
                    var recEl = document.getElementById('pi-rec-status');
                    if (recEl && data.alarm_monitor) {
                        var am = data.alarm_monitor;
                        if (am.state === 'recording') {
                            recEl.innerHTML = '<span style="color:#ef4444;font-weight:600;">? REC</span>';
                        } else if (am.state === 'idle') {
                            recEl.innerHTML = '<span style="color:#10b981;">Bereit</span>';
                        } else if (am.state === 'not_running') {
                            recEl.innerHTML = '<span style="color:#94a3b8;">Inaktiv</span>';
                        } else {
                            recEl.textContent = am.state || '---';
                        }
                    }

                    // === Konfiguration synchronisieren ===
                    if (data.config) {
                        timeoutActive = data.config.timeout_active || false;
                        timeoutMinutes = data.config.timeout_minutes || 5;
                        updateTimeoutStatus();
                    }

                    // === Node Config Dropdown aktualisieren ===
                    var targetSelect = document.getElementById('conf-target');
                    if (targetSelect) {
                        var currentSelection = targetSelect.value;
                        var sOnline = data.status.sender && data.status.sender.online;
                        var rOnline = data.status.receiver && data.status.receiver.online;
                        var newOpts = '<option value="sender">ESP Sender' + (sOnline ? '' : ' (OFFLINE)') + '</option>';
                        newOpts += '<option value="receiver">ESP Receiver' + (rOnline ? '' : ' (OFFLINE)') + '</option>';
                        if (targetSelect.innerHTML !== newOpts) {
                            targetSelect.innerHTML = newOpts;
                            if (currentSelection) targetSelect.value = currentSelection;
                        }
                    }

                    // === Alarm Toggle synchronisieren ===
                    // Nur aktualisieren wenn Benutzer den Switch nicht gerade bedient
                    var alarmSwitch = document.getElementById('alarm-toggle');
                    if (alarmSwitch && data.status.receiver && data.status.receiver.alarm !== undefined && document.activeElement !== alarmSwitch) {
                        alarmSwitch.checked = (data.status.receiver.alarm == true || data.status.receiver.alarm == "1");
                    }

                    // === Live-Logs aktualisieren ===
                    var logSender = document.getElementById('log-sender');
                    var logReceiver = document.getElementById('log-receiver');
                    var logCamera = document.getElementById('log-camera');
                    // Terminals leeren vor Neubefüllung
                    if (logSender) logSender.innerHTML = '';
                    if (logReceiver) logReceiver.innerHTML = '';
                    if (logCamera) logCamera.innerHTML = '';
                    
                    if (data.logs) {
                        // Logs nach Quelle in Terminal verteilen
                        data.logs.forEach(function(line) {
                            var tgt = 'log-sender';  // Default: Sender
                            if (line.indexOf('receiver:') !== -1) tgt = 'log-receiver';
                            if (line.indexOf('camera:') !== -1) tgt = 'log-camera';
                            var div = document.createElement('div');
                            div.className = 'log-line';
                            div.textContent = line;
                            var container = document.getElementById(tgt);
                            if (container) container.appendChild(div);
                        });
                        // Auto-Scroll zu neuesten Einträgen
                        ['log-sender', 'log-receiver', 'log-camera'].forEach(function(id) {
                            var el = document.getElementById(id);
                            if (el) el.scrollTop = el.scrollHeight;
                        });
                    }

                    // Audit-Logs aktualisieren wenn Tab aktiv
                    var ulView = document.getElementById('view-userlogs');
                    if (ulView && ulView.style.display !== 'none') loadUserLogsSimple();
                })
                ['catch'](function(err) { console.error('Dashboard Error:', err); });
        }

        /**
         * updateNode() - Einzelne Gerätekarte aktualisieren
         * @param {string} name - Gerätename ('sender', 'receiver', 'camera')
         * @param {Object} data - Statusdaten {ip, online, status, ...}
         */
        function updateNode(name, data) {
            var card = document.getElementById('card-' + name);
            var ipField = document.getElementById('ip-' + name);
            var msgField = document.getElementById('msg-' + name);
            var dot = document.getElementById('dot-' + name);
            if (!card) return;
            
            if (!data) {
                // Keine Daten ? Offline-Zustand
                card.classList.remove('online');
                if (dot) dot.classList.remove('online');
                return;
            }
            if (ipField) ipField.innerText = data.ip || '---';
            if (data.online) {
                if (msgField) msgField.innerText = data.status || 'Online';
                card.classList.add('online');
                if (dot) dot.classList.add('online');
            } else {
                card.classList.remove('online');
                if (msgField) msgField.innerText = 'Device offline';
                if (dot) dot.classList.remove('online');
            }
        }

        /**
         * startLoop() - Rekursiver Dashboard-Update Loop
         * Verwendet setTimeout statt setInterval für robusteres Timing
         */
        function startLoop() {
            updateDashboard();
            updateTimer = setTimeout(startLoop, refreshRate);
        }

        // ============================================================
        // AUDIT LOG FUNKTIONEN
        // ============================================================
        
        /** loadUserLogsSimple() - Audit-Logs per AJAX laden und rendern */
        function loadUserLogsSimple() {
            var container = document.getElementById('userlog-container');
            if (!container) return;
            fetch('api.php?action=get_user_logs', {credentials: 'same-origin'})
                .then(function(r) { return r.text(); })
                .then(function(text) {
                    var logs;
                    try { logs = JSON.parse(text); } catch(e) {
                        container.innerHTML = '<div style="text-align:center;padding:40px;color:var(--accent-red);">Error parsing log data</div>';
                        return;
                    }
                    if (!logs || logs.length === 0) {
                        container.innerHTML = '<div style="text-align:center;padding:40px;color:var(--text-secondary);">No activity logged</div>';
                        return;
                    }
                    // Maximal 20 Log-Einträge anzeigen
                    var html = '';
                    logs.slice(0, 20).forEach(function(log) {
                        html += '<div class="log-entry">' +
                            '<div class="log-entry-header">' +
                                '<div class="log-action" style="display:flex;align-items:center;gap:8px;">' +
                                    '<span>' + (log.action || '') + '</span>' +
                                '</div>' +
                                '<span class="log-time">' + (log.date || '') + '</span>' +
                            '</div>' +
                            '<div class="log-details">' +
                                'IP: ' + (log.ip || '') + ' | Device: ' + (log.device_name || '') +
                                (log.details ? '<br><span style="color:var(--accent-yellow);">&rarr; ' + log.details + '</span>' : '') +
                            '</div></div>';
                    });
                    container.innerHTML = html;
                })
                ['catch'](function() {
                    container.innerHTML = '<div style="text-align:center;padding:40px;color:var(--accent-red);">Error loading logs</div>';
                });
        }

        /** exportUserLogs() - Audit-Logs als Datei herunterladen */
        function exportUserLogs() { window.location.href = 'api.php?action=export_user_logs'; }

        /** clearUserLogs() - Alle Audit-Logs löschen */
        function clearUserLogs() {
            if (!confirm('Delete all audit logs?')) return;
            apiCall('clear_user_logs')
                .then(function(r) { return r.json(); })
                .then(function(data) { alert(data.message || data.error); loadUserLogsSimple(); })
                ['catch'](function() { alert('Request failed'); });
        }

        // ============================================================
        // INITIALISIERUNG
        // ============================================================
        updateTimeoutStatus();
        
        // Gespeicherten Tab wiederherstellen (nach Seiten-Reload)
        try {
            var savedTab = sessionStorage.getItem('activeTab');
            if (savedTab && savedTab !== 'dashboard') switchView(savedTab);
        } catch(e) {}
        
        // Dashboard-Update Loop starten
        startLoop();
        
        // Lucide Icons initialisieren
        try { if (typeof lucide !== 'undefined') lucide.createIcons(); } catch(e) {}

        console.log('[INIT] All critical functions loaded OK. CSRF token:', CSRF_TOKEN ? 'present (' + CSRF_TOKEN.length + ' chars)' : 'MISSING!');
    </script>

    <!-- ============================================================
         SCRIPT 2: Charts (isoliert)
         ============================================================
         Chart.js Telemetrie-Diagramme sind in einem separaten
         Script-Block um sicherzustellen, dass Chart-Fehler
         KEINE kritischen Funktionen (Buttons, Navigation) brechen.
         ============================================================ -->
    <script>
        // Chart-Instanzen (global für Destroy/Recreate)
        var rssiChart = null, heapChart = null;
        
        /**
         * initDiagnoseCharts() - Telemetrie-Charts initialisieren
         * 
         * Erstellt zwei Chart.js Line-Charts:
         * 1. RSSI Chart: WLAN-Signalstärke über Zeit (alle 3 Geräte)
         * 2. Heap Chart: Verfügbarer RAM über Zeit (alle 3 Geräte)
         * 
         * Verwendet eine gemeinsame Zeitachse (Union aller Timestamps).
         * Fehlende Datenpunkte werden als null dargestellt (Lücken).
         */
        function initDiagnoseCharts() {
            try {
                if (typeof Chart === 'undefined') {
                    console.warn('[CHARTS] Chart.js not loaded');
                    return;
                }

                // Telemetrie-Daten aus PHP (beim Seitenaufbau eingebettet)
                var chartData = <?php echo json_encode($chartData); ?>;
                
                // === Gemeinsame Zeitachse erstellen ===
                // Sammle alle Timestamps aller Geräte in ein sortiertes Array
                var allTimes = {};
                ['sender', 'receiver', 'camera'].forEach(function(src) {
                    chartData[src].time.forEach(function(t) { allTimes[t] = true; });
                });
                var sortedTimes = Object.keys(allTimes).map(Number).sort(function(a,b) { return a-b; });
                
                // Keine Daten ? Platzhalter-Nachricht anzeigen
                if (sortedTimes.length === 0) {
                    console.log('[CHARTS] Keine Telemetrie-Daten vorhanden');
                    var rssiEl = document.getElementById('rssiChart');
                    var heapEl = document.getElementById('heapChart');
                    if (rssiEl) rssiEl.parentElement.innerHTML = '<div style="color:var(--text-secondary);text-align:center;padding:40px;">Keine Telemetrie-Daten vorhanden. Warte auf ESP-Heartbeats...</div>';
                    if (heapEl) heapEl.parentElement.innerHTML = '<div style="color:var(--text-secondary);text-align:center;padding:40px;">Keine Telemetrie-Daten vorhanden.</div>';
                    return;
                }
                
                // Zeitstempel zu lesbaren Labels konvertieren
                var labels = sortedTimes.map(function(t) { return new Date(t * 1000).toLocaleTimeString('de-DE'); });
                
                /**
                 * mapToTimeline() - Gerätedaten auf gemeinsame Zeitachse mappen
                 * @param {Object} srcData - Daten eines Geräts {time:[], rssi:[], heap:[]}
                 * @param {string} field   - Feldname ('rssi' oder 'heap')
                 * @returns {Array}        - Werte aligned mit sortedTimes (null wo Daten fehlen)
                 */
                function mapToTimeline(srcData, field) {
                    var lookup = {};
                    srcData.time.forEach(function(t, i) { lookup[t] = srcData[field][i]; });
                    return sortedTimes.map(function(t) { return (t in lookup) ? lookup[t] : null; });
                }

                // Bestehende Charts zerstören vor Neuerstellen
                if (rssiChart) { rssiChart.destroy(); rssiChart = null; }
                if (heapChart) { heapChart.destroy(); heapChart = null; }

                /**
                 * makeDatasets() - Chart.js Datasets für alle 3 Geräte erstellen
                 * @param {string} field - 'rssi' oder 'heap'
                 * @returns {Array}      - Array von Chart.js Dataset-Objekten
                 */
                function makeDatasets(field) {
                    return [
                        { label: 'Sender',   data: mapToTimeline(chartData.sender, field),   borderColor: '#3b82f6', tension: 0.3, fill: false, spanGaps: true },
                        { label: 'Receiver', data: mapToTimeline(chartData.receiver, field), borderColor: '#f59e0b', tension: 0.3, fill: false, spanGaps: true },
                        { label: 'PiCam',    data: mapToTimeline(chartData.camera, field),   borderColor: '#8b5cf6', tension: 0.3, fill: false, spanGaps: true }
                    ];
                }

                // === RSSI Chart Optionen ===
                var rssiOpts = {
                    responsive: true, maintainAspectRatio: false, animation: false,
                    plugins: { legend: { labels: { color: '#f1f5f9', usePointStyle: true, pointStyle: 'line' } } },
                    scales: {
                        x: { ticks: { color: '#94a3b8', maxTicksLimit: 12, maxRotation: 45 }, grid: { color: 'rgba(51,65,85,0.3)' } },
                        y: { 
                            ticks: { color: '#94a3b8' }, 
                            grid: { color: 'rgba(51,65,85,0.3)' }, 
                            title: { display: true, text: 'dBm', color: '#94a3b8' }, 
                            suggestedMin: -80,   // Typischer RSSI-Bereich
                            suggestedMax: -30 
                        }
                    },
                    elements: { point: { radius: 0, hitRadius: 6 }, line: { borderWidth: 2 } }
                };

                // RSSI Chart erstellen
                rssiChart = new Chart(document.getElementById('rssiChart'), {
                    type: 'line', data: { labels: labels, datasets: makeDatasets('rssi') }, options: rssiOpts
                });

                // === Heap Chart Optionen ===
                var heapOpts = {
                    responsive: true, maintainAspectRatio: false, animation: false,
                    plugins: { legend: { labels: { color: '#f1f5f9', usePointStyle: true, pointStyle: 'line' } } },
                    scales: {
                        x: { ticks: { color: '#94a3b8', maxTicksLimit: 12, maxRotation: 45 }, grid: { color: 'rgba(51,65,85,0.3)' } },
                        y: { 
                            ticks: { 
                                color: '#94a3b8', 
                                // Bytes in KB umrechnen für bessere Lesbarkeit
                                callback: function(v) { return (v/1024).toFixed(1) + ' KB'; } 
                            }, 
                            grid: { color: 'rgba(51,65,85,0.3)' }, 
                            title: { display: true, text: 'Bytes', color: '#94a3b8' } 
                        }
                    },
                    elements: { point: { radius: 0, hitRadius: 6 }, line: { borderWidth: 2 } }
                };

                // Heap Chart erstellen
                heapChart = new Chart(document.getElementById('heapChart'), {
                    type: 'line', data: { labels: labels, datasets: makeDatasets('heap') }, options: heapOpts
                });
                
                console.log('[CHARTS] OK:', sortedTimes.length, 'points');
            } catch(err) {
                console.error('[CHARTS] Error:', err);
            }
            
            // === Auto-Refresh für Diagnose-Seite ===
            var toggle = document.getElementById('diag-auto-refresh');
            var diagInterval = null;
            // Gespeicherten Zustand wiederherstellen
            try {
                var saved = sessionStorage.getItem('diagAutoRefresh');
                if (saved !== null) toggle.checked = (saved === 'true');
            } catch(e) {}
            
            /** startDiagRefresh() - Auto-Refresh Timer starten/stoppen */
            function startDiagRefresh() {
                if (diagInterval) clearInterval(diagInterval);
                // Bei aktiviertem Toggle: Seite alle 5 Sekunden neu laden
                if (toggle.checked) diagInterval = setInterval(function() { location.reload(); }, 5000);
            }
            toggle.addEventListener('change', function() {
                try { sessionStorage.setItem('diagAutoRefresh', this.checked); } catch(e) {}
                startDiagRefresh();
            });
            startDiagRefresh();
        }
        
        console.log('[INIT] Chart module loaded');
        
        // === Auto-Init Check ===
        // Falls der Diagnose-Tab beim Seitenladen bereits aktiv ist
        // (z.B. durch SessionStorage-Restore in Script 1),
        // müssen die Charts hier initialisiert werden, da Script 1
        // switchView() aufgerufen hat BEVOR initDiagnoseCharts() definiert war.
        try {
            var diagView = document.getElementById('view-diagnose');
            if (diagView && diagView.style.display === 'block') {
                console.log('[CHARTS] Diagnose tab active on load - initializing charts');
                initDiagnoseCharts();
            }
        } catch(e) { console.error('[CHARTS] Auto-init error:', e); }
    </script>
</body>
</html>
