<?php
// ============================================================
// index.php - IoT Control Center Dashboard (Entry Point)
// ============================================================
// Schlanker Einstiegspunkt - delegiert Logik an Include-Dateien:
//
//   includes/config.php  - Einstellungen & Brute-Force-Konfiguration
//   includes/auth.php    - Session, Timeout, Logout, BF-Funktionen
//   includes/data.php    - Diagnose- & Telemetrie-Daten laden
//   views/login.php      - Login-Seite HTML
//   views/dashboard.php  - Dashboard HTML (Header + alle Tabs)
//   css/style.css        - Dashboard Dark-Theme Stylesheet
//   js/app.js            - Kritische JS-Funktionen
//   js/charts.js         - Chart.js Telemetrie-Charts
// ============================================================

// ============================================================
// SESSION-KONFIGURATION (vor session_start!)
// ============================================================
ini_set('session.cookie_httponly', 1);
ini_set('session.cookie_samesite', 'Strict');
ini_set('session.use_strict_mode', 1);
if (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off') {
    ini_set('session.cookie_secure', 1);
}
session_start();

require_once 'includes/config.php';
require_once 'includes/auth.php';

// ============================================================
// SECURITY HEADERS
// ============================================================
// Nonce für Content-Security-Policy: einmaliger zufälliger Wert
// pro Request, der ausschließlich für den einen inline <script>-Block
// in index.php erlaubt ist (PHP-Vars → JS). Kein 'unsafe-inline' nötig.
$csp_nonce = base64_encode(random_bytes(16));

header('X-Content-Type-Options: nosniff');
header('X-Frame-Options: DENY');
header('Cache-Control: no-store, no-cache, must-revalidate');
header('Referrer-Policy: strict-origin-when-cross-origin');
// HSTS: Browser erzwingen HTTPS für 1 Jahr (nur setzen wenn HTTPS aktiv)
if (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off') {
    header('Strict-Transport-Security: max-age=31536000; includeSubDomains');
}
header(
    "Content-Security-Policy: " .
    "default-src 'self'; " .
    // Inline-Script nur mit passendem Nonce erlaubt (der PHP-Vars-Block)
    "script-src 'self' 'nonce-{$csp_nonce}' https://unpkg.com https://cdn.jsdelivr.net; " .
    // Google Fonts CSS + eigene Stylesheets; 'unsafe-inline' für inline styles in dashboard
    "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; " .
    // Google Fonts Schriftdateien
    "font-src https://fonts.gstatic.com; " .
    // Bilder nur von sich selbst + data-URIs (z.B. Chart.js)
    "img-src 'self' data:; " .
    // API-Requests nur zu sich selbst
    "connect-src 'self'; " .
    // Kein Embedding in iFrames (verstärkt X-Frame-Options)
    "frame-ancestors 'none'; " .
    // Formular-Submissions nur an sich selbst (Logout-Form)
    "form-action 'self';"
);

// Login-Seite anzeigen wenn nicht eingeloggt
if (!isset($_SESSION['loggedin']) || $_SESSION['loggedin'] !== true) {
    require 'views/login.php';
    exit;
}

// Diagnose-Daten für eingeloggte Benutzer laden
require_once 'includes/data.php';
?>
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title><?php echo htmlspecialchars($settings['site_title']); ?></title>
    <!-- CSRF-Token als Meta-Tag für JavaScript-Zugriff -->
    <meta name="csrf-token" content="<?php echo htmlspecialchars($_SESSION['csrf_token'] ?? ''); ?>">
    <!-- Google Fonts -->
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <!-- Icons & Charts – feste Versionen + SRI-Hashes (Subresource Integrity).
         Bei CDN-Kompromittierung wird abweichender Code vom Browser blockiert. -->
    <script src="https://unpkg.com/lucide@0.344.0/dist/umd/lucide.min.js"
            integrity="sha384-tTkFttkBclaU1cloKwOi9xk3pbao3VZxTjLNBt8iFABWDBQibbAbWpVmO28zMuxq"
            crossorigin="anonymous"></script>
    <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.7/dist/chart.umd.min.js"
            integrity="sha384-vsrfeLOOY6KuIYKDlmVH5UiBmgIdB1oEf7p01YgWHuqmOHfZr374+odEv96n9tNC"
            crossorigin="anonymous"></script>
    <!-- Dashboard Stylesheet -->
    <link rel="stylesheet" href="css/style.css">
</head>
<body>

<?php require 'views/dashboard.php'; ?>

<!-- PHP-Konfigurationsvariablen für JavaScript (Nonce erforderlich durch CSP) -->
<script nonce="<?php echo htmlspecialchars($csp_nonce, ENT_QUOTES, 'UTF-8'); ?>">
var CAMERA_PORT    = <?php echo (int)($settings['camera_port'] ?? 8082); ?>;
var refreshRate    = <?php echo (int)$settings['refresh_rate']; ?>;
var timeoutActive  = <?php echo json_encode($settings['timeout_active'] ?? false); ?>;
var timeoutMinutes = <?php echo (int)($settings['timeout_minutes'] ?? 5); ?>;
var chartData      = <?php echo json_encode($chartData); ?>;
</script>

<!-- Dashboard Logik -->
<script src="js/app.js"></script>
<!-- Telemetrie-Charts (isoliert damit Chart-Fehler keine Buttons brechen) -->
<script src="js/charts.js"></script>

</body>
</html>
