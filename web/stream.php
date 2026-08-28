<?php
// Session-Sicherheits-Flags (identisch zu index.php – müssen vor session_start() stehen)
ini_set('session.cookie_httponly', 1);
ini_set('session.cookie_samesite', 'Strict');
ini_set('session.use_strict_mode', 1);
if (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off') {
    ini_set('session.cookie_secure', 1);
}
session_start();

// Gleiche zentrale Auth-/Timeout-Logik wie im Dashboard nutzen.
require_once __DIR__ . '/includes/config.php';
require_once __DIR__ . '/includes/auth.php';

// Security Headers
header('X-Content-Type-Options: nosniff');
header('X-Frame-Options: DENY');
header('Cache-Control: no-store, no-cache, must-revalidate');
header('Referrer-Policy: strict-origin-when-cross-origin');
if (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off') {
    header('Strict-Transport-Security: max-age=31536000; includeSubDomains');
}
// Nonce für CSP (verhindert unbefugte Inline-Scripts)
$stream_nonce = base64_encode(random_bytes(16));
header(
    "Content-Security-Policy: " .
    "default-src 'self'; " .
    "script-src 'self' 'nonce-{$stream_nonce}'; " .
    "style-src 'self' 'unsafe-inline'; " .
    "img-src 'self' data:; " .
    "connect-src 'self'; " .
    "frame-ancestors 'none';"
);

// Auth pruefen
if (!isset($_SESSION['loggedin']) || $_SESSION['loggedin'] !== true) {
    header('Location: index.php');
    exit;
}
// Stream wird jetzt ueber den nginx-Proxy bereitgestellt (verhindert Mixed Content)
$streamUrl = '/cam/?action=stream';
?>
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>PiCam Stream</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: "Inter", -apple-system, sans-serif;
            background: #0f172a;
            color: #f1f5f9;
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            align-items: center;
        }
        .toolbar {
            width: 100%;
            padding: 12px 20px;
            background: #1e293b;
            border-bottom: 1px solid #334155;
            display: flex;
            align-items: center;
            justify-content: space-between;
            gap: 12px;
        }
        .toolbar a {
            color: #3b82f6;
            text-decoration: none;
            font-size: 14px;
            font-weight: 500;
            display: flex;
            align-items: center;
            gap: 6px;
        }
        .toolbar a:hover { color: #60a5fa; }
        .toolbar a svg { width: 18px; height: 18px; }
        .title { font-size: 16px; font-weight: 600; }
        .stream-container {
            flex: 1;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 16px;
            width: 100%;
        }
        .stream-container img {
            max-width: 100%;
            max-height: calc(100vh - 60px);
            border-radius: 8px;
            border: 1px solid #334155;
        }
        .error-msg {
            text-align: center;
            color: #94a3b8;
            padding: 40px;
        }
    </style>
</head>
<body>
    <div class="toolbar">
        <a href="index.php">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="15 18 9 12 15 6"/></svg>
            Zurueck zum Dashboard
        </a>
        <span class="title">PiCam Live</span>
        <a href="<?php echo htmlspecialchars($streamUrl); ?>" target="_blank">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/><polyline points="15 3 21 3 21 9"/><line x1="10" y1="14" x2="21" y2="3"/></svg>
            Direktlink
        </a>
    </div>
    <div class="stream-container">
        <!-- onerror als Inline-Attribut wäre durch CSP blockiert → separater Script-Block mit Nonce -->
        <img id="stream-img" src="<?php echo htmlspecialchars($streamUrl); ?>" alt="PiCam Stream">
    </div>
    <script nonce="<?php echo htmlspecialchars($stream_nonce, ENT_QUOTES, 'UTF-8'); ?>">
    document.getElementById('stream-img').addEventListener('error', function() {
        this.style.display = 'none';
        var msg = document.createElement('div');
        msg.className = 'error-msg';
        msg.innerHTML = 'Stream nicht erreichbar.<br>Ist mjpg-streamer gestartet?<br><br>' +
            '<a href="index.php" style="color:#3b82f6">Zurueck zum Dashboard</a>';
        this.parentElement.appendChild(msg);
    });
    </script>
</body>
</html>
