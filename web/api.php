<?php
// api.php - V4.1 (Auto-Logout mit Inaktivitäts-Timer)

$dataDir = 'data/';
if (!is_dir($dataDir)) mkdir($dataDir, 0777, true);

$statusFile = $dataDir . 'status.json';
$logFile    = $dataDir . 'log.txt';
$userLogFile= $dataDir . 'user_logs.json';
$cmdFile    = $dataDir . 'commands.json';
$confFile   = $dataDir . 'settings.json';

// Init Files
if (!file_exists($statusFile)) file_put_contents($statusFile, json_encode([]));
if (!file_exists($cmdFile))    file_put_contents($cmdFile, json_encode([]));
if (!file_exists($userLogFile)) file_put_contents($userLogFile, json_encode([]));

// Standard-Einstellungen (inkl. Timeout)
if (!file_exists($confFile)) {
    $defaults = [
        "password" => "admin", 
        "refresh_rate" => 2000, 
        "site_title" => "IoT-AlarmSystem",
        "timeout_active" => false,
        "timeout_minutes" => 5
    ];
    file_put_contents($confFile, json_encode($defaults));
}

// --- HELPER: User Action Logger ---
function logUserAction($action, $details = "") {
    global $userLogFile;
    $logs = json_decode(file_get_contents($userLogFile), true) ?? [];
    
    $ip = $_SERVER['REMOTE_ADDR'];
    $agent = $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown';
    $device = gethostbyaddr($ip);
    if($device == $ip) $device = "Unknown Device";

    $newLog = [
        'timestamp' => time(),
        'date' => date("d.m.Y H:i:s"),
        'ip' => $ip,
        'device_name' => $device,
        'user_agent' => $agent,
        'action' => $action,
        'details' => $details,
        'session_id' => session_id()
    ];

    array_unshift($logs, $newLog);
    $logs = array_slice($logs, 0, 100);
    file_put_contents($userLogFile, json_encode($logs));
}
// ------------------------------------

session_start();

// A. DASHBOARD AKTIONEN
if (isset($_GET['action'])) {
    
    // Activity Ping (für Timeout-Reset)
    if ($_GET['action'] == 'ping_activity') {
        if (isset($_SESSION['loggedin']) && $_SESSION['loggedin'] === true) {
            $_SESSION['last_activity'] = time();
            echo json_encode(['status' => 'ok', 'time' => time()]);
        }
        exit;
    }

    // Clear Logs
    if ($_GET['action'] == 'clear_logs' && isset($_GET['target'])) {
        $target = $_GET['target'];
        $lines = file_exists($logFile) ? file($logFile) : [];
        $filtered = array_filter($lines, function($line) use ($target) {
            return strpos($line, $target . ':') === false;
        });
        file_put_contents($logFile, implode('', $filtered));
        logUserAction("Clear Logs", "Logs für '$target' gelöscht");
        exit;
    }

    // User Logs abrufen
    if ($_GET['action'] == 'get_user_logs') {
        if (!isset($_SESSION['loggedin']) || $_SESSION['loggedin'] !== true) die("Access Denied");
        echo file_get_contents($userLogFile);
        exit;
    }

    // User Logs exportieren
    if ($_GET['action'] == 'export_user_logs') {
        if (!isset($_SESSION['loggedin']) || $_SESSION['loggedin'] !== true) die("Access Denied");
        
        $logs = json_decode(file_get_contents($userLogFile), true) ?? [];
        
        header('Content-Type: text/csv; charset=utf-8');
        header('Content-Disposition: attachment; filename="user_logs_'.date('Y-m-d_H-i-s').'.csv"');
        
        $output = fopen('php://output', 'w');
        fputcsv($output, ['Zeitstempel', 'Datum', 'IP', 'Gerätename', 'Aktion', 'Details', 'Session-ID', 'User Agent']);
        
        foreach ($logs as $log) {
            fputcsv($output, [
                $log['timestamp'],
                $log['date'],
                $log['ip'],
                $log['device_name'],
                $log['action'],
                $log['details'],
                $log['session_id'],
                $log['user_agent']
            ]);
        }
        
        fclose($output);
        logUserAction("Export User Logs", "User-Logs als CSV exportiert");
        exit;
    }

    // User Logs löschen
    if ($_GET['action'] == 'clear_user_logs') {
        if (!isset($_SESSION['loggedin']) || $_SESSION['loggedin'] !== true) die("Access Denied");
        
        logUserAction("Clear User Logs", "Alle User-Logs wurden gelöscht!");
        file_put_contents($userLogFile, json_encode([]));
        echo "✅ Benutzer-Logs wurden gelöscht.";
        exit;
    }

    // Settings speichern
    if ($_GET['action'] == 'save_settings' && isset($_POST['settings'])) {
        $newSettings = json_decode($_POST['settings'], true);
        $currentSettings = json_decode(file_get_contents($confFile), true) ?? [];
        
        if (empty($newSettings['password'])) $newSettings['password'] = $currentSettings['password'];
        
        // Timeout Einstellungen validieren
        if (!isset($newSettings['timeout_active'])) $newSettings['timeout_active'] = false;
        if (!isset($newSettings['timeout_minutes']) || $newSettings['timeout_minutes'] < 1) {
            $newSettings['timeout_minutes'] = 5;
        }

        $finalSettings = array_merge($currentSettings, $newSettings);
        file_put_contents($confFile, json_encode($finalSettings));
        
        $timeoutStatus = $finalSettings['timeout_active'] ? "aktiviert ({$finalSettings['timeout_minutes']} Min)" : "deaktiviert";
        logUserAction("Config Change", "Dashboard Einstellungen geändert (Timeout: $timeoutStatus)");
        echo "Einstellungen gespeichert.";
        exit;
    }

    // Node Config speichern
    if ($_GET['action'] == 'save_node_config' && isset($_POST['target'])) {
        $target = $_POST['target'];
        
        // Online Check
        $status = json_decode(file_get_contents($statusFile), true) ?? [];
        $isOnline = isset($status[$target]['last_seen']) && (time() - $status[$target]['last_seen'] < 15);
        
        if (!$isOnline) {
            echo "❌ Fehler: '$target' ist OFFLINE!";
            exit;
        }

        $file = $dataDir . 'update_' . $target . '.json';
        file_put_contents($file, $_POST['config']);
        
        logUserAction("Node Update", "Neue Config für '$target' bereitgestellt.");
        echo "✅ Update für '$target' hinterlegt.";
        exit;
    }

    // System Reset
    if ($_GET['action'] == 'system_reset') {
        file_put_contents($statusFile, json_encode([]));
        file_put_contents($logFile, "");
        file_put_contents($cmdFile, json_encode([]));
        
        logUserAction("SYSTEM RESET", "Alle Daten gelöscht!");
        
        // User Logs bleiben bestehen für Audit Trail
        echo "System Reset durchgeführt.";
        exit;
    }

    // Befehl senden
    if ($_GET['action'] == 'send_command' && isset($_GET['target']) && isset($_GET['cmd'])) {
        $target = $_GET['target'];
        $cmd = $_GET['cmd'];
        
        // Online Check
        $status = json_decode(file_get_contents($statusFile), true) ?? [];
        $isOnline = isset($status[$target]['last_seen']) && (time() - $status[$target]['last_seen'] < 15);
        
        if (!$isOnline && !in_array($cmd, ['ALARM_ON', 'ALARM_OFF'])) {
            echo "❌ Fehler: '$target' ist OFFLINE!";
            exit;
        }

        $cmds = json_decode(file_get_contents($cmdFile), true) ?? [];
        $cmds[$target] = $cmd;
        file_put_contents($cmdFile, json_encode($cmds));
        
        logUserAction("Command Sent", "Befehl '$cmd' an '$target'");
        echo "Befehl gesendet.";
        exit;
    }
}

// B. ESP KOMMUNIKATION (POST)
if ($_SERVER['REQUEST_METHOD'] === 'POST' && !isset($_GET['action'])) {
    $json = file_get_contents('php://input');
    $data = json_decode($json, true);

    if ($data && isset($data['source'])) {
        $source = $data['source'];

        // Logging (System Log)
        if (isset($data['log'])) {
            $entry = date("[d.m.Y H:i:s]") . " $source: " . $data['log'] . "\n";
            file_put_contents($logFile, $entry, FILE_APPEND);
        }

        // Status Update & Telemetrie History
        if (isset($data['status_msg'])) {
            $statusData = [
                'last_seen' => time(),
                'ip' => $data['ip'] ?? '0.0.0.0',
                'status' => $data['status_msg'],
                'alarm' => ($source == "receiver" && isset($data['alarm_state'])) ? $data['alarm_state'] : false,
                'rssi' => $data['rssi'] ?? 0,
                'heap' => $data['heap'] ?? 0,
                'reset_reason' => $data['reset_reason'] ?? 'unknown',
                'uptime' => $data['uptime'] ?? 0
            ];

            $currentStatus = json_decode(file_get_contents($statusFile), true) ?? [];
            if (!isset($currentStatus[$source])) $currentStatus[$source] = [];
            $currentStatus[$source] = array_merge($currentStatus[$source], $statusData);
            file_put_contents($statusFile, json_encode($currentStatus));

            // CSV History
            $csvLine = time() . "," . $source . "," . $statusData['rssi'] . "," . $statusData['heap'] . "\n";
            $csvFile = $dataDir . 'telemetry.csv';
            if (file_exists($csvFile) && filesize($csvFile) > 1000000) rename($csvFile, $csvFile . ".old"); 
            file_put_contents($csvFile, $csvLine, FILE_APPEND);
        }

        // ANTWORT BAUEN
        $response = ["logging_active" => true];

        // Befehle prüfen
        $cmds = json_decode(file_get_contents($cmdFile), true) ?? [];
        if (isset($cmds[$source])) {
            $response["command"] = $cmds[$source];
            unset($cmds[$source]);
            file_put_contents($cmdFile, json_encode($cmds));
        }

        // Config Update prüfen
        $updateFile = $dataDir . 'update_' . $source . '.json';
        if (file_exists($updateFile)) {
            $response["new_config"] = json_decode(file_get_contents($updateFile), true);
            unlink($updateFile);
        }

        header('Content-Type: application/json');
        echo json_encode($response);
    }
}

// C. GET ALL (für Dashboard AJAX)
if ($_SERVER['REQUEST_METHOD'] === 'GET' && isset($_GET['get']) && $_GET['get'] == 'all') {
    $status = json_decode(file_get_contents($statusFile), true);
    if($status) {
        foreach ($status as $k => $v) $status[$k]['online'] = (isset($v['last_seen']) && time() - $v['last_seen'] < 15);
    }
    $logs = file_exists($logFile) ? array_slice(file($logFile), -20) : [];
    $settings = json_decode(file_get_contents($confFile), true);
    echo json_encode(["status" => $status, "logs" => $logs, "config" => $settings]);
}
?>