<?php
ini_set('display_errors', 0);
ini_set('display_startup_errors', 0);
ini_set('log_errors', 1);
error_reporting(E_ALL);

ini_set('session.cookie_httponly', 1);
ini_set('session.cookie_samesite', 'Strict');
ini_set('session.use_strict_mode', 1);
if (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off') {
    ini_set('session.cookie_secure', 1);
}
session_start();

// EINSTELLUNGEN LADEN
$confFile = 'data/settings.json';
$defaults = [
    "password" => password_hash("admin", PASSWORD_BCRYPT), 
    "refresh_rate" => 2000, 
    "site_title" => "IoT Control Center",
    "timeout_active" => true,
    "timeout_minutes" => 5,
    "esp_token" => bin2hex(random_bytes(16)),
    "camera_port" => 8082
];

$settings = [];
if (file_exists($confFile)) {
    $settings = json_decode(file_get_contents($confFile), true);
}
if (!$settings || !isset($settings['password'])) {
    $settings = $defaults;
    if (!is_dir('data')) mkdir('data', 0750, true);
    file_put_contents($confFile, json_encode($settings));
}

// LOGIN CHECK
$loginFailed = false;
if (isset($_POST['password'])) {
    $loginAttemptFile = 'data/login_attempts.json';
    $ip = $_SERVER['REMOTE_ADDR'];
    $attempts = file_exists($loginAttemptFile) ? json_decode(file_get_contents($loginAttemptFile), true) : [];
    $now = time();
    if (isset($attempts[$ip])) {
        $attempts[$ip] = array_values(array_filter($attempts[$ip], function($t) use ($now) { return ($now - $t) < 300; }));
    }
    $blocked = isset($attempts[$ip]) && count($attempts[$ip]) >= 5;

    if (!$blocked && password_verify($_POST['password'], $settings['password'])) {
        session_regenerate_id(true);
        $_SESSION['loggedin'] = true;
        $_SESSION['last_activity'] = time();
        $_SESSION['login_time'] = time();
        $_SESSION['csrf_token'] = bin2hex(random_bytes(32));
        unset($attempts[$ip]);
        file_put_contents($loginAttemptFile, json_encode($attempts));

        $agent = $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown';
        $device = gethostbyaddr($ip);
        if ($device == $ip) $device = "Unknown Device";
        $userLogFile = 'data/user_logs.json';
        $logs = file_exists($userLogFile) ? json_decode(file_get_contents($userLogFile), true) : [];
        array_unshift($logs, [
            'timestamp' => time(), 'date' => date("d.m.Y H:i:s"), 'ip' => $ip,
            'device_name' => $device, 'user_agent' => $agent,
            'action' => 'LOGIN', 'details' => 'Erfolgreicher Login', 'session_id' => session_id()
        ]);
        $logs = array_slice($logs, 0, 100);
        if (!is_dir('data')) mkdir('data', 0750, true);
        file_put_contents($userLogFile, json_encode($logs));
    } else {
        if (!isset($attempts[$ip])) $attempts[$ip] = [];
        $attempts[$ip][] = $now;
        if (!is_dir('data')) mkdir('data', 0750, true);
        file_put_contents($loginAttemptFile, json_encode($attempts));
        $loginFailed = true;
    }
}

// TIMEOUT CHECK
if (isset($_SESSION['loggedin']) && $_SESSION['loggedin'] === true) {
    if ($settings['timeout_active'] && isset($_SESSION['last_activity'])) {
        $timeout_seconds = ($settings['timeout_minutes'] ?? 5) * 60;
        if (time() - $_SESSION['last_activity'] > $timeout_seconds) {
            $userLogFile = 'data/user_logs.json';
            $logs = file_exists($userLogFile) ? json_decode(file_get_contents($userLogFile), true) : [];
            
            $ip = $_SERVER['REMOTE_ADDR'];
            $agent = $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown';
            $device = gethostbyaddr($ip);
            if($device == $ip) $device = "Unknown Device";
            
            $sessionDuration = isset($_SESSION['login_time']) ? (time() - $_SESSION['login_time']) : 0;
            $durationStr = $sessionDuration > 0 ? gmdate("H:i:s", $sessionDuration) : "0s";
            
            $newLog = [
                'timestamp' => time(),
                'date' => date("d.m.Y H:i:s"),
                'ip' => $ip,
                'device_name' => $device,
                'user_agent' => $agent,
                'action' => 'AUTO-LOGOUT',
                'details' => "Timeout nach Inaktivität (Sitzungsdauer: $durationStr)",
                'session_id' => session_id()
            ];
            array_unshift($logs, $newLog);
            $logs = array_slice($logs, 0, 100);
            if (!is_dir('data')) mkdir("data", 0750, true);
            file_put_contents($userLogFile, json_encode($logs));
            
            session_destroy();
            header("Location: index.php?timeout=1");
            exit;
        }
    }
    $_SESSION['last_activity'] = time();
}

if (isset($_GET['logout'])) {
    $userLogFile = 'data/user_logs.json';
    $logs = file_exists($userLogFile) ? json_decode(file_get_contents($userLogFile), true) : [];
    
    $ip = $_SERVER['REMOTE_ADDR'];
    $agent = $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown';
    $device = gethostbyaddr($ip);
    if($device == $ip) $device = "Unknown Device";
    
    $sessionDuration = isset($_SESSION['login_time']) ? (time() - $_SESSION['login_time']) : 0;
    $durationStr = $sessionDuration > 0 ? gmdate("H:i:s", $sessionDuration) : "0s";
    
    $newLog = [
        'timestamp' => time(),
        'date' => date("d.m.Y H:i:s"),
        'ip' => $ip,
        'device_name' => $device,
        'user_agent' => $agent,
        'action' => 'LOGOUT',
        'details' => "Sitzungsdauer: $durationStr",
        'session_id' => session_id()
    ];
    array_unshift($logs, $newLog);
    $logs = array_slice($logs, 0, 100);
    file_put_contents($userLogFile, json_encode($logs));
    
    session_destroy();
    header("Location: index.php");
    exit;
}

// LOGIN MASKE
if (!isset($_SESSION['loggedin']) || $_SESSION['loggedin'] !== true) {
    $timeoutMsg = isset($_GET['timeout']) ? '<div class="alert-warning"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><path d="M12 6v6l4 2"/></svg><span>Session expired due to inactivity</span></div>' : '';
    $failedMsg = (isset($loginFailed) && $loginFailed) ? '<div class="alert-warning" style="background:#fee2e2;border-color:#ef4444;color:#991b1b;"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><path d="M15 9l-6 6M9 9l6 6"/></svg><span>Invalid password</span></div>' : '';
    echo '<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>Login</title><link rel="preconnect" href="https://fonts.googleapis.com"><link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet"><style>*{margin:0;padding:0;box-sizing:border-box;}body{font-family:"Inter",sans-serif;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;}.login-container{background:rgba(255,255,255,0.95);backdrop-filter:blur(10px);padding:40px;border-radius:16px;box-shadow:0 20px 60px rgba(0,0,0,0.3);width:100%;max-width:400px;}.login-header{text-align:center;margin-bottom:32px;}.login-header svg{width:48px;height:48px;margin-bottom:16px;color:#667eea;}.login-header h1{font-size:24px;font-weight:700;color:#1a1a1a;margin-bottom:8px;}.login-header p{color:#666;font-size:14px;}.alert-warning{display:flex;align-items:center;gap:12px;padding:12px 16px;background:#fff3cd;border:1px solid #ffc107;border-radius:8px;color:#856404;font-size:14px;margin-bottom:20px;}.alert-warning svg{flex-shrink:0;}.form-group{margin-bottom:20px;}.form-label{display:block;font-weight:500;font-size:14px;color:#333;margin-bottom:8px;}.form-input{width:100%;padding:12px 16px;border:2px solid #e0e0e0;border-radius:8px;font-size:15px;font-family:inherit;transition:border-color 0.2s;}.form-input:focus{outline:none;border-color:#667eea;}.btn-primary{width:100%;padding:14px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);color:white;border:none;border-radius:8px;font-size:15px;font-weight:600;cursor:pointer;transition:transform 0.2s,box-shadow 0.2s;}.btn-primary:hover{transform:translateY(-2px);box-shadow:0 10px 20px rgba(102,126,234,0.3);}.btn-primary:active{transform:translateY(0);}</style></head><body><div class="login-container"><div class="login-header"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/></svg><h1>Secure Login</h1><p>Access Control Center</p></div>' . $timeoutMsg . $failedMsg . '<form method="post"><div class="form-group"><label class="form-label">Password</label><input type="password" name="password" class="form-input" placeholder="Enter password" required autofocus></div><button type="submit" class="btn-primary">Sign In</button></form></div></body></html>';
    exit;
}

// DIAGNOSE DATEN LADEN
$statusFile = 'data/status.json';
$logFile = 'data/log.txt';
$csvFile = 'data/telemetry.csv';

$diagStatus = [];
if (file_exists($statusFile)) {
    $diagStatus = json_decode(file_get_contents($statusFile), true);
}

$chartData = [
    'sender' => ['rssi' => [], 'heap' => [], 'time' => []],
    'receiver' => ['rssi' => [], 'heap' => [], 'time' => []],
    'camera' => ['rssi' => [], 'heap' => [], 'time' => []]
];

if (file_exists($csvFile)) {
    $lines = file($csvFile);
    $lines = array_slice($lines, -100);
    
    foreach ($lines as $line) {
        $parts = explode(",", trim($line));
        if (count($parts) >= 4) {
            $timestamp = (int)$parts[0];
            $source = trim($parts[1]);
            $rssi = (int)$parts[2];
            $heap = (int)$parts[3];
            
            if (isset($chartData[$source])) {
                $chartData[$source]['time'][] = $timestamp;
                $chartData[$source]['rssi'][] = $rssi;
                $chartData[$source]['heap'][] = $heap;
            }
        }
    }
}

$systemLogs = [];
if (file_exists($logFile)) {
    $systemLogs = array_slice(file($logFile), -50);
}
?>

<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title><?php echo htmlspecialchars($settings['site_title']); ?></title>
    <meta name="csrf-token" content="<?php echo htmlspecialchars($_SESSION['csrf_token'] ?? ''); ?>">
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <script src="https://unpkg.com/lucide@latest"></script>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        :root {
            --bg-primary: #0f172a;
            --bg-secondary: #1e293b;
            --bg-tertiary: #334155;
            --text-primary: #f1f5f9;
            --text-secondary: #94a3b8;
            --text-tertiary: #64748b;
            --accent-blue: #3b82f6;
            --accent-green: #10b981;
            --accent-yellow: #f59e0b;
            --accent-red: #ef4444;
            --accent-purple: #8b5cf6;
            --border-color: #334155;
            --shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.3);
            --shadow-lg: 0 10px 15px -3px rgba(0, 0, 0, 0.4);
        }
        
        * { margin: 0; padding: 0; box-sizing: border-box; }
        
        body {
            font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
            background: var(--bg-primary);
            color: var(--text-primary);
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            -webkit-font-smoothing: antialiased;
        }
        
        /* === HEADER === */
        .header {
            background: var(--bg-secondary);
            border-bottom: 1px solid var(--border-color);
            padding: 0 24px;
            height: 64px;
            display: flex;
            align-items: center;
            justify-content: space-between;
            position: sticky;
            top: 0;
            z-index: 100;
            backdrop-filter: blur(8px);
        }
        
        .header-left {
            display: flex;
            align-items: center;
            gap: 32px;
        }
        
        .logo {
            display: flex;
            align-items: center;
            gap: 12px;
            font-size: 18px;
            font-weight: 700;
            color: var(--text-primary);
        }
        
        .logo svg {
            width: 24px;
            height: 24px;
            color: var(--accent-blue);
        }
        
        /* === NAVIGATION === */
        .nav-tabs {
            display: flex;
            gap: 4px;
        }
        
        .tab-btn {
            background: none;
            border: none;
            color: var(--text-secondary);
            padding: 8px 16px;
            border-radius: 6px;
            cursor: pointer;
            font-size: 14px;
            font-weight: 500;
            transition: all 0.2s;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        
        .tab-btn:hover {
            background: var(--bg-tertiary);
            color: var(--text-primary);
        }
        
        .tab-btn.active {
            background: var(--accent-blue);
            color: white;
        }
        
        .tab-btn svg {
            width: 16px;
            height: 16px;
        }
        
        .btn-logout {
            padding: 8px 16px;
            background: transparent;
            border: 1px solid var(--border-color);
            border-radius: 6px;
            color: var(--text-secondary);
            text-decoration: none;
            font-size: 14px;
            font-weight: 500;
            transition: all 0.2s;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        
        .btn-logout:hover {
            border-color: var(--accent-red);
            color: var(--accent-red);
        }
        
        .btn-logout svg {
            width: 16px;
            height: 16px;
        }
        
        /* === MAIN CONTENT === */
        .main-content {
            flex: 1;
            padding: 32px;
            overflow-y: auto;
        }
        
        .container {
            max-width: 1400px;
            margin: 0 auto;
        }
        
        .page-header {
            margin-bottom: 32px;
        }
        
        .page-title {
            font-size: 28px;
            font-weight: 700;
            color: var(--text-primary);
            margin-bottom: 8px;
        }
        
        .page-subtitle {
            font-size: 14px;
            color: var(--text-secondary);
        }
        
        .grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(350px, 1fr));
            gap: 24px;
        }
        
        .grid-2 {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(500px, 1fr));
            gap: 24px;
        }
        
        /* === CARDS === */
        .card {
            background: var(--bg-secondary);
            border: 1px solid var(--border-color);
            border-radius: 12px;
            padding: 24px;
            transition: all 0.3s;
            position: relative;
            overflow: hidden;
        }
        
        .card::before {
            content: '';
            position: absolute;
            top: 0;
            left: 0;
            right: 0;
            height: 3px;
            background: var(--border-color);
            transition: all 0.3s;
        }
        
        .card.online::before { background: var(--accent-green); }
        .card.sender.online::before { background: var(--accent-blue); }
        .card.receiver.online::before { background: var(--accent-yellow); }
        .card.camera.online::before { background: var(--accent-purple); }
        
        .card:hover {
            border-color: var(--bg-tertiary);
            transform: translateY(-2px);
            box-shadow: var(--shadow-lg);
        }
        
        .card-header {
            display: flex;
            align-items: center;
            justify-content: space-between;
            margin-bottom: 20px;
        }
        
        .card-title {
            display: flex;
            align-items: center;
            gap: 12px;
            font-size: 16px;
            font-weight: 600;
            color: var(--text-primary);
        }
        
        .card-title svg {
            width: 20px;
            height: 20px;
            color: var(--accent-blue);
        }
        
        .status-indicator {
            width: 10px;
            height: 10px;
            border-radius: 50%;
            background: var(--accent-red);
            box-shadow: 0 0 10px currentColor;
            animation: pulse 2s infinite;
        }
        
        .status-indicator.online {
            background: var(--accent-green);
        }
        
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
        
        .card-content {
            margin-bottom: 20px;
        }
        
        .info-row {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 8px 0;
            border-bottom: 1px solid var(--border-color);
            font-size: 14px;
        }
        
        .info-row:last-child {
            border-bottom: none;
        }
        
        .info-label {
            color: var(--text-secondary);
            font-weight: 500;
        }
        
        .info-value {
            color: var(--text-primary);
            font-weight: 600;
        }
        
        .status-message {
            padding: 12px 16px;
            background: var(--bg-tertiary);
            border-radius: 8px;
            border-left: 3px solid var(--accent-blue);
            font-size: 14px;
            font-weight: 500;
            margin-bottom: 16px;
        }
        
        /* === BUTTONS === */
        .btn-group {
            display: flex;
            gap: 8px;
            flex-wrap: wrap;
        }
        
        .btn {
            flex: 1;
            padding: 10px 16px;
            border: 1px solid var(--border-color);
            border-radius: 8px;
            background: var(--bg-tertiary);
            color: var(--text-primary);
            font-size: 13px;
            font-weight: 500;
            cursor: pointer;
            transition: all 0.2s;
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 8px;
            text-decoration: none;
        }
        
        .btn svg {
            width: 16px;
            height: 16px;
        }
        
        .btn:hover {
            background: var(--bg-primary);
            transform: translateY(-1px);
        }
        
        .btn-danger {
            border-color: var(--accent-red);
            color: var(--accent-red);
        }
        
        .btn-danger:hover {
            background: var(--accent-red);
            color: white;
        }
        
        .btn-primary {
            background: var(--accent-blue);
            border-color: var(--accent-blue);
            color: white;
        }
        
        .btn-primary:hover {
            background: #2563eb;
        }
        
        /* === TOGGLE SWITCH === */
        .toggle-wrapper {
            flex: 1;
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 8px;
            padding: 10px 16px;
            background: var(--bg-tertiary);
            border: 1px solid var(--border-color);
            border-radius: 8px;
        }
        
        .toggle-wrapper svg {
            width: 16px;
            height: 16px;
            color: var(--text-secondary);
        }
        
        .switch {
            position: relative;
            width: 44px;
            height: 24px;
        }
        
        .switch input {
            opacity: 0;
            width: 0;
            height: 0;
        }
        
        .slider {
            position: absolute;
            cursor: pointer;
            top: 0;
            left: 0;
            right: 0;
            bottom: 0;
            background: var(--bg-primary);
            border: 2px solid var(--border-color);
            transition: 0.3s;
            border-radius: 24px;
        }
        
        .slider:before {
            position: absolute;
            content: "";
            height: 16px;
            width: 16px;
            left: 2px;
            bottom: 2px;
            background: var(--text-secondary);
            transition: 0.3s;
            border-radius: 50%;
        }
        
        input:checked + .slider {
            background: var(--accent-yellow);
            border-color: var(--accent-yellow);
        }
        
        input:checked + .slider:before {
            transform: translateX(20px);
            background: white;
        }
        
        /* === TERMINAL/LOGS === */
        .terminal {
            background: #0a0e1a;
            border: 1px solid var(--border-color);
            border-radius: 8px;
            padding: 16px;
            height: 320px;
            overflow-y: auto;
            font-family: 'Courier New', monospace;
            font-size: 13px;
            color: var(--accent-green);
        }
        
        .terminal-header {
            font-weight: 600;
            color: var(--text-primary);
            margin-bottom: 12px;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        
        .terminal-header svg {
            width: 16px;
            height: 16px;
        }
        
        .log-line {
            padding: 4px 0;
            border-bottom: 1px solid rgba(255,255,255,0.05);
            opacity: 0.8;
        }
        
        .terminal-actions {
            margin-top: 12px;
        }
        
        /* === FORMS === */
        .view-section {
            display: none;
        }
        
        .form-group {
            margin-bottom: 20px;
        }
        
        .form-label {
            display: block;
            font-size: 13px;
            font-weight: 500;
            color: var(--text-secondary);
            margin-bottom: 8px;
        }
        
        .form-input,
        .form-select {
            width: 100%;
            padding: 10px 14px;
            background: var(--bg-primary);
            border: 1px solid var(--border-color);
            border-radius: 8px;
            color: var(--text-primary);
            font-size: 14px;
            font-family: inherit;
            transition: border-color 0.2s;
        }
        
        .form-input:focus,
        .form-select:focus {
            outline: none;
            border-color: var(--accent-blue);
        }
        
        .form-hint {
            font-size: 12px;
            color: var(--text-tertiary);
            margin-top: 6px;
        }
        
        /* === ALERT BOXES === */
        .alert {
            padding: 14px 18px;
            border-radius: 8px;
            margin-bottom: 20px;
            display: flex;
            align-items: center;
            gap: 12px;
            font-size: 14px;
        }
        
        .alert svg {
            width: 20px;
            height: 20px;
            flex-shrink: 0;
        }
        
        .alert-info {
            background: rgba(59, 130, 246, 0.1);
            border: 1px solid var(--accent-blue);
            color: var(--accent-blue);
        }
        
        .alert-warning {
            background: rgba(245, 158, 11, 0.1);
            border: 1px solid var(--accent-yellow);
            color: var(--accent-yellow);
        }
        
        .alert-danger {
            background: rgba(239, 68, 68, 0.1);
            border: 1px solid var(--accent-red);
            color: var(--accent-red);
        }
        
        /* === DIAGNOSE STATISTIKEN === */
        .stats-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 16px;
            margin-bottom: 24px;
        }
        
        .stat-box {
            background: var(--bg-secondary);
            border: 1px solid var(--border-color);
            padding: 20px;
            border-radius: 12px;
            text-align: center;
            border-left: 3px solid var(--accent-blue);
        }
        
        .stat-value {
            font-size: 2rem;
            font-weight: 700;
            color: var(--accent-blue);
            margin: 8px 0;
        }
        
        .stat-label {
            font-size: 13px;
            color: var(--text-secondary);
            font-weight: 500;
        }
        
        .chart-container {
            position: relative;
            height: 300px;
            width: 100%;
        }
        
        .status-badge {
            display: inline-block;
            padding: 4px 10px;
            border-radius: 6px;
            font-size: 12px;
            font-weight: 600;
        }
        
        .badge-online {
            background: rgba(16, 185, 129, 0.2);
            color: var(--accent-green);
        }
        
        .badge-offline {
            background: rgba(239, 68, 68, 0.2);
            color: var(--accent-red);
        }
        
        .badge-good {
            background: rgba(16, 185, 129, 0.2);
            color: var(--accent-green);
        }
        
        .badge-warning {
            background: rgba(245, 158, 11, 0.2);
            color: var(--accent-yellow);
        }
        
        .badge-critical {
            background: rgba(239, 68, 68, 0.2);
            color: var(--accent-red);
        }
        
        table {
            width: 100%;
            border-collapse: collapse;
        }
        
        th, td {
            text-align: left;
            padding: 12px;
            border-bottom: 1px solid var(--border-color);
            font-size: 14px;
        }
        
        th {
            color: var(--text-secondary);
            font-weight: 600;
            font-size: 13px;
            text-transform: uppercase;
        }
        
        tbody tr:hover {
            background: rgba(255, 255, 255, 0.03);
        }
        
        /* === USER LOGS === */
        .log-container {
            background: var(--bg-primary);
            border-radius: 8px;
            padding: 16px;
            max-height: 600px;
            overflow-y: auto;
        }
        
        .log-entry {
            padding: 16px;
            background: var(--bg-secondary);
            border-radius: 8px;
            border-left: 3px solid var(--accent-blue);
            margin-bottom: 12px;
        }
        
        .log-entry-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 8px;
        }
        
        .log-action {
            font-weight: 600;
            font-size: 14px;
        }
        
        .log-time {
            font-size: 12px;
            color: var(--text-tertiary);
        }
        
        .log-details {
            font-size: 13px;
            color: var(--text-secondary);
        }
        
        /* === RESPONSIVE === */
        @media (max-width: 768px) {
            .header-left {
                gap: 16px;
            }
            
            .nav-tabs {
                display: none;
            }
            
            .grid, .grid-2 {
                grid-template-columns: 1fr;
            }
        }
    </style>
</head>
<body>

    <header class="header">
        <div class="header-left">
            <div class="logo">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/>
                </svg>
                <span id="site-title"><?php echo htmlspecialchars($settings['site_title']); ?></span>
            </div>
            <nav class="nav-tabs">
                <button id="btn-dash" class="tab-btn active" onclick="switchView('dashboard')">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="3" width="7" height="7"/><rect x="14" y="3" width="7" height="7"/><rect x="14" y="14" width="7" height="7"/><rect x="3" y="14" width="7" height="7"/></svg>
                    Dashboard
                </button>
                <button id="btn-logs" class="tab-btn" onclick="switchView('logs')">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>
                    Logs
                </button>
                <button id="btn-diag" class="tab-btn" onclick="switchView('diagnose')">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/></svg>
                    Diagnose
                </button>
                <button id="btn-conf" class="tab-btn" onclick="switchView('config')">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M12 1v6m0 6v6m-6-6h6m6 0h-6M4.2 4.2l4.2 4.2m7.2 0l4.2-4.2M4.2 19.8l4.2-4.2m7.2 0l4.2 4.2"/></svg>
                    Settings
                </button>
                <?php if(strtolower($settings['site_title']) === 'admin'): ?>
                <button id="btn-userlogs" class="tab-btn" onclick="switchView('userlogs')">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/></svg>
                    Audit
                </button>
                <?php endif; ?>
            </nav>
        </div>
        <a href="?logout" class="btn-logout">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/><polyline points="16 17 21 12 16 7"/><line x1="21" y1="12" x2="9" y2="12"/></svg>
            Logout
        </a>
    </header>

    <main class="main-content">
        <div class="container">
            
            <!-- DASHBOARD VIEW -->
            <div id="view-dashboard">
                <div class="page-header">
                    <h1 class="page-title">Network Status</h1>
                    <p class="page-subtitle">Real-time monitoring of connected devices</p>
                </div>
                <div class="grid">
                    <div id="card-sender" class="card sender">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="22" y1="2" x2="11" y2="13"/><polygon points="22 2 15 22 11 13 2 9 22 2"/></svg>
                                ESP Sender
                            </div>
                            <div id="dot-sender" class="status-indicator"></div>
                        </div>
                        <div class="status-message" id="msg-sender">Waiting for connection...</div>
                        <div class="card-content">
                            <div class="info-row">
                                <span class="info-label">IP Address</span>
                                <span class="info-value" id="ip-sender">---</span>
                            </div>
                        </div>
                        <div class="btn-group">
                            <button class="btn btn-danger" onclick="sendCommand('sender', 'REBOOT')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg>
                                Reboot
                            </button>
                            <button class="btn btn-danger" onclick="sendCommand('sender', 'RESET')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="1 4 1 10 7 10"/><path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10"/></svg>
                                Reset
                            </button>
                        </div>
                    </div>

                    <div id="card-receiver" class="card receiver">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="17 11 12 6 7 11"/><polyline points="17 18 12 13 7 18"/></svg>
                                ESP Receiver
                            </div>
                            <div id="dot-receiver" class="status-indicator"></div>
                        </div>
                        <div class="status-message" id="msg-receiver">Waiting for connection...</div>
                        <div class="card-content">
                            <div class="info-row">
                                <span class="info-label">IP Address</span>
                                <span class="info-value" id="ip-receiver">---</span>
                            </div>
                        </div>
                        <div class="btn-group">
                            <div class="toggle-wrapper">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9"/><path d="M13.73 21a2 2 0 0 1-3.46 0"/></svg>
                                <label class="switch">
                                    <input type="checkbox" id="alarm-toggle" onchange="toggleAlarm(this)">
                                    <span class="slider"></span>
                                </label>
                            </div>
                            <button class="btn btn-danger" onclick="sendCommand('receiver', 'REBOOT')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg>
                                Reboot
                            </button>
                            <button class="btn btn-danger" onclick="sendCommand('receiver', 'RESET')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="1 4 1 10 7 10"/><path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10"/></svg>
                                Reset
                            </button>
                        </div>
                    </div>

                    <div id="card-camera" class="card camera">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2z"/><circle cx="12" cy="13" r="4"/></svg>
                                PiCam
                            </div>
                            <div id="dot-camera" class="status-indicator online"></div>
                        </div>
                        <div class="status-message" id="msg-camera">Running</div>
                        <div class="card-content">
                            <div class="info-row">
                                <span class="info-label">IP Address</span>
                                <span class="info-value" id="ip-camera">---</span>
                            </div>
                        </div>
                        <div class="btn-group">
                            <button class="btn btn-primary" onclick="openCameraStream()">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polygon points="5 3 19 12 5 21 5 3"/></svg>
                                Stream
                            </button>
                            <button class="btn btn-danger" onclick="rebootPi()">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg>
                                Reboot
                            </button>
                        </div>
                    </div>
                </div>
            </div>

            <!-- LOGS VIEW -->
            <div id="view-logs" class="view-section">
                <div class="page-header">
                    <h1 class="page-title">Live Communication Logs</h1>
                    <p class="page-subtitle">Real-time device communication streams</p>
                </div>
                <div class="grid">
                    <div class="card">
                        <div class="terminal-header">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/></svg>
                            ESP Sender Terminal
                        </div>
                        <div id="log-sender" class="terminal"></div>
                        <div class="terminal-actions">
                            <button class="btn btn-danger" onclick="clearLog('log-sender')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                                Clear
                            </button>
                        </div>
                    </div>
                    
                    <div class="card">
                        <div class="terminal-header">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/></svg>
                            ESP Receiver Terminal
                        </div>
                        <div id="log-receiver" class="terminal"></div>
                        <div class="terminal-actions">
                            <button class="btn btn-danger" onclick="clearLog('log-receiver')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                                Clear
                            </button>
                        </div>
                    </div>
                    
                    <div class="card">
                        <div class="terminal-header">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/></svg>
                            PiCam Terminal
                        </div>
                        <div id="log-camera" class="terminal"></div>
                        <div class="terminal-actions">
                            <button class="btn btn-danger" onclick="clearLog('log-camera')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                                Clear
                            </button>
                        </div>
                    </div>
                </div>
            </div>

            <!-- DIAGNOSE VIEW -->
            <div id="view-diagnose" class="view-section">
                <div class="page-header">
                    <h1 class="page-title">System-Diagnose & Telemetrie</h1>
                    <p class="page-subtitle">Detaillierte Systemüberwachung und Performance-Analyse</p>
                </div>
                
                <!-- Auto-Refresh Toggle -->
                <div class="alert alert-info" style="display: flex; align-items: center; gap: 12px;">
                    <label class="switch">
                        <input type="checkbox" id="diag-auto-refresh" checked>
                        <span class="slider"></span>
                    </label>
                    <span>Auto-Refresh (alle 5 Sek.)</span>
                </div>
                
                <!-- Statistiken -->
                <div class="stats-grid">
                    <?php 
                    $onlineCount = 0;
                    $totalUptime = 0;
                    foreach($diagStatus as $data) {
                        if(isset($data['last_seen']) && time() - $data['last_seen'] < 30) $onlineCount++;
                        if(isset($data['uptime'])) $totalUptime += $data['uptime'];
                    }
                    ?>
                    <div class="stat-box">
                        <div class="stat-label">Online Geräte</div>
                        <div class="stat-value"><?php echo $onlineCount; ?> / <?php echo count($diagStatus); ?></div>
                    </div>
                    <div class="stat-box">
                        <div class="stat-label">Telemetrie-Einträge</div>
                        <div class="stat-value"><?php echo file_exists($csvFile) ? count(file($csvFile)) : 0; ?></div>
                    </div>
                    <div class="stat-box">
                        <div class="stat-label">System Logs</div>
                        <div class="stat-value"><?php echo count($systemLogs); ?></div>
                    </div>
                    <div class="stat-box" style="border-left-color: var(--accent-green);">
                        <div class="stat-label">Gesamt Uptime</div>
                        <div class="stat-value" style="color: var(--accent-green); font-size: 1.5rem;">
                            <?php echo gmdate("H:i:s", $totalUptime); ?>
                        </div>
                    </div>
                </div>
                
                <!-- Geräte-Status Tabelle -->
                <div class="card" style="margin-bottom: 24px;">
                    <h3 style="margin-bottom: 16px; display: flex; align-items: center; gap: 8px;">
                        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/></svg>
                        Geräte-Status & Telemetrie
                    </h3>
                    <div style="overflow-x: auto;">
                        <table>
                            <thead>
                                <tr>
                                    <th>Gerät</th>
                                    <th>Status</th>
                                    <th>IP-Adresse</th>
                                    <th>Zuletzt gesehen</th>
                                    <th>Uptime</th>
                                    <th>Reset Grund</th>
                                    <th>Freier RAM</th>
                                    <th>Signal (RSSI)</th>
                                </tr>
                            </thead>
                            <tbody>
                                <?php if($diagStatus): foreach ($diagStatus as $name => $data): 
                                    $isOnline = isset($data['last_seen']) && time() - $data['last_seen'] < 30;
                                    $rssi = $data['rssi'] ?? 0;
                                    $rssiClass = $rssi > -60 ? 'badge-good' : ($rssi > -75 ? 'badge-warning' : 'badge-critical');
                                ?>
                                <tr>
                                    <td style="font-weight:600;">
                                        <?php echo htmlspecialchars(ucfirst($name)); ?>
                                    </td>
                                    <td>
                                        <span class="status-badge <?php echo $isOnline ? 'badge-online' : 'badge-offline'; ?>">
                                            <?php echo $isOnline ? 'ONLINE' : 'OFFLINE'; ?>
                                        </span>
                                    </td>
                                    <td><?php echo htmlspecialchars($data['ip'] ?? 'N/A'); ?></td>
                                    <td style="font-size: 13px;">
                                        <?php 
                                        if(isset($data['last_seen'])) {
                                            $diff = time() - $data['last_seen'];
                                            echo date("d.m.Y H:i:s", $data['last_seen']);
                                            echo " <span style='color: var(--text-tertiary);'>(" . $diff . "s)</span>";
                                        } else {
                                            echo '-';
                                        }
                                        ?>
                                    </td>
                                    <td><?php echo isset($data['uptime']) ? gmdate("H:i:s", $data['uptime']) : '-'; ?></td>
                                    <td>
                                        <span style="color: <?php echo in_array($data['reset_reason'] ?? '', ['External System', 'Power On']) ? 'var(--accent-green)' : 'var(--accent-red)'; ?>">
                                            <?php echo htmlspecialchars($data['reset_reason'] ?? 'unknown'); ?>
                                        </span>
                                    </td>
                                    <td><?php echo number_format($data['heap'] ?? 0); ?> B</td>
                                    <td>
                                        <span class="status-badge <?php echo $rssiClass; ?>">
                                            <?php echo $rssi; ?> dBm
                                        </span>
                                    </td>
                                </tr>
                                <?php endforeach; else: ?>
                                <tr><td colspan="8" style="text-align: center; color: var(--text-secondary);">Keine Geräte verbunden</td></tr>
                                <?php endif; ?>
                            </tbody>
                        </table>
                    </div>
                </div>
                
                <!-- Charts -->
                <div class="grid-2" style="margin-top: 24px;">
                    <div class="card">
                        <h3 style="margin-bottom: 16px; display: flex; align-items: center; gap: 8px;">
                            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="2"/><path d="M16.24 7.76a6 6 0 0 1 0 8.49m-8.48-.01a6 6 0 0 1 0-8.49m11.31-2.82a10 10 0 0 1 0 14.14m-14.14 0a10 10 0 0 1 0-14.14"/></svg>
                            WLAN-Signal (RSSI)
                        </h3>
                        <div class="chart-container"><canvas id="rssiChart"></canvas></div>
                    </div>
                    <div class="card">
                        <h3 style="margin-bottom: 16px; display: flex; align-items: center; gap: 8px;">
                            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="4" y="4" width="16" height="16" rx="2" ry="2"/><rect x="9" y="9" width="6" height="6"/><line x1="9" y1="1" x2="9" y2="4"/><line x1="15" y1="1" x2="15" y2="4"/><line x1="9" y1="20" x2="9" y2="23"/><line x1="15" y1="20" x2="15" y2="23"/><line x1="20" y1="9" x2="23" y2="9"/><line x1="20" y1="14" x2="23" y2="14"/><line x1="1" y1="9" x2="4" y2="9"/><line x1="1" y1="14" x2="4" y2="14"/></svg>
                            Verfügbarer RAM (Heap)
                        </h3>
                        <div class="chart-container"><canvas id="heapChart"></canvas></div>
                    </div>
                </div>
                
                <!-- System Logs -->
                <div class="card" style="margin-top: 24px;">
                    <h3 style="margin-bottom: 16px; display: flex; align-items: center; gap: 8px;">
                        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>
                        System-Logs (Letzte 50 Einträge)
                    </h3>
                    <div class="terminal" style="height: 400px;">
                        <?php if(!empty($systemLogs)): ?>
                            <?php foreach($systemLogs as $log): ?>
                                <div class="log-line"><?php echo htmlspecialchars($log); ?></div>
                            <?php endforeach; ?>
                        <?php else: ?>
                            <div style="color: var(--text-secondary); text-align: center; padding: 40px;">
                                Keine Logs verfügbar
                            </div>
                        <?php endif; ?>
                    </div>
                </div>
                
                <!-- Wartung -->
                <div class="card" style="margin-top: 24px;">
                    <h3 style="margin-bottom: 16px; display: flex; align-items: center; gap: 8px;">
                        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M12 1v6m0 6v6"/></svg>
                        Wartung & Export
                    </h3>
                    <div class="btn-group">
                        <a href="admin.php?action=export_telemetry" class="btn btn-primary">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
                            Telemetrie exportieren (CSV)
                        </a>
                        <a href="admin.php?action=clear_telemetry" class="btn btn-danger" onclick="return confirm('Telemetrie-Daten wirklich löschen?')">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                            Telemetrie löschen
                        </a>
                        <a href="admin.php?action=clear_all_logs" class="btn btn-danger" onclick="return confirm('System-Logs wirklich löschen?')">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                            System-Logs löschen
                        </a>
                    </div>
                </div>
            </div>

            <!-- CONFIG VIEW -->
            <div id="view-config" class="view-section">
                <div class="page-header">
                    <h1 class="page-title">System Configuration</h1>
                    <p class="page-subtitle">Manage dashboard and device settings</p>
                </div>
                <div class="grid">
                    <div class="card">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="3" width="18" height="18" rx="2" ry="2"/><line x1="9" y1="3" x2="9" y2="21"/></svg>
                                Dashboard Settings
                            </div>
                        </div>
                        
                        <div class="alert alert-info" id="timeout-status">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/></svg>
                            <div>
                                <div id="timeout-status-text" style="font-weight: 600;">Auto-Logout: Disabled</div>
                                <div id="timeout-info" style="font-size: 12px; opacity: 0.8; margin-top: 2px;">No automatic logout</div>
                            </div>
                        </div>
                        
                        <div class="form-group">
                            <label class="form-label">Page Title</label>
                            <input type="text" id="cfg-title" class="form-input" value="<?php echo $settings['site_title']; ?>">
                        </div>
                        
                        <div class="form-group">
                            <label class="form-label">Change Admin Password</label>
                            <input type="password" id="cfg-pw" class="form-input" placeholder="Enter new password">
                        </div>
                        
                        <div class="form-group">
                            <label class="form-label">Refresh Rate (ms)</label>
                            <input type="number" id="cfg-refresh" class="form-input" value="<?php echo $settings['refresh_rate']; ?>">
                        </div>
                        
                        <div class="form-group">
                            <label class="form-label">Auto-Logout Timeout (Minutes)</label>
                            <input type="number" id="cfg-timeout-min" class="form-input" value="<?php echo $settings['timeout_minutes'] ?? 5; ?>" min="1" max="60">
                            <div class="form-hint">Inactivity period before automatic logout</div>
                        </div>
                        
                        <div class="form-group">
                            <div class="toggle-wrapper">
                                <span>Enable Auto-Logout</span>
                                <label class="switch">
                                    <input type="checkbox" id="cfg-timeout-active" <?php echo ($settings['timeout_active'] ?? false) ? 'checked' : ''; ?>>
                                    <span class="slider"></span>
                                </label>
                            </div>
                        </div>
                        
                        <button class="btn btn-primary" onclick="saveConfig()" style="width: 100%;">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/><polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/></svg>
                            Save Settings
                        </button>
                        
                        <div class="alert alert-danger" style="margin-top: 24px;">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>
                            <span style="font-weight: 600;">Danger Zone</span>
                        </div>
                        <button class="btn btn-danger" onclick="resetSystem()" style="width: 100%;">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/><line x1="10" y1="11" x2="10" y2="17"/><line x1="14" y1="11" x2="14" y2="17"/></svg>
                            System Reset
                        </button>
                    </div>
                    
                    <div class="card">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="2"/><path d="M16.24 7.76a6 6 0 0 1 0 8.49m-8.48-.01a6 6 0 0 1 0-8.49m11.31-2.82a10 10 0 0 1 0 14.14m-14.14 0a10 10 0 0 1 0-14.14"/></svg>
                                Node Remote Config
                            </div>
                        </div>
                        
                        <div class="alert alert-info">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/><line x1="12" y1="8" x2="12.01" y2="8"/></svg>
                            <span>Only online devices can be configured</span>
                        </div>
                        
                        <div class="form-group">
                            <label class="form-label">Target Device</label>
                            <select id="conf-target" class="form-select">
                                <option value="" disabled selected>Loading status...</option>
                            </select>
                        </div>
                        
                        <div class="form-group">
                            <label class="form-label">API Server IP</label>
                            <input type="text" id="conf-apiip" class="form-input" placeholder="e.g. 192.168.1.50">
                        </div>
                        
                        <div class="form-group">
                            <label class="form-label">Primary WLAN SSID</label>
                            <input type="text" id="conf-mssid" class="form-input" placeholder="Network name">
                        </div>
                        
                        <div class="form-group">
                            <label class="form-label">Primary WLAN Password</label>
                            <input type="password" id="conf-mpass" class="form-input" placeholder="Network password">
                        </div>
                        
                        <div class="form-group">
                            <label class="form-label">Backup WLAN SSID</label>
                            <input type="text" id="conf-bssid" class="form-input" placeholder="Backup network name">
                        </div>
                        
                        <div class="form-group">
                            <label class="form-label">Backup WLAN Password</label>
                            <input type="password" id="conf-bpass" class="form-input" placeholder="Backup password">
                        </div>
                        
                        <div class="form-group">
                            <label class="form-label">Telnet Password</label>
                            <input type="password" id="conf-telnet" class="form-input" placeholder="Telnet access password">
                        </div>
                        
                        <button class="btn btn-primary" onclick="sendNodeConfig()" style="width: 100%;">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="12" y1="5" x2="12" y2="19"/><polyline points="19 12 12 19 5 12"/></svg>
                            Send to ESP
                        </button>
                    </div>
                </div>
            </div>

            <!-- USER LOGS VIEW -->
            <?php if(strtolower($settings['site_title']) === 'admin'): ?>
            <div id="view-userlogs" class="view-section">
                <div class="page-header">
                    <h1 class="page-title">Security Audit Log</h1>
                    <p class="page-subtitle">Administrator activity tracking and monitoring</p>
                </div>
                
                <div class="card" style="margin-bottom: 24px;">
                    <div class="alert alert-danger">
                        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>
                        <div>
                            <div style="font-weight: 600;">Security Protocol Active</div>
                            <div style="font-size: 12px; opacity: 0.8; margin-top: 2px;">All administrative actions are logged and monitored</div>
                        </div>
                    </div>
                    <div class="btn-group">
                        <button class="btn btn-primary" onclick="exportUserLogs()">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
                            Export Logs
                        </button>
                        <button class="btn btn-danger" onclick="clearUserLogs()">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                            Clear Logs
                        </button>
                    </div>
                </div>
                
                <div class="card">
                    <div id="userlog-container" class="log-container">
                        <div style="text-align: center; padding: 40px; color: var(--text-secondary);">
                            <svg style="width: 48px; height: 48px; margin-bottom: 16px; opacity: 0.5;" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="12" y1="2" x2="12" y2="6"/><line x1="12" y1="18" x2="12" y2="22"/><line x1="4.93" y1="4.93" x2="7.76" y2="7.76"/><line x1="16.24" y1="16.24" x2="19.07" y2="19.07"/><line x1="2" y1="12" x2="6" y2="12"/><line x1="18" y1="12" x2="22" y2="12"/><line x1="4.93" y1="19.07" x2="7.76" y2="16.24"/><line x1="16.24" y1="7.76" x2="19.07" y2="4.93"/></svg>
                            <div>Loading audit logs...</div>
                        </div>
                    </div>
                </div>
            </div>
            <?php endif; ?>

        </div>
    </main>


    <!-- SCRIPT 1: Kritische Funktionen (Buttons, Dashboard, Navigation) -->
    <script>
        // ============================================================
        // SECURITY: CSRF + Secure API Helper
        // ============================================================
        var csrfMeta = document.querySelector('meta[name="csrf-token"]');
        var CSRF_TOKEN = csrfMeta ? csrfMeta.content : '';
        var CAMERA_PORT = <?php echo (int)($settings['camera_port'] ?? 8082); ?>;

        function apiCall(action, params) {
            if (!params) params = {};
            params.csrf_token = CSRF_TOKEN;
            console.log('[API]', action, params);
            return fetch('api.php?action=' + action, {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/x-www-form-urlencoded',
                    'X-CSRF-TOKEN': CSRF_TOKEN
                },
                body: new URLSearchParams(params),
                credentials: 'same-origin'
            }).then(function(response) {
                console.log('[API Response]', action, response.status);
                if (response.status === 403) {
                    window.location.href = 'index.php?timeout=1';
                    return new Promise(function() {});
                }
                return response;
            })['catch'](function(err) {
                console.error('[API Error]', action, err);
                throw err;
            });
        }

        // ============================================================
        var refreshRate = <?php echo (int)$settings['refresh_rate']; ?>;
        var updateTimer = null;
        var timeoutActive = <?php echo json_encode($settings['timeout_active'] ?? false); ?>;
        var timeoutMinutes = <?php echo (int)($settings['timeout_minutes'] ?? 5); ?>;

        var lastPing = 0;
        function resetActivityTimer() {
            if (!timeoutActive) return;
            var now = Date.now();
            if (now - lastPing < 30000) return;
            lastPing = now;
            fetch('api.php?action=ping_activity', {method: 'POST', credentials: 'same-origin'});
        }

        ['mousedown', 'keydown', 'scroll', 'touchstart'].forEach(function(event) {
            document.addEventListener(event, resetActivityTimer, {passive: true});
        });

        function sendCommand(target, cmd) {
            if(!confirm('Execute ' + cmd + ' on ' + target + '?')) return;
            console.log('[CMD] Sending', cmd, 'to', target);
            apiCall('send_command', {target: target, cmd: cmd})
                .then(function(r) { return r.json(); })
                .then(function(data) {
                    console.log('[CMD] Response data:', data);
                    alert(data.message || data.error || 'Unknown response');
                })
                ['catch'](function(err) {
                    console.error('[CMD] Error:', err);
                    alert('Request failed: ' + err);
                });
        }

        function toggleAlarm(checkbox) {
            var cmd = checkbox.checked ? "ALARM_ON" : "ALARM_OFF";
            console.log('[ALARM] Sending', cmd);
            apiCall('send_command', {target: 'receiver', cmd: cmd})
                .then(function(r) { return r.json(); })
                .then(function(data) { console.log('[ALARM] Response:', data); })
                ['catch'](function(err) { console.error('[ALARM] Error:', err); });
        }

        function clearLog(id) {
            var target = '';
            if (id === 'log-sender') target = 'sender';
            if (id === 'log-receiver') target = 'receiver';
            if (id === 'log-camera') target = 'camera';
            if (!target) return;
            apiCall('clear_logs', {target: target})
                .then(function() {
                    document.getElementById(id).innerHTML = '<div style="opacity:0.5;text-align:center;padding:20px;">Log cleared</div>';
                });
        }

        function openCameraStream() {
            var el = document.getElementById('ip-camera');
            var camIp = el ? el.innerText : window.location.hostname;
            var url = 'http://' + (camIp !== '---' ? camIp : window.location.hostname) + ':' + CAMERA_PORT + '/?action=stream';
            window.open(url, '_blank');
        }

        function rebootPi() {
            if (!confirm('Raspberry Pi wirklich neustarten?')) return;
            if (!confirm('ACHTUNG: Dashboard wird kurzzeitig nicht erreichbar!')) return;
            apiCall('pi_reboot')
                .then(function(r) { return r.json(); })
                .then(function(data) { alert(data.message || data.error); })
                ['catch'](function() { alert('Request failed'); });
        }

        function switchView(tabName) {
            var views = ['dashboard', 'logs', 'diagnose', 'config', 'userlogs'];
            var btnMap = { 'dashboard': 'dash', 'config': 'conf', 'logs': 'logs', 'userlogs': 'userlogs', 'diagnose': 'diag' };
            
            views.forEach(function(v) {
                var viewEl = document.getElementById('view-' + v);
                if (viewEl) viewEl.style.display = 'none';
                var btn = document.getElementById('btn-' + btnMap[v]);
                if (btn) btn.classList.remove('active');
            });
            
            var targetView = document.getElementById('view-' + tabName);
            if (targetView) targetView.style.display = 'block';
            var activeBtn = document.getElementById('btn-' + btnMap[tabName]);
            if (activeBtn) activeBtn.classList.add('active');
            
            try { sessionStorage.setItem('activeTab', tabName); } catch(e) {}
            
            if (tabName === 'userlogs') loadUserLogsSimple();
            if (tabName === 'diagnose' && typeof initDiagnoseCharts === 'function') initDiagnoseCharts();
            
            resetActivityTimer();
        }

        function saveConfig() {
            var settings = {
                site_title: document.getElementById('cfg-title').value,
                password: document.getElementById('cfg-pw').value,
                refresh_rate: document.getElementById('cfg-refresh').value,
                timeout_active: document.getElementById('cfg-timeout-active').checked,
                timeout_minutes: parseInt(document.getElementById('cfg-timeout-min').value)
            };
            apiCall('save_settings', {settings: JSON.stringify(settings)})
                .then(function(r) { return r.json(); })
                .then(function(data) { alert(data.message || data.error); location.reload(); })
                ['catch'](function() { alert('Request failed'); });
        }

        function sendNodeConfig() {
            var target = document.getElementById('conf-target').value;
            if (!target) { alert("No device selected!"); return; }
            var config = {};
            var api = document.getElementById('conf-apiip').value; if(api) config.apiip = api;
            var mssid = document.getElementById('conf-mssid').value; if(mssid) config.mssid = mssid;
            var mpass = document.getElementById('conf-mpass').value; if(mpass) config.mpass = mpass;
            var bssid = document.getElementById('conf-bssid').value; if(bssid) config.bssid = bssid;
            var bpass = document.getElementById('conf-bpass').value; if(bpass) config.bpass = bpass;
            var telnet = document.getElementById('conf-telnet').value; if(telnet) config.tpass = telnet;
            if (Object.keys(config).length === 0) { alert("No fields filled!"); return; }
            if (!confirm('Send configuration to ' + target + '? Device will restart.')) return;
            apiCall('save_node_config', {target: target, config: JSON.stringify(config)})
                .then(function(r) { return r.json(); })
                .then(function(data) { alert(data.message || data.error); })
                ['catch'](function() { alert('Request failed'); });
        }

        function resetSystem() {
            if (!confirm("Delete all data?")) return;
            if (!confirm("CONFIRM: Erase all status, logs and commands?")) return;
            apiCall('system_reset')
                .then(function(r) { return r.json(); })
                .then(function(data) { alert(data.message || data.error); location.reload(); })
                ['catch'](function() { alert('Request failed'); });
        }

        function updateTimeoutStatus() {
            var indicator = document.getElementById('timeout-status');
            var statusText = document.getElementById('timeout-status-text');
            var infoText = document.getElementById('timeout-info');
            if (!indicator || !statusText || !infoText) return;
            
            if (timeoutActive) {
                indicator.className = 'alert alert-warning';
                statusText.textContent = 'Auto-Logout: Enabled';
                infoText.textContent = 'Automatic logout after ' + timeoutMinutes + ' minutes of inactivity';
            } else {
                indicator.className = 'alert alert-info';
                statusText.textContent = 'Auto-Logout: Disabled';
                infoText.textContent = 'No automatic logout';
            }
        }

        function updateDashboard() {
            fetch('api.php?get=all', {credentials: 'same-origin'})
                .then(function(response) {
                    if (response.status === 403) { window.location.href = 'index.php?timeout=1'; return null; }
                    return response.json();
                })
                .then(function(data) {
                    if (!data || !data.status) return;
                    updateNode('sender', data.status.sender || null);
                    updateNode('receiver', data.status.receiver || null);
                    
                    var camData = data.status.camera ? JSON.parse(JSON.stringify(data.status.camera)) : {};
                    var piData = data.status.pi || {};
                    if (!camData.ip || camData.ip === '0.0.0.0') camData.ip = piData.ip || '---';
                    if (!camData.online && piData.online) {
                        camData.online = true;
                        camData.status = 'Stream bereit';
                    }
                    updateNode('camera', camData);

                    if (data.config) {
                        timeoutActive = data.config.timeout_active || false;
                        timeoutMinutes = data.config.timeout_minutes || 5;
                        updateTimeoutStatus();
                    }

                    var targetSelect = document.getElementById('conf-target');
                    if (targetSelect) {
                        var currentSelection = targetSelect.value;
                        var sOnline = data.status.sender && data.status.sender.online;
                        var rOnline = data.status.receiver && data.status.receiver.online;
                        var newOpts = '<option value="sender">ESP Sender' + (sOnline ? '' : ' (OFFLINE)') + '</option>';
                        newOpts += '<option value="receiver">ESP Receiver' + (rOnline ? '' : ' (OFFLINE)') + '</option>';
                        if (targetSelect.innerHTML !== newOpts) {
                            targetSelect.innerHTML = newOpts;
                            if (currentSelection) targetSelect.value = currentSelection;
                        }
                    }

                    var alarmSwitch = document.getElementById('alarm-toggle');
                    if (alarmSwitch && data.status.receiver && data.status.receiver.alarm !== undefined && document.activeElement !== alarmSwitch) {
                        alarmSwitch.checked = (data.status.receiver.alarm == true || data.status.receiver.alarm == "1");
                    }

                    var logSender = document.getElementById('log-sender');
                    var logReceiver = document.getElementById('log-receiver');
                    var logCamera = document.getElementById('log-camera');
                    if (logSender) logSender.innerHTML = '';
                    if (logReceiver) logReceiver.innerHTML = '';
                    if (logCamera) logCamera.innerHTML = '';
                    
                    if (data.logs) {
                        data.logs.forEach(function(line) {
                            var tgt = 'log-sender';
                            if (line.indexOf('receiver:') !== -1) tgt = 'log-receiver';
                            if (line.indexOf('camera:') !== -1) tgt = 'log-camera';
                            var div = document.createElement('div');
                            div.className = 'log-line';
                            div.textContent = line;
                            var container = document.getElementById(tgt);
                            if (container) container.appendChild(div);
                        });
                        ['log-sender', 'log-receiver', 'log-camera'].forEach(function(id) {
                            var el = document.getElementById(id);
                            if (el) el.scrollTop = el.scrollHeight;
                        });
                    }

                    var ulView = document.getElementById('view-userlogs');
                    if (ulView && ulView.style.display !== 'none') loadUserLogsSimple();
                })
                ['catch'](function(err) { console.error('Dashboard Error:', err); });
        }

        function updateNode(name, data) {
            var card = document.getElementById('card-' + name);
            var ipField = document.getElementById('ip-' + name);
            var msgField = document.getElementById('msg-' + name);
            var dot = document.getElementById('dot-' + name);
            if (!card) return;
            if (!data) {
                card.classList.remove('online');
                if (dot) dot.classList.remove('online');
                return;
            }
            if (ipField) ipField.innerText = data.ip || '---';
            if (data.online) {
                if (msgField) msgField.innerText = data.status || 'Online';
                card.classList.add('online');
                if (dot) dot.classList.add('online');
            } else {
                card.classList.remove('online');
                if (msgField) msgField.innerText = 'Device offline';
                if (dot) dot.classList.remove('online');
            }
        }

        function startLoop() {
            updateDashboard();
            updateTimer = setTimeout(startLoop, refreshRate);
        }

        function loadUserLogsSimple() {
            var container = document.getElementById('userlog-container');
            if (!container) return;
            fetch('api.php?action=get_user_logs', {credentials: 'same-origin'})
                .then(function(r) { return r.text(); })
                .then(function(text) {
                    var logs;
                    try { logs = JSON.parse(text); } catch(e) {
                        container.innerHTML = '<div style="text-align:center;padding:40px;color:var(--accent-red);">Error parsing log data</div>';
                        return;
                    }
                    if (!logs || logs.length === 0) {
                        container.innerHTML = '<div style="text-align:center;padding:40px;color:var(--text-secondary);">No activity logged</div>';
                        return;
                    }
                    var html = '';
                    logs.slice(0, 20).forEach(function(log) {
                        html += '<div class="log-entry">' +
                            '<div class="log-entry-header">' +
                                '<div class="log-action" style="display:flex;align-items:center;gap:8px;">' +
                                    '<span>' + (log.action || '') + '</span>' +
                                '</div>' +
                                '<span class="log-time">' + (log.date || '') + '</span>' +
                            '</div>' +
                            '<div class="log-details">' +
                                'IP: ' + (log.ip || '') + ' | Device: ' + (log.device_name || '') +
                                (log.details ? '<br><span style="color:var(--accent-yellow);">&rarr; ' + log.details + '</span>' : '') +
                            '</div></div>';
                    });
                    container.innerHTML = html;
                })
                ['catch'](function() {
                    container.innerHTML = '<div style="text-align:center;padding:40px;color:var(--accent-red);">Error loading logs</div>';
                });
        }

        function exportUserLogs() { window.location.href = 'api.php?action=export_user_logs'; }

        function clearUserLogs() {
            if (!confirm('Delete all audit logs?')) return;
            apiCall('clear_user_logs')
                .then(function(r) { return r.json(); })
                .then(function(data) { alert(data.message || data.error); loadUserLogsSimple(); })
                ['catch'](function() { alert('Request failed'); });
        }

        // === INIT ===
        updateTimeoutStatus();
        try {
            var savedTab = sessionStorage.getItem('activeTab');
            if (savedTab && savedTab !== 'dashboard') switchView(savedTab);
        } catch(e) {}
        startLoop();
        try { if (typeof lucide !== 'undefined') lucide.createIcons(); } catch(e) {}

        console.log('[INIT] All critical functions loaded OK. CSRF token:', CSRF_TOKEN ? 'present (' + CSRF_TOKEN.length + ' chars)' : 'MISSING!');
    </script>

    <!-- SCRIPT 2: Charts (isoliert - Fehler hier brechen KEINE Buttons) -->
    <script>
        var rssiChart = null, heapChart = null;
        
        function initDiagnoseCharts() {
            try {
                if (typeof Chart === 'undefined') {
                    console.warn('[CHARTS] Chart.js not loaded');
                    return;
                }

                var chartData = <?php echo json_encode($chartData); ?>;
                
                var allTimes = {};
                ['sender', 'receiver', 'camera'].forEach(function(src) {
                    chartData[src].time.forEach(function(t) { allTimes[t] = true; });
                });
                var sortedTimes = Object.keys(allTimes).map(Number).sort(function(a,b) { return a-b; });
                var labels = sortedTimes.map(function(t) { return new Date(t * 1000).toLocaleTimeString('de-DE'); });
                
                function mapToTimeline(srcData, field) {
                    var lookup = {};
                    srcData.time.forEach(function(t, i) { lookup[t] = srcData[field][i]; });
                    return sortedTimes.map(function(t) { return (t in lookup) ? lookup[t] : null; });
                }

                if (rssiChart) { rssiChart.destroy(); rssiChart = null; }
                if (heapChart) { heapChart.destroy(); heapChart = null; }

                function makeDatasets(field) {
                    return [
                        { label: 'Sender',   data: mapToTimeline(chartData.sender, field),   borderColor: '#3b82f6', tension: 0.3, fill: false, spanGaps: true },
                        { label: 'Receiver', data: mapToTimeline(chartData.receiver, field), borderColor: '#f59e0b', tension: 0.3, fill: false, spanGaps: true },
                        { label: 'PiCam',    data: mapToTimeline(chartData.camera, field),   borderColor: '#8b5cf6', tension: 0.3, fill: false, spanGaps: true }
                    ];
                }

                var rssiOpts = {
                    responsive: true, maintainAspectRatio: false, animation: false,
                    plugins: { legend: { labels: { color: '#f1f5f9', usePointStyle: true, pointStyle: 'line' } } },
                    scales: {
                        x: { ticks: { color: '#94a3b8', maxTicksLimit: 12, maxRotation: 45 }, grid: { color: 'rgba(51,65,85,0.3)' } },
                        y: { ticks: { color: '#94a3b8' }, grid: { color: 'rgba(51,65,85,0.3)' }, title: { display: true, text: 'dBm', color: '#94a3b8' }, suggestedMin: -80, suggestedMax: -30 }
                    },
                    elements: { point: { radius: 0, hitRadius: 6 }, line: { borderWidth: 2 } }
                };

                rssiChart = new Chart(document.getElementById('rssiChart'), {
                    type: 'line', data: { labels: labels, datasets: makeDatasets('rssi') }, options: rssiOpts
                });

                var heapOpts = {
                    responsive: true, maintainAspectRatio: false, animation: false,
                    plugins: { legend: { labels: { color: '#f1f5f9', usePointStyle: true, pointStyle: 'line' } } },
                    scales: {
                        x: { ticks: { color: '#94a3b8', maxTicksLimit: 12, maxRotation: 45 }, grid: { color: 'rgba(51,65,85,0.3)' } },
                        y: { ticks: { color: '#94a3b8', callback: function(v) { return (v/1024).toFixed(1) + ' KB'; } }, grid: { color: 'rgba(51,65,85,0.3)' }, title: { display: true, text: 'Bytes', color: '#94a3b8' } }
                    },
                    elements: { point: { radius: 0, hitRadius: 6 }, line: { borderWidth: 2 } }
                };

                heapChart = new Chart(document.getElementById('heapChart'), {
                    type: 'line', data: { labels: labels, datasets: makeDatasets('heap') }, options: heapOpts
                });
                
                console.log('[CHARTS] OK:', sortedTimes.length, 'points');
            } catch(err) {
                console.error('[CHARTS] Error:', err);
            }
            
            // Auto-Refresh Toggle
            var toggle = document.getElementById('diag-auto-refresh');
            var diagInterval = null;
            try {
                var saved = sessionStorage.getItem('diagAutoRefresh');
                if (saved !== null) toggle.checked = (saved === 'true');
            } catch(e) {}
            function startDiagRefresh() {
                if (diagInterval) clearInterval(diagInterval);
                if (toggle.checked) diagInterval = setInterval(function() { location.reload(); }, 5000);
            }
            toggle.addEventListener('change', function() {
                try { sessionStorage.setItem('diagAutoRefresh', this.checked); } catch(e) {}
                startDiagRefresh();
            });
            startDiagRefresh();
        }
        console.log('[INIT] Chart module loaded');
    </script>
</body>
</html>
