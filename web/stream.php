<?php
session_start();
// Auth pruefen
if (!isset($_SESSION['loggedin']) || $_SESSION['loggedin'] !== true) {
    header('Location: index.php');
    exit;
}
// Stream wird jetzt über nginx-Proxy bereitgestellt (verhindert Mixed Content)
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
        <img src="<?php echo htmlspecialchars($streamUrl); ?>" alt="PiCam Stream"
             onerror="this.style.display='none';this.parentElement.innerHTML='<div class=error-msg>Stream nicht erreichbar.<br>Ist mjpg-streamer gestartet?<br><br><a href=index.php style=color:#3b82f6>Zurueck zum Dashboard</a></div>';">
    </div>
</body>
</html>