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

session_start();

require_once 'includes/config.php';
require_once 'includes/auth.php';

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
    <!-- Icons & Charts -->
    <script src="https://unpkg.com/lucide@latest"></script>
    <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.7/dist/chart.umd.min.js"></script>
    <!-- Dashboard Stylesheet -->
    <link rel="stylesheet" href="css/style.css">
</head>
<body>

<?php require 'views/dashboard.php'; ?>

<!-- PHP-Konfigurationsvariablen für JavaScript -->
<script>
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
