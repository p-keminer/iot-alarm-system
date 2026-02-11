<?php
// api.php - V5.1 (Security Hardened + Lighttpd-kompatibel)

header('X-Content-Type-Options: nosniff');
header('X-Frame-Options: DENY');
header('X-XSS-Protection: 1; mode=block');
header('Cache-Control: no-store, no-cache, must-revalidate');

$dataDir = 'data/';
if (!is_dir($dataDir)) mkdir($dataDir, 0750, true);

$statusFile   = $dataDir . 'status.json';
$logFile      = $dataDir . 'log.txt';
$userLogFile  = $dataDir . 'user_logs.json';
$cmdFile      = $dataDir . 'commands.json';
$confFile     = $dataDir . 'settings.json';
$rateLimitDir = $dataDir . 'ratelimit/';

if (!is_dir($rateLimitDir)) mkdir($rateLimitDir, 0750, true);

if (!file_exists($statusFile))  file_put_contents($statusFile, json_encode([]));
if (!file_exists($cmdFile))     file_put_contents($cmdFile, json_encode([]));
if (!file_exists($userLogFile)) file_put_contents($userLogFile, json_encode([]));

if (!file_exists($confFile)) {
    $defaults = [
        "password"        => password_hash("admin", PASSWORD_BCRYPT),
        "refresh_rate"    => 2000,
        "site_title"      => "IoT-AlarmSystem",
        "timeout_active"  => true,
        "timeout_minutes" => 5,
        "esp_token"       => bin2hex(random_bytes(16)),
        "camera_port"     => 8082
    ];
    file_put_contents($confFile, json_encode($defaults));
}

// ============================================================
// SECURITY HELPERS
// ============================================================

function requireAuth($dieOnFail = true) {
    if (isset($_SESSION['loggedin']) && $_SESSION['loggedin'] === true) {
        // Timeout-Check auch fuer AJAX-Requests
        $confFile = 'data/settings.json';
        if (file_exists($confFile)) {
            $s = json_decode(file_get_contents($confFile), true);
            if (!empty($s['timeout_active']) && isset($_SESSION['last_activity'])) {
                $timeout = (($s['timeout_minutes'] ?? 5) * 60);
                if (time() - $_SESSION['last_activity'] > $timeout) {
                    session_destroy();
                    if ($dieOnFail) {
                        http_response_code(403);
                        die(json_encode(['error' => 'Session expired']));
                    }
                    return false;
                }
            }
        }
        return true;
    }
    if ($dieOnFail) {
        http_response_code(403);
        die(json_encode(['error' => 'Access Denied']));
    }
    return false;
}

function getCSRFToken() {
    if (empty($_SESSION['csrf_token'])) $_SESSION['csrf_token'] = bin2hex(random_bytes(32));
    return $_SESSION['csrf_token'];
}

function validateCSRF() {
    $token = $_POST['csrf_token'] ?? $_SERVER['HTTP_X_CSRF_TOKEN'] ?? '';
    if (empty($token) || !hash_equals($_SESSION['csrf_token'] ?? '', $token)) {
        http_response_code(403);
        logUserAction("CSRF_BLOCKED", "Ungueltiger CSRF-Token");
        die(json_encode(['error' => 'Invalid CSRF token']));
    }
}

function checkRateLimit($maxRequests = 60, $windowSeconds = 60) {
    global $rateLimitDir;
    $ip = $_SERVER['REMOTE_ADDR'];
    $file = $rateLimitDir . md5($ip) . '.json';
    $now = time();
    $data = [];
    if (file_exists($file)) {
        $data = json_decode(file_get_contents($file), true) ?? [];
        $data = array_filter($data, function($t) use ($now, $windowSeconds) { return ($now - $t) < $windowSeconds; });
    }
    if (count($data) >= $maxRequests) {
        http_response_code(429);
        die(json_encode(['error' => 'Too many requests']));
    }
    $data[] = $now;
    file_put_contents($file, json_encode(array_values($data)));
}

/**
 * Robuste Auth-Header-Erkennung fuer alle Webserver-Konfigurationen
 */
function getAuthorizationHeader() {
    if (!empty($_SERVER['HTTP_AUTHORIZATION']))          return $_SERVER['HTTP_AUTHORIZATION'];
    if (!empty($_SERVER['REDIRECT_HTTP_AUTHORIZATION'])) return $_SERVER['REDIRECT_HTTP_AUTHORIZATION'];
    if (!empty($_SERVER['HTTP_X_ESP_TOKEN']))             return 'Bearer ' . $_SERVER['HTTP_X_ESP_TOKEN'];
    if (function_exists('getallheaders')) {
        $headers = getallheaders();
        if ($headers) {
            foreach ($headers as $key => $value) {
                $lower = strtolower($key);
                if ($lower === 'authorization') return $value;
                if ($lower === 'x-esp-token')   return 'Bearer ' . $value;
            }
        }
    }
    return '';
}

function validateESPToken() {
    global $confFile;
    $authHeader = getAuthorizationHeader();
    $token = '';
    if (preg_match('/^Bearer\s+(.+)$/i', $authHeader, $matches)) {
        $token = $matches[1];
    } elseif (!empty($authHeader)) {
        $token = $authHeader;
    }
    $settings = json_decode(file_get_contents($confFile), true) ?? [];
    $expectedToken = $settings['esp_token'] ?? '';
    if (empty($token) || empty($expectedToken) || !hash_equals($expectedToken, $token)) {
        http_response_code(401);
        $ip = $_SERVER['REMOTE_ADDR'];
        $entry = date("[d.m.Y H:i:s]") . " SECURITY: ESP auth failed from $ip (got " . strlen($token) . " chars)\n";
        file_put_contents('data/log.txt', $entry, FILE_APPEND);
        die(json_encode(['error' => 'Invalid device token']));
    }
}

function sanitizeNodeName($name) {
    $allowed = ['sender', 'receiver', 'camera'];
    $name = strtolower(trim($name));
    if (!in_array($name, $allowed)) {
        http_response_code(400);
        die(json_encode(['error' => 'Invalid node: ' . htmlspecialchars($name)]));
    }
    return $name;
}

function sanitizeCommand($cmd) {
    $allowed = ['ALARM_ON', 'ALARM_OFF', 'REBOOT', 'RESET', 'STATUS', 'UPDATE'];
    $cmd = strtoupper(trim($cmd));
    if (!in_array($cmd, $allowed)) {
        http_response_code(400);
        die(json_encode(['error' => 'Invalid command: ' . htmlspecialchars($cmd)]));
    }
    return $cmd;
}

function logUserAction($action, $details = "") {
    global $userLogFile;
    $logs = json_decode(file_get_contents($userLogFile), true) ?? [];
    $ip = $_SERVER['REMOTE_ADDR'];
    $agent = $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown';
    $device = gethostbyaddr($ip);
    if ($device == $ip) $device = "Unknown Device";
    $newLog = [
        'timestamp' => time(), 'date' => date("d.m.Y H:i:s"), 'ip' => $ip,
        'device_name' => $device, 'user_agent' => substr($agent, 0, 256),
        'action' => substr($action, 0, 50), 'details' => substr($details, 0, 256),
        'session_id' => session_id()
    ];
    array_unshift($logs, $newLog);
    $logs = array_slice($logs, 0, 100);
    file_put_contents($userLogFile, json_encode($logs));
}

// ============================================================
// SESSION
// ============================================================
ini_set('session.cookie_httponly', 1);
ini_set('session.cookie_samesite', 'Strict');
ini_set('session.use_strict_mode', 1);
if (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off') {
    ini_set('session.cookie_secure', 1);
}
session_start();

// ============================================================
// A. DASHBOARD AKTIONEN
// ============================================================

// Lighttpd CGI Workaround: $_POST kann leer sein
$_rawPostBody = '';
if ($_SERVER['REQUEST_METHOD'] === 'POST' && empty($_POST) && isset($_GET['action'])) {
    $_rawPostBody = file_get_contents('php://input');
    if (!empty($_rawPostBody)) {
        parse_str($_rawPostBody, $_POST);
    }
}

if (isset($_GET['action'])) {
    $action = $_GET['action'];

    // Kein Login noetig
    if ($action === 'ping_activity') {
        if (requireAuth(false)) {
            $_SESSION['last_activity'] = time();
            echo json_encode(['status' => 'ok']);
        }
        exit;
    }
    if ($action === 'get_csrf_token') {
        if (requireAuth(false)) echo json_encode(['csrf_token' => getCSRFToken()]);
        exit;
    }

    // Debug-Endpunkt: zeigt welche Headers ankommen
    if ($action === 'debug_headers') {
        requireAuth();
        $info = [
            'HTTP_AUTHORIZATION'          => $_SERVER['HTTP_AUTHORIZATION'] ?? '(not set)',
            'REDIRECT_HTTP_AUTHORIZATION' => $_SERVER['REDIRECT_HTTP_AUTHORIZATION'] ?? '(not set)',
            'HTTP_X_ESP_TOKEN'            => $_SERVER['HTTP_X_ESP_TOKEN'] ?? '(not set)',
            'resolved'                    => getAuthorizationHeader() ?: '(empty)',
            'sapi'                        => php_sapi_name(),
            'server'                      => $_SERVER['SERVER_SOFTWARE'] ?? 'unknown'
        ];
        header('Content-Type: application/json');
        echo json_encode($info, JSON_PRETTY_PRINT);
        exit;
    }

    // Debug-Endpunkt: testet POST-Datenempfang (kein CSRF noetig)
    if ($action === 'debug_post') {
        requireAuth();
        global $_rawPostBody;
        $info = [
            'REQUEST_METHOD'  => $_SERVER['REQUEST_METHOD'],
            'CONTENT_TYPE'    => $_SERVER['CONTENT_TYPE'] ?? $_SERVER['HTTP_CONTENT_TYPE'] ?? '(not set)',
            '_POST'           => $_POST,
            'raw_body'        => substr($_rawPostBody ?: '(not captured)', 0, 500),
            'csrf_in_post'    => $_POST['csrf_token'] ?? '(missing)',
            'csrf_in_header'  => $_SERVER['HTTP_X_CSRF_TOKEN'] ?? '(missing)',
            'csrf_in_session' => $_SESSION['csrf_token'] ?? '(missing)',
            'csrf_match'      => (!empty($_POST['csrf_token']) && hash_equals($_SESSION['csrf_token'] ?? '', $_POST['csrf_token'])) ? 'YES' : 'NO',
            'session_id'      => session_id(),
            'session_data'    => ['loggedin' => $_SESSION['loggedin'] ?? false, 'last_activity' => $_SESSION['last_activity'] ?? 0]
        ];
        header('Content-Type: application/json');
        echo json_encode($info, JSON_PRETTY_PRINT);
        exit;
    }

    // Ab hier: Login + Rate Limit
    requireAuth();
    checkRateLimit(60, 60);

    // Lesend (GET ok)
    if ($action === 'get_user_logs') {
        header('Content-Type: application/json');
        echo file_get_contents($userLogFile);
        exit;
    }
    if ($action === 'export_user_logs') {
        $logs = json_decode(file_get_contents($userLogFile), true) ?? [];
        header('Content-Type: text/csv; charset=utf-8');
        header('Content-Disposition: attachment; filename="user_logs_' . date('Y-m-d_H-i-s') . '.csv"');
        $out = fopen('php://output', 'w');
        fputcsv($out, ['Zeitstempel','Datum','IP','Geraet','Aktion','Details','Session','User Agent']);
        foreach ($logs as $l) fputcsv($out, [$l['timestamp'],$l['date'],$l['ip'],$l['device_name'],$l['action'],$l['details'],$l['session_id'],$l['user_agent']]);
        fclose($out);
        exit;
    }
    if ($action === 'get_esp_token') {
        $s = json_decode(file_get_contents($confFile), true);
        echo json_encode(['esp_token' => $s['esp_token'] ?? '']);
        exit;
    }

    // Schreibend: POST + CSRF
    if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
        http_response_code(405);
        die(json_encode(['error' => 'POST required']));
    }
    validateCSRF();

    if ($action === 'clear_logs' && isset($_POST['target'])) {
        $target = sanitizeNodeName($_POST['target']);
        $lines = file_exists($logFile) ? file($logFile) : [];
        $filtered = array_filter($lines, function($line) use ($target) {
            return strpos($line, $target . ':') === false;
        });
        file_put_contents($logFile, implode('', $filtered));
        logUserAction("Clear Logs", "Logs fuer '$target' geloescht");
        echo json_encode(['status' => 'ok', 'message' => "Logs geloescht"]);
        exit;
    }

    if ($action === 'clear_user_logs') {
        logUserAction("Clear User Logs", "Geloescht");
        file_put_contents($userLogFile, json_encode([]));
        echo json_encode(['status' => 'ok', 'message' => 'User-Logs geloescht.']);
        exit;
    }

    if ($action === 'save_settings' && isset($_POST['settings'])) {
        $new = json_decode($_POST['settings'], true);
        if (!$new) { http_response_code(400); die(json_encode(['error' => 'Invalid JSON'])); }
        $cur = json_decode(file_get_contents($confFile), true) ?? [];

        if (!empty($new['password'])) { $new['password'] = password_hash($new['password'], PASSWORD_BCRYPT); }
        else { $new['password'] = $cur['password']; }
        $new['esp_token'] = $cur['esp_token'] ?? bin2hex(random_bytes(16));

        if (!isset($new['timeout_active'])) $new['timeout_active'] = false;
        $new['timeout_minutes'] = min(max((int)($new['timeout_minutes'] ?? 5), 1), 120);
        $new['refresh_rate'] = max(1000, min((int)($new['refresh_rate'] ?? 2000), 30000));
        $new['camera_port'] = (int)($new['camera_port'] ?? $cur['camera_port'] ?? 8082);
        $new['site_title'] = htmlspecialchars(substr($new['site_title'] ?? 'IoT-AlarmSystem', 0, 50));

        $allowed = ['password','refresh_rate','site_title','timeout_active','timeout_minutes','esp_token','camera_port'];
        $final = [];
        foreach ($allowed as $k) $final[$k] = $new[$k] ?? $cur[$k] ?? null;

        file_put_contents($confFile, json_encode($final));
        logUserAction("Config", "Einstellungen geaendert");
        echo json_encode(['status' => 'ok', 'message' => 'Gespeichert.']);
        exit;
    }

    if ($action === 'save_node_config' && isset($_POST['target'])) {
        $target = sanitizeNodeName($_POST['target']);
        $config = json_decode($_POST['config'] ?? '{}', true);
        if (!$config) { http_response_code(400); die(json_encode(['error' => 'Invalid config'])); }

        $file = $dataDir . 'update_' . $target . '.json';
        file_put_contents($file, json_encode($config));
        logUserAction("Node Update", "Config fuer '$target'");
        echo json_encode(['status' => 'ok', 'message' => "Config fuer '$target' hinterlegt."]);
        exit;
    }

    if ($action === 'system_reset') {
        file_put_contents($statusFile, json_encode([]));
        file_put_contents($logFile, "");
        file_put_contents($cmdFile, json_encode([]));
        logUserAction("SYSTEM RESET", "Alle Daten geloescht");
        echo json_encode(['status' => 'ok', 'message' => 'System Reset.']);
        exit;
    }

    if ($action === 'send_command' && isset($_POST['target']) && isset($_POST['cmd'])) {
        $target = sanitizeNodeName($_POST['target']);
        $cmd = sanitizeCommand($_POST['cmd']);

        // Befehl IMMER speichern (auch wenn offline)
        $cmds = json_decode(file_get_contents($cmdFile), true) ?? [];
        $cmds[$target] = $cmd;
        file_put_contents($cmdFile, json_encode($cmds));

        $status = json_decode(file_get_contents($statusFile), true) ?? [];
        $isOnline = isset($status[$target]['last_seen']) && (time() - $status[$target]['last_seen'] < 30);

        logUserAction("Command", "'$cmd' -> '$target'" . ($isOnline ? "" : " (offline)"));
        $msg = $isOnline
            ? "'$cmd' an '$target' gesendet."
            : "'$cmd' fuer '$target' hinterlegt (offline - wird beim naechsten Heartbeat ausgefuehrt).";
        echo json_encode(['status' => 'ok', 'message' => $msg]);
        exit;
    }

    if ($action === 'regenerate_esp_token') {
        $s = json_decode(file_get_contents($confFile), true) ?? [];
        $s['esp_token'] = bin2hex(random_bytes(16));
        file_put_contents($confFile, json_encode($s));
        logUserAction("Security", "ESP-Token regeneriert");
        echo json_encode(['status' => 'ok', 'token' => $s['esp_token']]);
        exit;
    }

    if ($action === 'pi_reboot') {
        logUserAction("PI REBOOT", "Neustart ausgeloest");
        echo json_encode(['status' => 'ok', 'message' => 'Pi wird neugestartet...']);
        if (function_exists('fastcgi_finish_request')) fastcgi_finish_request();
        exec('sudo /sbin/reboot 2>&1 &');
        exit;
    }

    http_response_code(400);
    die(json_encode(['error' => 'Unknown action']));
}

// ============================================================
// B. ESP KOMMUNIKATION
// ============================================================
if ($_SERVER['REQUEST_METHOD'] === 'POST' && !isset($_GET['action'])) {
    checkRateLimit(60, 60);
    validateESPToken();

    $json = file_get_contents('php://input');
    if (strlen($json) > 4096) { http_response_code(413); die(json_encode(['error' => 'Too large'])); }

    $data = json_decode($json, true);
    if ($data && isset($data['source'])) {
        $allowedSources = ['sender', 'receiver', 'camera'];
        $source = strtolower(trim($data['source']));
        if (!in_array($source, $allowedSources)) { http_response_code(400); die(json_encode(['error' => 'Invalid source'])); }

        if (isset($data['log'])) {
            $logMsg = preg_replace('/[\x00-\x1F\x7F]/', '', substr($data['log'], 0, 256));
            $entry = date("[d.m.Y H:i:s]") . " $source: " . $logMsg . "\n";
            if (file_exists($logFile) && filesize($logFile) > 500000) {
                file_put_contents($logFile, implode('', array_slice(file($logFile), -200)));
            }
            file_put_contents($logFile, $entry, FILE_APPEND);
        }

        if (isset($data['status_msg'])) {
            $sd = [
                'last_seen' => time(),
                'ip'        => filter_var($data['ip'] ?? '0.0.0.0', FILTER_VALIDATE_IP) ?: '0.0.0.0',
                'status'    => substr($data['status_msg'] ?? '', 0, 128),
                'alarm'     => ($source === "receiver" && isset($data['alarm_state'])) ? (bool)$data['alarm_state'] : false,
                'rssi'      => (int)($data['rssi'] ?? 0),
                'heap'      => (int)($data['heap'] ?? 0),
                'reset_reason' => substr($data['reset_reason'] ?? 'unknown', 0, 32),
                'uptime'    => (int)($data['uptime'] ?? 0)
            ];
            $cs = json_decode(file_get_contents($statusFile), true) ?? [];
            
            // Auto-Log: Heartbeat alle 60s loggen (damit Receiver im Log erscheint)
            $lastLogTime = $cs[$source]['last_log_time'] ?? 0;
            $statusChanged = ($cs[$source]['status'] ?? '') !== $sd['status'];
            if (!isset($data['log']) && (time() - $lastLogTime >= 60 || $statusChanged)) {
                $autoMsg = $sd['status'] ?: 'Heartbeat';
                $entry = date("[d.m.Y H:i:s]") . " $source: $autoMsg\n";
                if (file_exists($logFile) && filesize($logFile) > 500000) {
                    file_put_contents($logFile, implode('', array_slice(file($logFile), -200)));
                }
                file_put_contents($logFile, $entry, FILE_APPEND);
                $sd['last_log_time'] = time();
            } else {
                $sd['last_log_time'] = $cs[$source]['last_log_time'] ?? time();
            }
            
            $cs[$source] = array_merge($cs[$source] ?? [], $sd);
            file_put_contents($statusFile, json_encode($cs));

            $csvFile = $dataDir . 'telemetry.csv';
            if (file_exists($csvFile) && filesize($csvFile) > 1000000) rename($csvFile, $csvFile . ".old");
            file_put_contents($csvFile, time().",$source,{$sd['rssi']},{$sd['heap']}\n", FILE_APPEND);
        }

        $response = ["logging_active" => true];
        $cmds = json_decode(file_get_contents($cmdFile), true) ?? [];
        if (isset($cmds[$source])) {
            $response["command"] = $cmds[$source];
            unset($cmds[$source]);
            file_put_contents($cmdFile, json_encode($cmds));
        }
        $updateFile = $dataDir . 'update_' . $source . '.json';
        if (file_exists($updateFile)) {
            $response["new_config"] = json_decode(file_get_contents($updateFile), true);
            unlink($updateFile);
        }

        header('Content-Type: application/json');
        echo json_encode($response);
    } else {
        http_response_code(400);
        echo json_encode(['error' => 'Invalid payload']);
    }
    exit;
}

// ============================================================
// C. GET ALL (Dashboard)
// ============================================================
if ($_SERVER['REQUEST_METHOD'] === 'GET' && isset($_GET['get']) && $_GET['get'] === 'all') {
    requireAuth();

    $status = json_decode(file_get_contents($statusFile), true) ?? [];
    foreach ($status as $k => $v) {
        $status[$k]['online'] = (isset($v['last_seen']) && time() - $v['last_seen'] < 30);
    }

    // Pi: immer online
    $piUptime = 0;
    if (file_exists('/proc/uptime')) {
        $parts = explode(' ', file_get_contents('/proc/uptime'));
        $piUptime = (int)floatval($parts[0]);
    }
    $status['pi'] = [
        'last_seen' => time(),
        'ip' => $_SERVER['SERVER_ADDR'] ?? '127.0.0.1',
        'status' => 'Running',
        'online' => true,
        'rssi' => 0, 'heap' => 0,
        'uptime' => $piUptime,
        'reset_reason' => 'N/A'
    ];

    $logs = file_exists($logFile) ? array_slice(file($logFile), -20) : [];
    $settings = json_decode(file_get_contents($confFile), true);
    unset($settings['password'], $settings['esp_token']);

    header('Content-Type: application/json');
    echo json_encode(["status" => $status, "logs" => $logs, "config" => $settings]);
    exit;
}
?>
