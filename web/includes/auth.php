<?php
// ============================================================
// includes/auth.php - Authentifizierung & Session-Management
// ============================================================
// Enthält:
// - Hilfsfunktionen für Brute-Force-Schutz
// - Login-Status aus Session lesen
// - Session-Timeout-Prüfung (Auto-Logout)
// - Manueller Logout mit Audit-Logging
// ============================================================

// ============================================================
// HILFSFUNKTION: User-Logs laden
// ============================================================
function loadUserLogs() {
    $file = 'data/user_logs.json';
    if (!file_exists($file)) return [];
    $data = json_decode(file_get_contents($file), true);
    return is_array($data) ? $data : [];
}

// ============================================================
// BRUTE-FORCE HILFSFUNKTIONEN
// ============================================================

/**
 * Prüft den Brute-Force-Status für eine IP-Adresse.
 *
 * @param string $ip       - Die zu prüfende IP-Adresse
 * @param array  $bfConfig - Brute-Force-Konfiguration
 * @return array           - [blocked, remaining_attempts, lockout_remaining, total_attempts, tier]
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
            if ($now < $lockoutEnd) {
                return [
                    'blocked'            => true,
                    'remaining_attempts' => 0,
                    'lockout_remaining'  => $lockoutEnd - $now,
                    'total_attempts'     => $totalAttempts,
                    'tier'               => $currentTier
                ];
            }
            break;
        }
    }

    // Nicht gesperrt → berechne verbleibende Versuche bis nächste Stufe
    $nextTierAttempts = $bfConfig['lockout_tiers'][0]['attempts'];
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
 * @param string $ip       - IP-Adresse des Angreifers
 * @param array  $bfConfig - Brute-Force-Konfiguration
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
 * Löscht alle Fehlversuche einer IP nach erfolgreichem Login.
 *
 * @param string $ip       - IP-Adresse
 * @param array  $bfConfig - Brute-Force-Konfiguration
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
// LOGIN-STATUS AUS SESSION LESEN (von keks.php gesetzt)
// ============================================================
$loginFailed       = isset($_SESSION['login_failed'])       ? $_SESSION['login_failed']       : false;
$bruteForceStatus  = isset($_SESSION['brute_force_status']) ? $_SESSION['brute_force_status'] : null;

// Session-Flags zurücksetzen (nur einmal anzeigen)
unset($_SESSION['login_failed']);
unset($_SESSION['brute_force_status']);

// ============================================================
// SESSION TIMEOUT CHECK
// ============================================================
if (isset($_SESSION['loggedin']) && $_SESSION['loggedin'] === true) {
    if ($settings['timeout_active'] && isset($_SESSION['last_activity'])) {
        $timeout_seconds = ($settings['timeout_minutes'] ?? 5) * 60;

        if (time() - $_SESSION['last_activity'] > $timeout_seconds) {
            // Timeout erreicht → Auto-Logout durchführen
            $logs = loadUserLogs();
            $ip     = $_SERVER['REMOTE_ADDR'];
            $agent  = $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown';
            $device = gethostbyaddr($ip);
            if ($device == $ip) $device = "Unknown Device";

            $sessionDuration = isset($_SESSION['login_time']) ? (time() - $_SESSION['login_time']) : 0;
            $durationStr     = $sessionDuration > 0 ? gmdate("H:i:s", $sessionDuration) : "0s";

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
    $logs   = loadUserLogs();
    $ip     = $_SERVER['REMOTE_ADDR'];
    $agent  = $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown';
    $device = gethostbyaddr($ip);
    if ($device == $ip) $device = "Unknown Device";

    $sessionDuration = isset($_SESSION['login_time']) ? (time() - $_SESSION['login_time']) : 0;
    $durationStr     = $sessionDuration > 0 ? gmdate("H:i:s", $sessionDuration) : "0s";

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
