<?php

// api.php - V5.3 (Security Hardened + File Locking + SD-Card Safe)

// Sicherheits-Header setzen gegen verschiedene Angriffe
header('X-Content-Type-Options: nosniff');  // Verhindert MIME-Type-Sniffing
header('X-Frame-Options: DENY');            // Verhindert Einbettung in iFrames
header('X-XSS-Protection: 1; mode=block');  // XSS-Filter im Browser aktivieren
header('Cache-Control: no-store, no-cache, must-revalidate');  // Keine Cache-Speicherung

// Datenverzeichnis definieren und erstellen falls nicht vorhanden
$dataDir = 'data/';  // Hauptverzeichnis fuer alle Daten
if (!is_dir($dataDir)) mkdir($dataDir, 0750, true);  // Verzeichnis mit Rechten 0750 erstellen

// Dateipfade definieren
$statusFile   = $dataDir . 'status.json';      // ESP-Status (sender, receiver, camera)
$logFile      = $dataDir . 'log.txt';          // System-Logs
$userLogFile  = $dataDir . 'user_logs.json';   // Benutzeraktivitaeten
$cmdFile      = $dataDir . 'commands.json';    // Ausstehende Befehle fuer ESPs
$confFile     = $dataDir . 'settings.json';    // Dashboard-Konfiguration
$rateLimitDir = $dataDir . 'ratelimit/';       // Rate-Limiting-Daten pro IP

// Rate-Limit-Verzeichnis erstellen
if (!is_dir($rateLimitDir)) mkdir($rateLimitDir, 0750, true);

// Leere JSON-Dateien erstellen falls nicht vorhanden
if (!file_exists($statusFile))  file_put_contents($statusFile, json_encode([]));   // Leeres Array
if (!file_exists($cmdFile))     file_put_contents($cmdFile, json_encode([]));      // Leeres Array
if (!file_exists($userLogFile)) file_put_contents($userLogFile, json_encode([]));  // Leeres Array

// Standard-Konfiguration erstellen falls nicht vorhanden
if (!file_exists($confFile)) {
    $defaults = [
        "password"        => password_hash("CHANGE_ME", PASSWORD_BCRYPT),  // Dashboard-Passwort (gehashed)
        "refresh_rate"    => 2000,                                         // Dashboard-Refresh in ms
        "site_title"      => "IoT-AlarmSystem",                            // Seiten-Titel
        "timeout_active"  => true,                                         // Session-Timeout aktiv
        "timeout_minutes" => 5,                                            // Timeout-Dauer
        "esp_token"       => bin2hex(random_bytes(16)),                    // ESP-Authentifizierungs-Token
        "camera_port"     => 8082,                                         // mjpg-streamer Port
        "alarm_pin"       => password_hash("CHANGE_ME", PASSWORD_BCRYPT)   // Alarm-PIN (gehashed)
    ];
    file_put_contents($confFile, json_encode($defaults));  // Standard-Config speichern
}

// ============================================================
// SECURITY & LOCKING HELPERS
// ============================================================

/**
 * Sicheres JSON-Lesen mit Shared Lock (LOCK_SH)
 * Respektiert Write-Locks von Python/PHP
 */
function readJsonLocked($file) {
    if (!file_exists($file)) return [];  // Datei existiert nicht

    $fp = fopen($file, 'r');  // Datei zum Lesen oeffnen
    if (!$fp) return [];      // Oeffnen fehlgeschlagen

    $data = [];               // Leeres Array als Fallback
    if (flock($fp, LOCK_SH)) { // Shared Lock (Mehrere Leser erlaubt, blockiert bei Writer)
        $size = filesize($file);  // Dateigroesse ermitteln
        if ($size > 0) {          // Datei nicht leer
            $json = fread($fp, $size);  // Komplette Datei einlesen
            $data = json_decode($json, true);  // JSON in Array konvertieren
        }
        flock($fp, LOCK_UN);  // Lock freigeben
    }
    fclose($fp);  // Datei schliessen

    return is_array($data) ? $data : [];  // Array zurueckgeben oder leeres Array
}

/**
 * Sicheres JSON-Schreiben mit Exlusive Lock (LOCK_EX)
 * Leert die Datei erst, wenn der Lock steht.
 * fsync() erzwingt Schreiben auf SD-Karte (Stromausfall-Schutz).
 */
function writeJsonLocked($file, $data) {
    $fp = fopen($file, 'c+'); // c+ = Lesen & Schreiben, Zeiger am Anfang (Datei wird erstellt falls nicht vorhanden)
    if ($fp) {                // Oeffnen erfolgreich
        if (flock($fp, LOCK_EX)) { // Exklusiver Lock (Nur ein Writer, blockiert alle Reader)
            ftruncate($fp, 0);     // Datei auf 0 Bytes leeren (WICHTIG: Erst nach Lock!)
            rewind($fp);           // Dateizeiger an den Anfang setzen
            fwrite($fp, json_encode($data));  // JSON in Datei schreiben
            fflush($fp);           // Puffer leeren (PHP-Puffer -> OS-Puffer)
            
            // ECHTE PERSISTENZ (Wichtig fuer SD-Karten und Stromausfall-Sicherheit)
            if (function_exists('fsync')) {  // fsync() nur ab PHP 8.1 verfuegbar
                fsync($fp);        // OS-Puffer -> Physische Disk (Erzwingt Schreiben)
            }

            flock($fp, LOCK_UN);   // Lock freigeben
        }
        fclose($fp);  // Datei schliessen
    }
}

// Prueft ob Benutzer eingeloggt ist und Session nicht abgelaufen
function requireAuth($dieOnFail = true) {
    if (isset($_SESSION['loggedin']) && $_SESSION['loggedin'] === true) {  // Benutzer eingeloggt
        $confFile = 'data/settings.json';  // Config-Datei
        if (file_exists($confFile)) {      // Config existiert
            $s = json_decode(file_get_contents($confFile), true);  // Config laden
            if (!empty($s['timeout_active']) && isset($_SESSION['last_activity'])) {  // Timeout aktiv
                $timeout = (($s['timeout_minutes'] ?? 5) * 60);  // Timeout in Sekunden
                if (time() - $_SESSION['last_activity'] > $timeout) {  // Timeout abgelaufen
                    session_destroy();  // Session loeschen
                    if ($dieOnFail) {   // Fehler ausgeben
                        http_response_code(403);  // HTTP 403 Forbidden
                        die(json_encode(['error' => 'Session expired']));  // JSON-Fehler
                    }
                    return false;  // Nicht autorisiert
                }
            }
        }
        return true;  // Autorisiert
    }
    if ($dieOnFail) {  // Fehler ausgeben
        http_response_code(403);  // HTTP 403 Forbidden
        die(json_encode(['error' => 'Access Denied']));  // JSON-Fehler
    }
    return false;  // Nicht autorisiert
}

// Gibt CSRF-Token zurueck oder erstellt neues
function getCSRFToken() {
    if (empty($_SESSION['csrf_token'])) $_SESSION['csrf_token'] = bin2hex(random_bytes(32));  // 32 Bytes = 64 Hex-Zeichen
    return $_SESSION['csrf_token'];  // Token zurueckgeben
}

// Prueft ob CSRF-Token gueltig ist (POST-Requests)
function validateCSRF() {
    $token = $_POST['csrf_token'] ?? $_SERVER['HTTP_X_CSRF_TOKEN'] ?? '';  // Token aus POST oder Header
    if (empty($token) || !hash_equals($_SESSION['csrf_token'] ?? '', $token)) {  // Token ungueltig oder nicht vorhanden
        http_response_code(403);  // HTTP 403 Forbidden
        logUserAction("CSRF_BLOCKED", "Ungueltiger CSRF-Token");  // Log-Eintrag
        die(json_encode(['error' => 'Invalid CSRF token']));  // JSON-Fehler
    }
}

// Rate-Limiting: Begrenzt Anfragen pro IP (DoS-Schutz)
function checkRateLimit($maxRequests = 60, $windowSeconds = 60) {
    global $rateLimitDir;  // Verzeichnis fuer Rate-Limit-Daten
    $ip = $_SERVER['REMOTE_ADDR'];  // Client-IP
    $file = $rateLimitDir . md5($ip) . '.json';  // Dateiname: MD5-Hash der IP
    $now = time();  // Aktueller Zeitstempel
    $data = [];     // Leeres Array
    if (file_exists($file)) {  // Datei existiert
        $data = json_decode(file_get_contents($file), true) ?? [];  // Timestamps laden
        $data = array_filter($data, function($t) use ($now, $windowSeconds) { return ($now - $t) < $windowSeconds; });  // Alte Timestamps entfernen
    }
    if (count($data) >= $maxRequests) {  // Limit erreicht
        http_response_code(429);  // HTTP 429 Too Many Requests
        die(json_encode(['error' => 'Too many requests']));  // JSON-Fehler
    }
    $data[] = $now;  // Aktuellen Timestamp hinzufuegen
    file_put_contents($file, json_encode(array_values($data)));  // Speichern
}

// Liest Authorization-Header aus verschiedenen Quellen
function getAuthorizationHeader() {
    if (!empty($_SERVER['HTTP_AUTHORIZATION']))          return $_SERVER['HTTP_AUTHORIZATION'];  // Standard-Header
    if (!empty($_SERVER['REDIRECT_HTTP_AUTHORIZATION'])) return $_SERVER['REDIRECT_HTTP_AUTHORIZATION'];  // Nach Redirect
    if (!empty($_SERVER['HTTP_X_ESP_TOKEN']))             return 'Bearer ' . $_SERVER['HTTP_X_ESP_TOKEN'];  // Custom-Header
    if (function_exists('getallheaders')) {  // Apache-Funktion
        $headers = getallheaders();  // Alle Headers
        if ($headers) {              // Headers vorhanden
            foreach ($headers as $key => $value) {  // Alle Headers durchgehen
                $lower = strtolower($key);  // Zu Lowercase
                if ($lower === 'authorization') return $value;  // Authorization-Header
                if ($lower === 'x-esp-token')   return 'Bearer ' . $value;  // Custom-Header
            }
        }
    }
    return '';  // Kein Header gefunden
}

// Prueft ob ESP-Token gueltig ist (fuer ESP-Kommunikation)
function validateESPToken() {
    global $confFile;  // Config-Datei
    $authHeader = getAuthorizationHeader();  // Authorization-Header holen
    $token = '';  // Leerer Token
    if (preg_match('/^Bearer\s+(.+)$/i', $authHeader, $matches)) {  // "Bearer TOKEN" Format
        $token = $matches[1];  // Token extrahieren
    } elseif (!empty($authHeader)) {  // Header vorhanden aber kein Bearer
        $token = $authHeader;  // Direkt als Token verwenden
    }
    $settings = json_decode(file_get_contents($confFile), true) ?? [];  // Config laden
    $expectedToken = $settings['esp_token'] ?? '';  // Erwarteter Token
    if (empty($token) || empty($expectedToken) || !hash_equals($expectedToken, $token)) {  // Token ungueltig
        http_response_code(401);  // HTTP 401 Unauthorized
        $ip = $_SERVER['REMOTE_ADDR'];  // Client-IP
        $entry = date("[d.m.Y H:i:s]") . " SECURITY: ESP auth failed from $ip (got " . strlen($token) . " chars)\n";  // Log-Eintrag
        file_put_contents('data/log.txt', $entry, FILE_APPEND);  // Log schreiben
        die(json_encode(['error' => 'Invalid device token']));  // JSON-Fehler
    }
}

// Validiert und bereinigt Node-Namen (sender, receiver, camera)
function sanitizeNodeName($name) {
    $allowed = ['sender', 'receiver', 'camera'];  // Erlaubte Nodes
    $name = strtolower(trim($name));  // Zu Lowercase und Whitespace entfernen
    if (!in_array($name, $allowed)) {  // Node nicht erlaubt
        http_response_code(400);  // HTTP 400 Bad Request
        die(json_encode(['error' => 'Invalid node: ' . htmlspecialchars($name)]));  // JSON-Fehler
    }
    return $name;  // Bereinigter Name
}

// Validiert und bereinigt Befehle
function sanitizeCommand($cmd) {
    $allowed = ['ALARM_ON', 'ALARM_OFF', 'REBOOT', 'RESET', 'STATUS', 'UPDATE'];  // Erlaubte Befehle
    $cmd = strtoupper(trim($cmd));  // Zu Uppercase und Whitespace entfernen
    if (!in_array($cmd, $allowed)) {  // Befehl nicht erlaubt
        http_response_code(400);  // HTTP 400 Bad Request
        die(json_encode(['error' => 'Invalid command: ' . htmlspecialchars($cmd)]));  // JSON-Fehler
    }
    return $cmd;  // Bereinigter Befehl
}

// Loggt Benutzeraktionen mit Details
function logUserAction($action, $details = "") {
    global $userLogFile;  // User-Log-Datei
    $logs = json_decode(file_get_contents($userLogFile), true) ?? [];  // Logs laden
    $ip = $_SERVER['REMOTE_ADDR'];  // Client-IP
    $agent = $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown';  // User-Agent
    $device = gethostbyaddr($ip);  // Hostname der IP
    if ($device == $ip) $device = "Unknown Device";  // Hostname nicht aufloesbar
    $newLog = [
        'timestamp' => time(),  // Unix-Timestamp
        'date' => date("d.m.Y H:i:s"),  // Formatiertes Datum
        'ip' => $ip,  // IP-Adresse
        'device_name' => $device,  // Geraete-Name
        'user_agent' => substr($agent, 0, 256),  // User-Agent (max 256 Zeichen)
        'action' => substr($action, 0, 50),  // Aktion (max 50 Zeichen)
        'details' => substr($details, 0, 256),  // Details (max 256 Zeichen)
        'session_id' => session_id()  // Session-ID
    ];
    array_unshift($logs, $newLog);  // Neuen Log am Anfang einfuegen
    $logs = array_slice($logs, 0, 100);  // Nur letzte 100 Logs behalten
    file_put_contents($userLogFile, json_encode($logs));  // Logs speichern
}

// ============================================================
// SESSION
// ============================================================
ini_set('session.cookie_httponly', 1);  // Cookie nur per HTTP (nicht per JavaScript)
ini_set('session.cookie_samesite', 'Strict');  // Cookie nur bei gleicher Site
ini_set('session.use_strict_mode', 1);  // Strenger Session-Modus
if (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off') {  // HTTPS aktiv
    ini_set('session.cookie_secure', 1);  // Cookie nur ueber HTTPS
}
session_start();  // Session starten

// ============================================================
// A. DASHBOARD AKTIONEN
// ============================================================

// Workaround fuer manche Webserver: POST-Daten manuell parsen wenn noetig
$_rawPostBody = '';  // Roher POST-Body
if ($_SERVER['REQUEST_METHOD'] === 'POST' && empty($_POST) && isset($_GET['action'])) {  // POST ohne $_POST-Array
    $_rawPostBody = file_get_contents('php://input');  // Rohen Body lesen
    if (!empty($_rawPostBody)) {  // Body nicht leer
        parse_str($_rawPostBody, $_POST);  // In $_POST-Array parsen
    }
}

// Dashboard-Aktionen verarbeiten (per GET-Parameter 'action')
if (isset($_GET['action'])) {
    $action = $_GET['action'];  // Aktion aus GET-Parameter

    // Session-Activity pingen (ohne Auth-Requirement, da nur fuer Keep-Alive)
    if ($action === 'ping_activity') {
        if (requireAuth(false)) {  // Pruefe Auth ohne zu sterben
            $_SESSION['last_activity'] = time();  // Letzten Activity-Timestamp aktualisieren
            echo json_encode(['status' => 'ok']);  // OK zurueckgeben
        }
        exit;  // Beenden
    }
    // CSRF-Token abrufen (ohne Auth-Requirement)
    if ($action === 'get_csrf_token') {
        if (requireAuth(false)) echo json_encode(['csrf_token' => getCSRFToken()]);  // Token zurueckgeben
        exit;  // Beenden
    }

    // Ab hier: Auth und Rate-Limiting fuer alle Aktionen
    requireAuth();  // Auth pruefen (stirbt bei Fehler)
    checkRateLimit(60, 60);  // Max 60 Requests pro Minute

    // User-Logs abrufen
    if ($action === 'get_user_logs') {
        header('Content-Type: application/json');  // JSON-Header
        echo file_get_contents($userLogFile);  // Logs ausgeben
        exit;  // Beenden
    }
    // User-Logs als CSV exportieren
    if ($action === 'export_user_logs') {
        $logs = json_decode(file_get_contents($userLogFile), true) ?? [];  // Logs laden
        header('Content-Type: text/csv; charset=utf-8');  // CSV-Header
        header('Content-Disposition: attachment; filename="user_logs_' . date('Y-m-d_H-i-s') . '.csv"');  // Download-Dateiname
        $out = fopen('php://output', 'w');  // Ausgabe-Stream
        fputcsv($out, ['Zeitstempel','Datum','IP','Geraet','Aktion','Details','Session','User Agent']);  // CSV-Header
        foreach ($logs as $l) fputcsv($out, [$l['timestamp'],$l['date'],$l['ip'],$l['device_name'],$l['action'],$l['details'],$l['session_id'],$l['user_agent']]);  // Zeilen
        fclose($out);  // Stream schliessen
        exit;  // Beenden
    }
    // ESP-Token abrufen
    if ($action === 'get_esp_token') {
        $s = json_decode(file_get_contents($confFile), true);  // Config laden
        echo json_encode(['esp_token' => $s['esp_token'] ?? '']);  // Token zurueckgeben
        exit;  // Beenden
    }
    // Telemetrie als CSV exportieren
    if ($action === 'export_telemetry') {
        $csvFile = $dataDir . 'telemetry.csv';  // Telemetrie-Datei
        if (!file_exists($csvFile)) {  // Datei existiert nicht
            http_response_code(404);  // HTTP 404 Not Found
            die('Keine Telemetrie-Daten vorhanden.');  // Fehlermeldung
        }
        header('Content-Type: text/csv; charset=utf-8');  // CSV-Header
        header('Content-Disposition: attachment; filename="telemetry_' . date('Y-m-d_H-i-s') . '.csv"');  // Download-Dateiname
        echo "timestamp,source,rssi,heap\n";  // CSV-Header
        readfile($csvFile);  // Datei ausgeben
        exit;  // Beenden
    }

    // Alarm-Aufnahmen abrufen
    if ($action === 'get_recordings') {
        $recDir = $dataDir . 'recordings';  // Aufnahmen-Verzeichnis
        $files = [];  // Leeres Array
        if (is_dir($recDir)) {  // Verzeichnis existiert
            foreach (glob($recDir . '/alarm_*.{avi,mp4,mkv}', GLOB_BRACE) as $f) {  // Alle Aufnahmen
                $files[] = [
                    'name' => basename($f),  // Dateiname
                    'size' => filesize($f),  // Groesse in Bytes
                    'size_mb' => round(filesize($f) / 1048576, 1),  // Groesse in MB
                    'date' => date('d.m.Y H:i:s', filemtime($f)),  // Datum
                    'timestamp' => filemtime($f)  // Unix-Timestamp
                ];
            }
        }
        usort($files, function($a, $b) { return $b['timestamp'] - $a['timestamp']; });  // Nach Datum sortieren (neueste zuerst)
        echo json_encode(['recordings' => $files]);  // JSON zurueckgeben
        exit;  // Beenden
    }

    // Aufnahme herunterladen
    if ($action === 'download_recording' && isset($_GET['file'])) {
        $filename = basename($_GET['file']);  // Dateiname bereinigen
        $filepath = $dataDir . 'recordings/' . $filename;  // Voller Pfad
        if (!file_exists($filepath) || !preg_match('/^alarm_\d{8}_\d{6}\.(avi|mp4|mkv)$/', $filename)) {  // Datei nicht vorhanden oder ungueltiger Name
            http_response_code(404);  // HTTP 404 Not Found
            die('Datei nicht gefunden');  // Fehlermeldung
        }
        header('Content-Type: application/octet-stream');  // Binaer-Download
        header('Content-Disposition: attachment; filename="' . $filename . '"');  // Download-Dateiname
        header('Content-Length: ' . filesize($filepath));  // Dateigroesse
        readfile($filepath);  // Datei ausgeben
        exit;  // Beenden
    }

    // === ALARM STATUS (Locked Read) ===
    if ($action === 'get_alarm_status') {
        $statusFile2 = $dataDir . 'alarm_monitor.json';  // Alarm-Monitor-Datei

        if (file_exists($statusFile2)) {  // Datei existiert
            $data = readJsonLocked($statusFile2); //  Locked Read (blockiert wenn Python schreibt)
            echo json_encode($data);  // JSON zurueckgeben
        } else {  // Datei existiert nicht
            echo json_encode(['state' => 'not_running', 'timestamp' => null]);  // Standard-Antwort
        }
        exit;  // Beenden
    }

    // Ab hier: Nur POST-Requests erlaubt
    if ($_SERVER['REQUEST_METHOD'] !== 'POST') {  // Kein POST-Request
        http_response_code(405);  // HTTP 405 Method Not Allowed
        die(json_encode(['error' => 'POST required']));  // JSON-Fehler
    }
    validateCSRF();  // CSRF-Token pruefen

    // Aufnahme loeschen
    if ($action === 'delete_recording' && isset($_POST['file'])) {
        $filename = basename($_POST['file']);  // Dateiname bereinigen
        $filepath = $dataDir . 'recordings/' . $filename;  // Voller Pfad
        if (!file_exists($filepath) || !preg_match('/^alarm_\d{8}_\d{6}\.(avi|mp4|mkv)$/', $filename)) {  // Datei nicht vorhanden oder ungueltiger Name
            http_response_code(404);  // HTTP 404 Not Found
            die(json_encode(['error' => 'Datei nicht gefunden']));  // JSON-Fehler
        }
        unlink($filepath);  // Datei loeschen
        logUserAction("Recordings", "Aufnahme geloescht: $filename");  // Log-Eintrag
        echo json_encode(['status' => 'ok', 'message' => "Aufnahme geloescht: $filename"]);  // JSON-Antwort
        exit;  // Beenden
    }

    // Alle Aufnahmen loeschen
    if ($action === 'delete_all_recordings') {
        $recDir = $dataDir . 'recordings';  // Aufnahmen-Verzeichnis
        $count = 0;  // Zaehler
        if (is_dir($recDir)) {  // Verzeichnis existiert
            foreach (glob($recDir . '/alarm_*.{avi,mp4,mkv}', GLOB_BRACE) as $f) {  // Alle Aufnahmen
                unlink($f);  // Datei loeschen
                $count++;    // Zaehler erhoehen
            }
        }
        logUserAction("Recordings", "$count Aufnahmen geloescht");  // Log-Eintrag
        echo json_encode(['status' => 'ok', 'message' => "$count Aufnahmen geloescht"]);  // JSON-Antwort
        exit;  // Beenden
    }

    // Logs fuer einen Node loeschen
    if ($action === 'clear_logs' && isset($_POST['target'])) {
        $target = sanitizeNodeName($_POST['target']);  // Node validieren
        $lines = file_exists($logFile) ? file($logFile) : [];  // Log-Zeilen laden
        if ($target === 'sender') {  // Sender-Logs loeschen
            $filtered = array_filter($lines, function($line) {  // Nur receiver/camera behalten
                return strpos($line, 'receiver:') !== false || strpos($line, 'camera:') !== false;
            });
        } else {  // Andere Nodes
            $filtered = array_filter($lines, function($line) use ($target) {  // Nur Zeilen ohne diesen Node
                return strpos($line, $target . ':') === false;
            });
        }
        file_put_contents($logFile, implode('', $filtered));  // Gefilterte Logs speichern
        logUserAction("Clear Logs", "Logs fuer '$target' geloescht");  // Log-Eintrag
        echo json_encode(['status' => 'ok', 'message' => "Logs geloescht"]);  // JSON-Antwort
        exit;  // Beenden
    }

    // User-Logs loeschen
    if ($action === 'clear_user_logs') {
        logUserAction("Clear User Logs", "Geloescht");  // Log-Eintrag (vor dem Loeschen!)
        file_put_contents($userLogFile, json_encode([]));  // Leeres Array schreiben
        echo json_encode(['status' => 'ok', 'message' => 'User-Logs geloescht.']);  // JSON-Antwort
        exit;  // Beenden
    }

    // Telemetrie-Daten loeschen
    if ($action === 'clear_telemetry') {
        $csvFile = $dataDir . 'telemetry.csv';  // Telemetrie-Datei
        if (file_exists($csvFile)) @unlink($csvFile);  // Datei loeschen
        if (file_exists($csvFile . '.old')) @unlink($csvFile . '.old');  // Backup loeschen
        logUserAction("Clear Telemetry", "Telemetrie-Daten geloescht");  // Log-Eintrag
        echo json_encode(['status' => 'ok', 'message' => 'Telemetrie geloescht.']);  // JSON-Antwort
        exit;  // Beenden
    }

    // Alle System-Logs loeschen
    if ($action === 'clear_all_logs') {
        file_put_contents($logFile, "");  // Leere Datei schreiben
        logUserAction("Clear All Logs", "System-Logs geloescht");  // Log-Eintrag
        echo json_encode(['status' => 'ok', 'message' => 'System-Logs geloescht.']);  // JSON-Antwort
        exit;  // Beenden
    }

    // Einstellungen speichern
    if ($action === 'save_settings' && isset($_POST['settings'])) {
        $new = json_decode($_POST['settings'], true);  // Neue Einstellungen aus JSON
        if (!$new) { http_response_code(400); die(json_encode(['error' => 'Invalid JSON'])); }  // JSON-Parse-Fehler
        $cur = json_decode(file_get_contents($confFile), true) ?? [];  // Aktuelle Einstellungen

        // Passwort-Handling
        if (!empty($new['password'])) { $new['password'] = password_hash($new['password'], PASSWORD_BCRYPT); }  // Neues Passwort hashen
        else { $new['password'] = $cur['password']; }  // Altes Passwort behalten
        // Alarm-PIN-Handling
        if (!empty($new['alarm_pin'])) { $new['alarm_pin'] = password_hash($new['alarm_pin'], PASSWORD_BCRYPT); }  // Neue PIN hashen
        else { $new['alarm_pin'] = $cur['alarm_pin'] ?? password_hash("CHANGE_ME", PASSWORD_BCRYPT); }  // Alte PIN oder Standard
        $new['esp_token'] = $cur['esp_token'] ?? bin2hex(random_bytes(16));  // ESP-Token behalten oder neu generieren

        // Timeout-Einstellungen validieren
        if (!isset($new['timeout_active'])) $new['timeout_active'] = false;  // Standard: aus
        $new['timeout_minutes'] = min(max((int)($new['timeout_minutes'] ?? 5), 1), 120);  // 1-120 Minuten
        $new['refresh_rate'] = max(1000, min((int)($new['refresh_rate'] ?? 2000), 30000));  // 1-30 Sekunden
        $new['camera_port'] = (int)($new['camera_port'] ?? $cur['camera_port'] ?? 8082);  // Camera-Port
        $new['site_title'] = htmlspecialchars(substr($new['site_title'] ?? 'IoT-AlarmSystem', 0, 50));  // Site-Titel (max 50 Zeichen)

        // Nur erlaubte Keys behalten (Whitelist)
        $allowed = ['password','refresh_rate','site_title','timeout_active','timeout_minutes','esp_token','camera_port','alarm_pin'];
        $final = [];  // Finale Config
        foreach ($allowed as $k) $final[$k] = $new[$k] ?? $cur[$k] ?? null;  // Keys kopieren

        file_put_contents($confFile, json_encode($final));  // Config speichern
        logUserAction("Config", "Einstellungen geaendert");  // Log-Eintrag
        echo json_encode(['status' => 'ok', 'message' => 'Gespeichert.']);  // JSON-Antwort
        exit;  // Beenden
    }

    // Node-Konfiguration speichern (fuer Remote-Update der ESPs)
    if ($action === 'save_node_config' && isset($_POST['target'])) {
        $target = sanitizeNodeName($_POST['target']);  // Node validieren
        $config = json_decode($_POST['config'] ?? '{}', true);  // Config aus JSON
        if (!$config) { http_response_code(400); die(json_encode(['error' => 'Invalid config'])); }  // JSON-Parse-Fehler

        $file = $dataDir . 'update_' . $target . '.json';  // Update-Datei
        file_put_contents($file, json_encode($config));  // Config speichern
        logUserAction("Node Update", "Config fuer '$target'");  // Log-Eintrag
        echo json_encode(['status' => 'ok', 'message' => "Config fuer '$target' hinterlegt."]);  // JSON-Antwort
        exit;  // Beenden
    }

    // System-Reset (alle Daten loeschen)
    if ($action === 'system_reset') {
        file_put_contents($statusFile, json_encode([]));  // Status loeschen
        file_put_contents($logFile, "");  // Logs loeschen
        file_put_contents($cmdFile, json_encode([]));  // Befehle loeschen
        logUserAction("SYSTEM RESET", "Alle Daten geloescht");  // Log-Eintrag
        echo json_encode(['status' => 'ok', 'message' => 'System Reset.']);  // JSON-Antwort
        exit;  // Beenden
    }

    // Befehl an Node senden
    if ($action === 'send_command' && isset($_POST['target']) && isset($_POST['cmd'])) {
        $target = sanitizeNodeName($_POST['target']);  // Node validieren
        $cmd = sanitizeCommand($_POST['cmd']);  // Befehl validieren

        $cmds = json_decode(file_get_contents($cmdFile), true) ?? [];  // Befehle laden
        $cmds[$target] = $cmd;  // Befehl setzen
        file_put_contents($cmdFile, json_encode($cmds));  // Befehle speichern

        $status = json_decode(file_get_contents($statusFile), true) ?? [];  // Status laden
        $isOnline = isset($status[$target]['last_seen']) && (time() - $status[$target]['last_seen'] < 60);  // Online-Status

        logUserAction("Command", "'$cmd' -> '$target'" . ($isOnline ? "" : " (offline)"));  // Log-Eintrag
        $msg = $isOnline
            ? "'$cmd' an '$target' gesendet."  // Node online
            : "'$cmd' fuer '$target' hinterlegt (offline - wird beim naechsten Heartbeat ausgefuehrt).";  // Node offline
        echo json_encode(['status' => 'ok', 'message' => $msg]);  // JSON-Antwort
        exit;  // Beenden
    }

    // ESP-Token regenerieren
    if ($action === 'regenerate_esp_token') {
        $s = json_decode(file_get_contents($confFile), true) ?? [];  // Config laden
        $s['esp_token'] = bin2hex(random_bytes(16));  // Neuen Token generieren
        file_put_contents($confFile, json_encode($s));  // Config speichern
        logUserAction("Security", "ESP-Token regeneriert");  // Log-Eintrag
        echo json_encode(['status' => 'ok', 'token' => $s['esp_token']]);  // JSON-Antwort mit neuem Token
        exit;  // Beenden
    }

    // Seriellen Befehl an Arduino senden (SCHARF/UNSCHARF)
    if ($action === 'serial_send' && isset($_POST['cmd'])) {
        $cmd = $_POST['cmd'];  // Befehl
        $erlaubt = ['SCHARF', 'UNSCHARF', '1', '0'];  // Erlaubte Befehle
        if (!in_array($cmd, $erlaubt)) {  // Befehl nicht erlaubt
            http_response_code(400);  // HTTP 400 Bad Request
            die(json_encode(['error' => 'Unerlaubter Befehl']));  // JSON-Fehler
        }

        // PIN pruefen
        $pin = isset($_POST['alarm_pin']) ? $_POST['alarm_pin'] : '';  // PIN aus POST
        $s = json_decode(file_get_contents($confFile), true) ?? [];  // Config laden
        $storedPin = isset($s['alarm_pin']) ? $s['alarm_pin'] : '';  // Gespeicherte PIN (gehashed)
        if (empty($storedPin)) {  // Keine PIN gespeichert
            $storedPin = password_hash("CHANGE_ME", PASSWORD_BCRYPT);  // Standard-PIN hashen
            $s['alarm_pin'] = $storedPin;  // Speichern
            file_put_contents($confFile, json_encode($s));  // Config schreiben
        }
        if (empty($pin) || !password_verify($pin, $storedPin)) {  // PIN falsch oder leer
            http_response_code(401);  // HTTP 401 Unauthorized
            logUserAction("Alarm", "PIN falsch - Zugriff verweigert");  // Log-Eintrag
            die(json_encode(['error' => 'Falscher Alarm-PIN!']));  // JSON-Fehler
        }

        // Seriellen Port suchen
        $port = null;  // Kein Port gefunden
        foreach (['/dev/ttyUSB0', '/dev/ttyUSB1', '/dev/ttyACM0', '/dev/ttyACM1'] as $p) {  // Moegliche Ports
            if (file_exists($p)) { $port = $p; break; }  // Port gefunden
        }
        if (!$port) {  // Kein Port gefunden
            echo json_encode(['status' => 'error', 'message' => 'Kein serieller Port gefunden. Arduino angeschlossen?']);  // JSON-Fehler
            exit;  // Beenden
        }

        // Befehl konvertieren
        $serialCmd = ($cmd === 'SCHARF' || $cmd === '1') ? '1' : '0';  // 1 = SCHARF, 0 = UNSCHARF
        $label = ($serialCmd === '1') ? 'SCHARF' : 'UNSCHARF';  // Label fuer Log

        // Port konfigurieren mit stty
        exec("stty -F " . escapeshellarg($port) . " 9600 cs8 -cstopb -parenb -echo -hupcl raw 2>&1", $sttyOut, $sttyRc);
        if ($sttyRc !== 0) {  // stty fehlgeschlagen
            echo json_encode(['status' => 'error', 'message' => 'Port-Konfiguration fehlgeschlagen: ' . implode(' ', $sttyOut) . ' - Tipp: sudo usermod -a -G dialout www-data']);  // JSON-Fehler
            exit;  // Beenden
        }

        usleep(50000);  // 50ms warten (Port-Stabilisierung)

        // Befehl senden
        $fp = @fopen($port, 'w');  // Port zum Schreiben oeffnen
        if ($fp === false) {  // Oeffnen fehlgeschlagen
            echo json_encode(['status' => 'error', 'message' => 'Senden fehlgeschlagen. Berechtigung pruefen: sudo usermod -a -G dialout www-data']);  // JSON-Fehler
            exit;  // Beenden
        }
        fwrite($fp, $serialCmd . "\n");  // Befehl schreiben (mit Newline)
        fflush($fp);  // Puffer leeren
        usleep(50000);  // 50ms warten (Arduino-Verarbeitung)
        fclose($fp);  // Port schliessen

        logUserAction("Alarm", "$label via Seriell an $port");  // Log-Eintrag
        $logEntry = date("[d.m.Y H:i:s]") . " camera: Alarm $label gesendet (Seriell -> $port)\n";  // System-Log
        file_put_contents($logFile, $logEntry, FILE_APPEND);  // System-Log schreiben
        echo json_encode(['status' => 'ok', 'message' => "Alarm $label gesendet ($port)", 'port' => $port, 'cmd' => $serialCmd]);  // JSON-Antwort
        exit;  // Beenden
    }

    // Raspberry Pi neu starten
    if ($action === 'pi_reboot') {
        logUserAction("PI REBOOT", "Neustart ausgeloest");  // Log-Eintrag
        echo json_encode(['status' => 'ok', 'message' => 'Pi wird neugestartet...']);  // JSON-Antwort
        if (function_exists('fastcgi_finish_request')) fastcgi_finish_request();  // Response sofort senden (FastCGI)
        exec('sudo /sbin/reboot 2>&1 &');  // Neustart-Befehl (im Hintergrund)
        exit;  // Beenden
    }

    // Raspberry Pi herunterfahren (erfordert Alarm-PIN)
    if ($action === 'pi_shutdown') {
        // PIN pruefen
        $pin = isset($_POST['alarm_pin']) ? $_POST['alarm_pin'] : '';  // PIN aus POST
        $s = json_decode(file_get_contents($confFile), true) ?? [];  // Config laden
        $storedPin = isset($s['alarm_pin']) ? $s['alarm_pin'] : '';  // Gespeicherte PIN
        if (empty($storedPin)) {  // Keine PIN gespeichert
            $storedPin = password_hash("CHANGE_ME", PASSWORD_BCRYPT);  // Standard-PIN
            $s['alarm_pin'] = $storedPin;  // Speichern
            file_put_contents($confFile, json_encode($s));  // Config schreiben
        }
        if (empty($pin) || !password_verify($pin, $storedPin)) {  // PIN falsch
            http_response_code(401);  // HTTP 401 Unauthorized
            logUserAction("PI SHUTDOWN", "PIN falsch - Zugriff verweigert");  // Log-Eintrag
            die(json_encode(['error' => 'Falscher Alarm-PIN!']));  // JSON-Fehler
        }
        logUserAction("PI SHUTDOWN", "Herunterfahren ausgeloest");  // Log-Eintrag
        echo json_encode(['status' => 'ok', 'message' => 'Pi wird heruntergefahren...']);  // JSON-Antwort
        if (function_exists('fastcgi_finish_request')) fastcgi_finish_request();  // Response sofort senden
        exec('sudo /sbin/poweroff 2>&1 &');  // Shutdown-Befehl (im Hintergrund)
        exit;  // Beenden
    }

    // Unbekannte Aktion
    http_response_code(400);  // HTTP 400 Bad Request
    die(json_encode(['error' => 'Unknown action']));  // JSON-Fehler
}

// ============================================================
// B. ESP KOMMUNIKATION
// ============================================================
// POST-Requests ohne 'action'-Parameter = ESP-Kommunikation
if ($_SERVER['REQUEST_METHOD'] === 'POST' && !isset($_GET['action'])) {
    checkRateLimit(60, 60);  // Rate-Limiting
    validateESPToken();  // ESP-Token pruefen

    $json = file_get_contents('php://input');  // Rohen POST-Body lesen
    if (strlen($json) > 4096) { http_response_code(413); die(json_encode(['error' => 'Too large'])); }  // Payload zu gross

    $data = json_decode($json, true);  // JSON parsen
    if ($data && isset($data['source'])) {  // JSON gueltig und 'source' vorhanden
        $allowedSources = ['sender', 'receiver', 'camera'];  // Erlaubte Quellen
        $source = strtolower(trim($data['source']));  // Quelle normalisieren
        if (!in_array($source, $allowedSources)) { http_response_code(400); die(json_encode(['error' => 'Invalid source'])); }  // Ungueltige Quelle

        // Log-Nachricht verarbeiten
        if (isset($data['log'])) {
            $logMsg = preg_replace('/[\x00-\x1F\x7F]/', '', substr($data['log'], 0, 256));  // Control-Zeichen entfernen, max 256 Zeichen
            $entry = date("[d.m.Y H:i:s]") . " $source: " . $logMsg . "\n";  // Log-Zeile
            if (file_exists($logFile) && filesize($logFile) > 500000) {  // Log-Datei zu gross (>500KB)
                file_put_contents($logFile, implode('', array_slice(file($logFile), -200)));  // Nur letzte 200 Zeilen behalten
            }
            file_put_contents($logFile, $entry, FILE_APPEND);  // Log schreiben
        }

        // Status-Update verarbeiten
        if (isset($data['status_msg'])) {
            $sd = [
                'last_seen' => time(),  // Aktueller Zeitstempel
                'ip'        => filter_var($data['ip'] ?? '0.0.0.0', FILTER_VALIDATE_IP) ?: '0.0.0.0',  // IP validieren
                'status'    => substr($data['status_msg'] ?? '', 0, 128),  // Status (max 128 Zeichen)
                'alarm'     => ($source === "receiver" && isset($data['alarm_state'])) ? (bool)$data['alarm_state'] : false,  // Alarm-Status (nur fuer receiver)
                'rssi'      => (int)($data['rssi'] ?? 0),  // Signalstaerke
                'heap'      => (int)($data['heap'] ?? 0),  // Freier RAM
                'reset_reason' => substr($data['reset_reason'] ?? 'unknown', 0, 32),  // Reset-Grund
                'uptime'    => (int)($data['uptime'] ?? 0)  // Uptime in Sekunden
            ];

            // === STATUS LOCKING (Locked Read + Write) ===
            $cs = readJsonLocked($statusFile); //  Locked Read (wartet wenn Python schreibt)
            
            // Auto-Logging (alle 60s oder bei Status-Aenderung)
            $lastLogTime = $cs[$source]['last_log_time'] ?? 0;  // Letzter Log-Zeitstempel
            $statusChanged = ($cs[$source]['status'] ?? '') !== $sd['status'];  // Status hat sich geaendert
            if (!isset($data['log']) && (time() - $lastLogTime >= 60 || $statusChanged)) {  // Kein expliziter Log und 60s vergangen oder Status geaendert
                $autoMsg = $sd['status'] ?: 'Heartbeat';  // Log-Message
                $entry = date("[d.m.Y H:i:s]") . " $source: $autoMsg\n";  // Log-Zeile
                if (file_exists($logFile) && filesize($logFile) > 500000) {  // Log zu gross
                    file_put_contents($logFile, implode('', array_slice(file($logFile), -200)));  // Trimmen
                }
                file_put_contents($logFile, $entry, FILE_APPEND);  // Log schreiben
                $sd['last_log_time'] = time();  // Timestamp setzen
            } else {
                $sd['last_log_time'] = $cs[$source]['last_log_time'] ?? time();  // Alten Timestamp behalten
            }
            
            $cs[$source] = array_merge($cs[$source] ?? [], $sd);  // Status mergen (alte Werte behalten wenn nicht ueberschrieben)
            writeJsonLocked($statusFile, $cs); //  Locked Write (mit fsync fuer SD-Karte)
            // ============================================

            // Telemetrie-CSV schreiben
            $csvFile = $dataDir . 'telemetry.csv';  // CSV-Datei
            if (file_exists($csvFile) && filesize($csvFile) > 1000000) rename($csvFile, $csvFile . ".old");  // Backup wenn >1MB
            file_put_contents($csvFile, time().",$source,{$sd['rssi']},{$sd['heap']}\n", FILE_APPEND);  // CSV-Zeile anhaengen
        }

        // Response an ESP (mit Befehlen und Updates)
        $response = ["logging_active" => true];  // Logging ist aktiv
        $cmds = json_decode(file_get_contents($cmdFile), true) ?? [];  // Befehle laden
        if (isset($cmds[$source])) {  // Befehl fuer diesen Node vorhanden
            $response["command"] = $cmds[$source];  // Befehl in Response
            unset($cmds[$source]);  // Befehl aus Liste entfernen
            file_put_contents($cmdFile, json_encode($cmds));  // Befehle speichern
        }
        $updateFile = $dataDir . 'update_' . $source . '.json';  // Update-Datei fuer diesen Node
        if (file_exists($updateFile)) {  // Update vorhanden
            $response["new_config"] = json_decode(file_get_contents($updateFile), true);  // Config in Response
            unlink($updateFile);  // Update-Datei loeschen (nur einmal anwenden)
        }

        header('Content-Type: application/json');  // JSON-Header
        echo json_encode($response);  // Response senden
    } else {  // JSON ungueltig oder 'source' fehlt
        http_response_code(400);  // HTTP 400 Bad Request
        echo json_encode(['error' => 'Invalid payload']);  // JSON-Fehler
    }
    exit;  // Beenden
}

// ============================================================
// C. GET ALL (Dashboard)
// ============================================================
// GET-Request mit '?get=all' = Dashboard-Daten abrufen
if ($_SERVER['REQUEST_METHOD'] === 'GET' && isset($_GET['get']) && $_GET['get'] === 'all') {
    requireAuth();  // Auth pruefen

    // === STATUS LOCKING (Locked Read) ===
    $status = readJsonLocked($statusFile); //  Locked Read
    // ====================================

    // Online-Status berechnen (last_seen < 60s = online)
    foreach ($status as $k => $v) {
        $status[$k]['online'] = (isset($v['last_seen']) && time() - $v['last_seen'] < 60);  // Online wenn letzter Heartbeat <60s
    }

    // Raspberry Pi Status hinzufuegen
    $piUptime = 0;  // Pi-Uptime in Sekunden
    if (file_exists('/proc/uptime')) {  // Uptime-Datei existiert
        $parts = explode(' ', file_get_contents('/proc/uptime'));  // Datei lesen und splitten
        $piUptime = (int)floatval($parts[0]);  // Erste Zahl = Uptime in Sekunden
    }

    $status['pi'] = [
        'last_seen'    => time(),  // Aktueller Zeitstempel
        'ip'           => $_SERVER['SERVER_ADDR'] ?? '127.0.0.1',  // Server-IP
        'status'       => 'Running',  // Status
        'online'       => true,  // Immer online
        'rssi'         => 0,  // Kein RSSI fuer Pi
        'heap'         => 0,  // Kein Heap fuer Pi
        'uptime'       => $piUptime,  // Uptime
        'reset_reason' => 'N/A',  // Kein Reset-Grund
        'cpu_temp'     => 0,  // CPU-Temperatur
        'cpu_load'     => '0'  // CPU-Load
    ];

    // CPU-Temperatur auslesen
    if (file_exists('/sys/class/thermal/thermal_zone0/temp')) {  // Temperatur-Datei existiert
        $raw = trim(file_get_contents('/sys/class/thermal/thermal_zone0/temp'));  // Temperatur lesen (milli-Grad Celsius)
        $status['pi']['cpu_temp'] = round((int)$raw / 1000, 1);  // In Grad Celsius umrechnen
    }

    // CPU-Load auslesen
    if (file_exists('/proc/loadavg')) {  // Load-Datei existiert
        $parts = explode(' ', trim(file_get_contents('/proc/loadavg')));  // Datei lesen und splitten
        $status['pi']['cpu_load'] = $parts[0] ?? '0';  // 1-Minuten-Load-Average
    }

    // System-Logs (letzte 20 Zeilen)
    $logs = file_exists($logFile) ? array_slice(file($logFile), -20) : [];  // Letzte 20 Zeilen
    // Einstellungen laden (ohne sensible Daten)
    $settings = json_decode(file_get_contents($confFile), true);  // Config laden
    unset($settings['password'], $settings['esp_token']);  // Passwort und Token entfernen

    // === ALARM MONITOR LOCKING (Locked Read) ===
    $alarmMonFile = $dataDir . 'alarm_monitor.json';  // Alarm-Monitor-Datei
    $alarmMon = file_exists($alarmMonFile)
        ? readJsonLocked($alarmMonFile) //  Locked Read
        : ['state' => 'not_running'];  // Standard wenn Datei nicht existiert
    // ===========================================

    header('Content-Type: application/json');  // JSON-Header
    echo json_encode([
        "status"        => $status,  // Status aller Nodes + Pi
        "logs"          => $logs,  // System-Logs
        "config"        => $settings,  // Einstellungen
        "alarm_monitor" => $alarmMon  // Alarm-Monitor-Status
    ]);
    exit;  // Beenden
}
?>