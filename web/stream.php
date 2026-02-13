<?php
session_start();  // Session starten

// Auth pruefen - nur eingeloggte Benutzer duerfen Stream sehen
if (!isset($_SESSION['loggedin']) || $_SESSION['loggedin'] !== true) {  // Nicht eingeloggt
    header('Location: index.php');  // Zur Login-Seite umleiten
    exit;  // Script beenden
}

// Settings laden fuer Camera-Port
$confFile = 'data/settings.json';  // Config-Datei-Pfad
$settings = file_exists($confFile) ? json_decode(file_get_contents($confFile), true) : [];  // Config laden oder leeres Array
$port = (int)($settings['camera_port'] ?? 8082);  // Camera-Port aus Config oder Standard 8082
$piIp = $_SERVER['SERVER_ADDR'] ?? '127.0.0.1';  // Server-IP oder localhost
$streamUrl = 'http://' . $piIp . ':' . $port . '/?action=stream';  // Stream-URL zusammenbauen
?>
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">  <!-- Zeichensatz UTF-8 -->
    <meta name="viewport" content="width=device-width, initial-scale=1.0">  <!-- Responsive Meta-Tag -->
    <title>PiCam Stream</title>  <!-- Seitentitel -->
    <style>
        /* CSS Reset */
        * { margin: 0; padding: 0; box-sizing: border-box; }  /* Alle Elemente ohne Margin/Padding */
        
        /* Body-Styling */
        body {
            font-family: "Inter", -apple-system, sans-serif;  /* Moderne Schriftart */
            background: #0f172a;  /* Dunkler Hintergrund */
            color: #f1f5f9;  /* Helle Textfarbe */
            min-height: 100vh;  /* Minimale Hoehe 100% Viewport */
            display: flex;  /* Flexbox-Layout */
            flex-direction: column;  /* Vertikale Anordnung */
            align-items: center;  /* Horizontal zentriert */
        }
        
        /* Toolbar am oberen Rand */
        .toolbar {
            width: 100%;  /* Volle Breite */
            padding: 12px 20px;  /* Innenabstand */
            background: #1e293b;  /* Dunkler Hintergrund */
            border-bottom: 1px solid #334155;  /* Untere Trennlinie */
            display: flex;  /* Flexbox */
            align-items: center;  /* Vertikal zentriert */
            justify-content: space-between;  /* Links und rechts verteilt */
            gap: 12px;  /* Abstand zwischen Elementen */
        }
        
        /* Links in Toolbar */
        .toolbar a {
            color: #3b82f6;  /* Blauer Link */
            text-decoration: none;  /* Keine Unterstreichung */
            font-size: 14px;  /* Schriftgroesse */
            font-weight: 500;  /* Schriftstaerke */
            display: flex;  /* Flexbox */
            align-items: center;  /* Vertikal zentriert */
            gap: 6px;  /* Abstand zwischen Icon und Text */
        }
        
        /* Link-Hover-Effekt */
        .toolbar a:hover { color: #60a5fa; }  /* Hellblau bei Hover */
        
        /* SVG-Icons in Toolbar */
        .toolbar a svg { width: 18px; height: 18px; }  /* Icon-Groesse */
        
        /* Titel in Toolbar */
        .title { font-size: 16px; font-weight: 600; }  /* Titel-Styling */
        
        /* Stream-Container (zentriert) */
        .stream-container {
            flex: 1;  /* Nimmt restlichen Platz ein */
            display: flex;  /* Flexbox */
            align-items: center;  /* Vertikal zentriert */
            justify-content: center;  /* Horizontal zentriert */
            padding: 16px;  /* Innenabstand */
            width: 100%;  /* Volle Breite */
        }
        
        /* Stream-Bild */
        .stream-container img {
            max-width: 100%;  /* Maximale Breite 100% */
            max-height: calc(100vh - 60px);  /* Maximale Hoehe (minus Toolbar) */
            border-radius: 8px;  /* Abgerundete Ecken */
            border: 1px solid #334155;  /* Rahmen */
        }
        
        /* Fehler-Nachricht */
        .error-msg {
            text-align: center;  /* Zentrierter Text */
            color: #94a3b8;  /* Graue Textfarbe */
            padding: 40px;  /* Innenabstand */
        }
    </style>
</head>
<body>
    <!-- Toolbar mit Navigation -->
    <div class="toolbar">
        <!-- Zurueck-Link zum Dashboard -->
        <a href="index.php">
            <!-- Pfeil-zurueck-Icon (SVG) -->
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="15 18 9 12 15 6"/></svg>
            Zurueck zum Dashboard
        </a>
        <!-- Titel in der Mitte -->
        <span class="title">PiCam Live</span>
        <!-- Direktlink zum Stream (oeffnet in neuem Tab) -->
        <a href="<?php echo htmlspecialchars($streamUrl); ?>" target="_blank">
            <!-- Externes-Link-Icon (SVG) -->
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/><polyline points="15 3 21 3 21 9"/><line x1="10" y1="14" x2="21" y2="3"/></svg>
            Direktlink
        </a>
    </div>
    
    <!-- Stream-Container -->
    <div class="stream-container">
        <!-- Stream-Bild (MJPEG-Stream) -->
        <img src="<?php echo htmlspecialchars($streamUrl); ?>" alt="PiCam Stream"
             onerror="this.style.display='none';this.parentElement.innerHTML='<div class=error-msg>Stream nicht erreichbar.<br>Ist mjpg-streamer gestartet?<br><br><a href=index.php style=color:#3b82f6>Zurueck zum Dashboard</a></div>';">
        <!-- onerror: Bei Fehler wird Bild versteckt und Fehler-Nachricht angezeigt -->
    </div>
</body>
</html>