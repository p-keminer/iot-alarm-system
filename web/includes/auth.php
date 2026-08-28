<?php
// ============================================================
// includes/auth.php - Authentifizierung & Session-Management
// ============================================================
// Enthält:
// - Login-Status aus Session lesen
// - Session-Timeout-Prüfung (Auto-Logout)
// - Manueller Logout mit Audit-Logging
// ============================================================

require_once __DIR__ . '/brute_force.php';

// ============================================================
// LOGIN-STATUS AUS SESSION LESEN (von login.php gesetzt)
// ============================================================
$loginFailed = isset($_SESSION['login_failed']) ? $_SESSION['login_failed'] : false;

// Session-Flags zurücksetzen (nur einmal anzeigen)
unset($_SESSION['login_failed']);
unset($_SESSION['brute_force_status']); // Nicht mehr für Schutzentscheidungen genutzt

// ============================================================
// HOCH-1 FIX: IP-basierter Brute-Force-Status (Session-Rotation-sicher)
// ============================================================
// Vorher: $bruteForceStatus kam aus $_SESSION['brute_force_status'].
//   → Angreifer konnte via neue PHPSESSID den Session-Wert auf null setzen
//   → Login-Formular erschien nicht gesperrt (disabled-Attribut fehlte)
//   → Session-Rotation umging die UI-Sperre vollständig (HOCH-1, Test 21b)
//
// Fix: Für nicht-eingeloggte Requests wird der Status IMMER frisch aus
//   dem absoluten Login-Ledger gelesen (IP-indexed, unabhängig von Session-ID).
//   Eine neue Session ändert die Datei nicht → Bypass unmöglich.
//   Für eingeloggte Nutzer entfällt der File-I/O (nicht relevant).
if (!isset($_SESSION['loggedin']) || $_SESSION['loggedin'] !== true) {
    $bruteForceStatus = checkBruteForce($_SERVER['REMOTE_ADDR'], $bruteForceConfig);
} else {
    $bruteForceStatus = null; // Nicht relevant für eingeloggte Nutzer
}

// ============================================================
// SESSION TIMEOUT CHECK
// ============================================================
if (isset($_SESSION['loggedin']) && $_SESSION['loggedin'] === true) {
    if ($settings['timeout_active'] && isset($_SESSION['last_activity'])) {
        $timeout_seconds = ($settings['timeout_minutes'] ?? 5) * 60;

        if (time() - $_SESSION['last_activity'] > $timeout_seconds) {
            // Timeout erreicht → Auto-Logout durchführen
            $ip     = $_SERVER['REMOTE_ADDR'];
            $agent  = $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown';
            $uaInfo = parseUserAgent($agent);

            $sessionDuration = isset($_SESSION['login_time']) ? (time() - $_SESSION['login_time']) : 0;
            $durationStr     = $sessionDuration > 0 ? gmdate("H:i:s", $sessionDuration) : "0s";

            $newLog = [
                'timestamp'   => time(),
                'date'        => date("d.m.Y H:i:s"),
                'ip'          => $ip,
                'device_name' => $uaInfo['device'] . ' (' . $uaInfo['os'] . ')',
                'browser'     => $uaInfo['browser'],
                'user_agent'  => utf8SafeByteLimit($agent, 256),
                'action'      => 'AUTO-LOGOUT',
                'details'     => "Timeout nach Inaktivität (Sitzungsdauer: $durationStr)"
            ];
            if (!appendUserLogLocked($newLog))
                error_log('AUDIT_LOG_WRITE_FAILED action=AUTO-LOGOUT');

            session_destroy();
            header("Location: index.php?timeout=1");
            exit;
        }
    }
    // Aktivitäts-Timestamp aktualisieren bei jedem Seitenaufruf
    $_SESSION['last_activity'] = time();
}

// ============================================================
// MANUELLER LOGOUT (CSRF-geschützt via POST + CSRF-Token)
// Ein GET-basierter Logout wäre via <img src="?logout"> angreifbar.
// ============================================================
if ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_POST['logout'])) {
    // CSRF-Token prüfen bevor Session-Daten verarbeitet werden
    $provided_token = $_POST['csrf_token'] ?? '';
    $session_token  = $_SESSION['csrf_token'] ?? '';
    if (empty($session_token) || !hash_equals($session_token, $provided_token)) {
        // Ungültiger Token → Redirect ohne Aktion
        header("Location: index.php");
        exit;
    }

    $ip     = $_SERVER['REMOTE_ADDR'];
    $agent  = $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown';
    $uaInfo = parseUserAgent($agent);

    $sessionDuration = isset($_SESSION['login_time']) ? (time() - $_SESSION['login_time']) : 0;
    $durationStr     = $sessionDuration > 0 ? gmdate("H:i:s", $sessionDuration) : "0s";

    $newLog = [
        'timestamp'   => time(),
        'date'        => date("d.m.Y H:i:s"),
        'ip'          => $ip,
        'device_name' => $uaInfo['device'] . ' (' . $uaInfo['os'] . ')',
        'browser'     => $uaInfo['browser'],
        'user_agent'  => utf8SafeByteLimit($agent, 256),
        'action'      => 'LOGOUT',
        'details'     => "Sitzungsdauer: $durationStr"
    ];
    if (!appendUserLogLocked($newLog))
        error_log('AUDIT_LOG_WRITE_FAILED action=LOGOUT');

    session_destroy();
    header("Location: index.php");
    exit;
}
