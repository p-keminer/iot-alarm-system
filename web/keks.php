<?php
// ============================================================
// keks.php - Login Handler für IoT Control Center
// ============================================================
// Verarbeitet Login-Anfragen mit:
// - BCrypt Passwort-Verifizierung
// - Progressiver Brute-Force-Schutz
// - Session Regeneration (Session-Fixation-Schutz)
// - Audit-Logging
// ============================================================

session_start();

// Nur POST-Requests erlauben
if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    header('Location: index.php');
    exit;
}

// Helper-Funktion für User Logs
function loadUserLogs() {
    $file = 'data/user_logs.json';
    if (!file_exists($file)) return [];
    $data = json_decode(file_get_contents($file), true);
    return is_array($data) ? $data : [];
}

// ============================================================
// EINSTELLUNGEN LADEN
// ============================================================
$confFile = 'data/settings.json';
$settings = [];
if (file_exists($confFile)) {
    $settings = json_decode(file_get_contents($confFile), true);
}

if (!$settings || !isset($settings['password'])) {
    // Fallback zu Standardwerten falls Config fehlt
    $settings = [
        "password" => password_hash("CHANGE_ME", PASSWORD_BCRYPT),
        "timeout_active" => true,
        "timeout_minutes" => 5
    ];
}

// ============================================================
// BRUTE-FORCE SCHUTZ - Konfiguration
// ============================================================
$bruteForceConfig = [
    'lockout_tiers' => [
        ['attempts' => 5,  'lockout_seconds' => 300],   // 5 Versuche → 5 Min Sperre
        ['attempts' => 10, 'lockout_seconds' => 900],   // 10 Versuche → 15 Min Sperre
        ['attempts' => 15, 'lockout_seconds' => 3600],  // 15 Versuche → 60 Min Sperre
    ],
    'attempt_window' => 3600,
    'file' => 'data/login_attempts.json'
];

// ============================================================
// BRUTE-FORCE HILFSFUNKTIONEN
// ============================================================

/**
 * Prüft den Brute-Force-Status für eine IP-Adresse
 */
function checkBruteForce($ip, $bfConfig) {
    $attemptFile = $bfConfig['file'];
    $now = time();
    
    $attempts = [];
    if (file_exists($attemptFile)) {
        $attempts = json_decode(file_get_contents($attemptFile), true) ?: [];
    }
    
    if (!isset($attempts[$ip])) {
        return [
            'blocked' => false,
            'remaining_attempts' => $bfConfig['lockout_tiers'][0]['attempts'],
            'lockout_remaining' => 0,
            'total_attempts' => 0,
            'tier' => 0
        ];
    }
    
    $ipAttempts = $attempts[$ip];
    
    // Alte Versuche filtern (älter als attempt_window)
    $recentAttempts = array_filter($ipAttempts, function($attempt) use ($now, $bfConfig) {
        return ($now - $attempt['time']) < $bfConfig['attempt_window'];
    });
    
    $totalAttempts = count($recentAttempts);
    
    // Prüfe welche Sperr-Stufe gilt
    $currentTier = 0;
    $blocked = false;
    $lockoutRemaining = 0;
    
    foreach ($bfConfig['lockout_tiers'] as $tierIndex => $tier) {
        if ($totalAttempts >= $tier['attempts']) {
            $currentTier = $tierIndex;
            $lastAttemptTime = max(array_column($recentAttempts, 'time'));
            $timeSinceLastAttempt = $now - $lastAttemptTime;
            
            if ($timeSinceLastAttempt < $tier['lockout_seconds']) {
                $blocked = true;
                $lockoutRemaining = $tier['lockout_seconds'] - $timeSinceLastAttempt;
                break;
            }
        }
    }
    
    // Nächste Stufe ermitteln
    $nextTier = $currentTier + 1;
    if ($nextTier >= count($bfConfig['lockout_tiers'])) {
        $nextTier = count($bfConfig['lockout_tiers']) - 1;
    }
    
    $remainingAttempts = $bfConfig['lockout_tiers'][$nextTier]['attempts'] - $totalAttempts;
    if ($remainingAttempts < 0) $remainingAttempts = 0;
    
    return [
        'blocked' => $blocked,
        'remaining_attempts' => $remainingAttempts,
        'lockout_remaining' => $lockoutRemaining,
        'total_attempts' => $totalAttempts,
        'tier' => $currentTier
    ];
}

/**
 * Registriert einen fehlgeschlagenen Login-Versuch
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
    
    $attempts[$ip][] = [
        'time'  => time(),
        'agent' => $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown'
    ];
    
    if (!is_dir('data')) mkdir('data', 0750, true);
    file_put_contents($attemptFile, json_encode($attempts));
}

/**
 * Löscht alle Fehlversuche einer IP nach erfolgreichem Login
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

if (!isset($_POST['password'])) {
    header('Location: index.php');
    exit;
}

$ip = $_SERVER['REMOTE_ADDR'];

// Brute-Force-Status prüfen BEVOR Passwort verifiziert wird
$bruteForceStatus = checkBruteForce($ip, $bruteForceConfig);

if ($bruteForceStatus['blocked']) {
    // IP ist gesperrt → zurück zum Login mit Fehler
    $_SESSION['login_failed'] = true;
    $_SESSION['brute_force_status'] = $bruteForceStatus;
    header('Location: index.php');
    exit;
}

if (password_verify($_POST['password'], $settings['password'])) {
    // === ERFOLGREICHER LOGIN ===
    
    // Session-ID regenerieren (Session-Fixation-Schutz)
    session_regenerate_id(true);
    
    // Session-Variablen setzen
    $_SESSION['loggedin']      = true;
    $_SESSION['last_activity'] = time();
    $_SESSION['login_time']    = time();
    $_SESSION['csrf_token']    = bin2hex(random_bytes(32));
    
    // Fehlversuche für diese IP löschen
    clearFailedAttempts($ip, $bruteForceConfig);
    
    // Login in Audit-Log schreiben
    $agent = $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown';
    $device = gethostbyaddr($ip);
    if ($device == $ip) $device = "Unknown Device";
    
    $logs = loadUserLogs();
    array_unshift($logs, [
        'timestamp'   => time(),
        'date'        => date("d.m.Y H:i:s"),
        'ip'          => $ip,
        'device_name' => $device,
        'user_agent'  => $agent,
        'action'      => 'LOGIN',
        'details'     => 'Erfolgreicher Login',
        'session_id'  => session_id()
    ]);
    
    $logs = array_slice($logs, 0, 100);
    if (!is_dir('data')) mkdir('data', 0750, true);
    file_put_contents("data/user_logs.json", json_encode($logs));
    
    // Erfolgreich → zum Dashboard
    header('Location: index.php');
    exit;
    
} else {
    // === FEHLGESCHLAGENER LOGIN ===
    recordFailedAttempt($ip, $bruteForceConfig);
    
    // Status nach dem neuen Fehlversuch aktualisieren
    $bruteForceStatus = checkBruteForce($ip, $bruteForceConfig);
    
    // Zurück zum Login mit Fehler
    $_SESSION['login_failed'] = true;
    $_SESSION['brute_force_status'] = $bruteForceStatus;
    header('Location: index.php');
    exit;
}
?>
