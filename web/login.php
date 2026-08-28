<?php
// ============================================================
// login.php - Login Handler für IoT Control Center
// ============================================================
// Verarbeitet Login-Anfragen mit:
// - BCrypt Passwort-Verifizierung
// - Progressiver Brute-Force-Schutz
// - Session Regeneration (Session-Fixation-Schutz)
// - Audit-Logging
// ============================================================

// ============================================================
// SECURITY HEADERS (vor jeder Ausgabe setzen)
// ============================================================
header('X-Content-Type-Options: nosniff');
header('X-Frame-Options: DENY');
header('Cache-Control: no-store, no-cache, must-revalidate');

// ============================================================
// SESSION-KONFIGURATION (vor session_start!)
// Sicherheits-Flags müssen vor dem ersten session_start() gesetzt
// werden, damit der Set-Cookie Header sie enthält.
// ============================================================
ini_set('session.cookie_httponly', 1);
ini_set('session.cookie_samesite', 'Strict');
ini_set('session.use_strict_mode', 1);
if (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off') {
    ini_set('session.cookie_secure', 1);
}
session_start();

require_once __DIR__ . '/includes/config.php';
require_once __DIR__ . '/includes/brute_force.php';

// Nur POST-Requests erlauben
if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    header('Location: index.php');
    exit;
}

// ============================================================
// LOGIN-VERARBEITUNG
// ============================================================

if (!isset($_POST['password'])) {
    header('Location: index.php');
    exit;
}

$ip = $_SERVER['REMOTE_ADDR'];

// BCrypt verarbeitet intern nur die ersten 72 Bytes → Eingabe begrenzen.
$password = substr($_POST['password'] ?? '', 0, 72);

// Sperrprüfung, BCrypt-Verifikation und Record/Clear laufen unter demselben
// exklusiven Ledger-Lock. So können parallele Requests keinen Versuch verlieren.
$bruteForceStatus = authenticateWithBruteForce(
    $ip,
    $password,
    $settings['password'],
    $bruteForceConfig
);

if (!$bruteForceStatus['password_checked']) {
    // Fail2ban-Feed: PHP-Layer-Sperre in Apache-Error-Log schreiben,
    // damit Fail2ban die IP auch auf OS-Ebene per iptables sperren kann.
    // Format: "FAIL_LOGIN from <IP>" ist der Regex-Anker im Filter.
    if (!empty($bruteForceStatus['storage_error'])) {
        error_log("AUTH_STORAGE_ERROR for " . $ip . " (Login fail-closed)");
    } else {
        error_log("FAIL_LOGIN from " . $ip . " (geblockt, Tier " . $bruteForceStatus['tier'] . ")");
    }

    // IP ist gesperrt → zurück zum Login mit Fehler
    $_SESSION['login_failed'] = true;
    $_SESSION['brute_force_status'] = $bruteForceStatus;
    header('Location: index.php');
    exit;
}

if ($bruteForceStatus['authenticated']) {
    // === ERFOLGREICHER LOGIN ===
    
    // Session-ID regenerieren (Session-Fixation-Schutz)
    session_regenerate_id(true);
    
    // Session-Variablen setzen
    $_SESSION['loggedin']      = true;
    $_SESSION['last_activity'] = time();
    $_SESSION['login_time']    = time();
    $_SESSION['csrf_token']    = bin2hex(random_bytes(32));
    
    // Login in Audit-Log schreiben
    $agent  = $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown';
    // User-Agent parsen: parseUserAgent() aus includes/brute_force.php
    $uaInfo = parseUserAgent($agent);

    $loginLog = [
        'timestamp'   => time(),
        'date'        => date("d.m.Y H:i:s"),
        'ip'          => $ip,
        'device_name' => $uaInfo['device'] . ' (' . $uaInfo['os'] . ')',
        'browser'     => $uaInfo['browser'],
        'user_agent'  => utf8SafeByteLimit($agent, 256),
        'action'      => 'LOGIN',
        'details'     => 'Erfolgreicher Login'
    ];
    if (!appendUserLogLocked($loginLog))
        error_log('AUDIT_LOG_WRITE_FAILED action=LOGIN');
    
    // Erfolgreich → zum Dashboard
    header('Location: index.php');
    exit;
    
} else {
    // === FEHLGESCHLAGENER LOGIN ===
    // Fail2ban-Feed: Fehlversuch in PHP-FPM-Log schreiben.
    // Fail2ban parst /var/log/fpm-php.www.log nach diesem Muster
    // und sperrt die IP per iptables nach maxretry Treffern.
    error_log("FAIL_LOGIN from " . $ip);

    // Zurück zum Login mit Fehler
    $_SESSION['login_failed'] = true;
    $_SESSION['brute_force_status'] = $bruteForceStatus;
    header('Location: index.php');
    exit;
}
?>
