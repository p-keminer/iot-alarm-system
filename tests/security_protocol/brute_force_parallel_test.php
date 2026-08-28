<?php

if (PHP_SAPI !== 'cli') {
    fwrite(STDERR, "CLI only\n");
    exit(64);
}

$workerCount = 80;
$testDir = sys_get_temp_dir() . DIRECTORY_SEPARATOR . 'iot-alarm-bf-' . bin2hex(random_bytes(8));
if (!mkdir($testDir, 0700, true) && !is_dir($testDir)) {
    throw new RuntimeException('cannot create isolated test directory');
}

$previousDataDir = getenv('IOT_ALARM_DATA_DIR');
putenv('IOT_ALARM_DATA_DIR=' . $testDir);
require_once dirname(__DIR__, 2) . '/web/includes/brute_force.php';

$ledger = $testDir . DIRECTORY_SEPARATOR . 'login_attempts.json';
$ip = '192.0.2.80';
$hash = password_hash('correct-password', PASSWORD_BCRYPT, ['cost' => 4]);
$worker = __DIR__ . DIRECTORY_SEPARATOR . 'brute_force_worker.php';
$gate = $testDir . DIRECTORY_SEPARATOR . 'start.gate';
$readyDir = $testDir . DIRECTORY_SEPARATOR . 'ready';
if (!mkdir($readyDir, 0700, true) && !is_dir($readyDir)) {
    throw new RuntimeException('cannot create worker barrier directory');
}
$processes = [];
$results = [];

try {
    // Pfadauflösung muss auch mit einem fremden CWD ausschließlich auf das
    // konfigurierte Datenverzeichnis zeigen.
    if (iotAlarmDataPath('user_logs.json') !== $testDir . DIRECTORY_SEPARATOR . 'user_logs.json') {
        throw new RuntimeException('IOT_ALARM_DATA_DIR was not honored');
    }
    bfWriteJsonLocked(iotAlarmDataPath('user_logs.json'), [['probe' => true]]);
    if (loadUserLogs() !== [['probe' => true]]) throw new RuntimeException('user log path depends on CWD');
    file_put_contents($testDir . DIRECTORY_SEPARATOR . 'status.json', '{"path_probe":true}');
    $originalCwd = getcwd();
    try {
        chdir(sys_get_temp_dir());
        require dirname(__DIR__, 2) . '/web/includes/data.php';
    } finally {
        if ($originalCwd !== false) chdir($originalCwd);
    }
    if (($diagStatus['path_probe'] ?? false) !== true) throw new RuntimeException('diagnostic path depends on CWD');

    for ($i = 0; $i < $workerCount; $i++) {
        $pipes = [];
        $process = proc_open(
            [
                PHP_BINARY,
                $worker,
                $ledger,
                $ip,
                base64_encode($hash),
                $gate,
                $readyDir . DIRECTORY_SEPARATOR . $i . '.ready'
            ],
            [
                0 => ['pipe', 'r'],
                1 => ['pipe', 'w'],
                2 => ['pipe', 'w']
            ],
            $pipes,
            null,
            null,
            ['bypass_shell' => true]
        );
        if (!is_resource($process)) throw new RuntimeException("worker $i did not start");
        fclose($pipes[0]);
        $processes[] = [$process, $pipes[1], $pipes[2], $i];
    }

    $readyDeadline = microtime(true) + 15.0;
    do {
        $readyFiles = glob($readyDir . DIRECTORY_SEPARATOR . '*.ready');
        if (is_array($readyFiles) && count($readyFiles) === $workerCount) break;
        if (microtime(true) >= $readyDeadline) {
            throw new RuntimeException('not all workers reached the start barrier');
        }
        usleep(10000);
    } while (true);
    if (file_put_contents($gate, 'go') === false) throw new RuntimeException('cannot open start gate');

    foreach ($processes as [$process, $stdout, $stderr, $index]) {
        $out = stream_get_contents($stdout);
        $err = stream_get_contents($stderr);
        fclose($stdout);
        fclose($stderr);
        $exitCode = proc_close($process);
        if ($exitCode !== 0) {
            throw new RuntimeException("worker $index failed ($exitCode): " . trim($err));
        }
        $decoded = json_decode(trim($out), true);
        if (!is_array($decoded)) throw new RuntimeException("worker $index returned invalid JSON");
        $results[] = $decoded;
    }
    $processes = [];

    $raw = file_get_contents($ledger);
    $attempts = $raw === false ? null : json_decode($raw, true);
    if (!is_array($attempts) || !isset($attempts[$ip]) || !is_array($attempts[$ip])) {
        throw new RuntimeException('parallel ledger is missing or invalid');
    }
    if (count($attempts[$ip]) !== $workerCount) {
        throw new RuntimeException('lost update: expected 80 attempts, got ' . count($attempts[$ip]));
    }

    $observedTotals = array_map(function($result) {
        return (int)($result['total_attempts'] ?? -1);
    }, $results);
    sort($observedTotals, SORT_NUMERIC);
    if ($observedTotals !== range(1, $workerCount)) {
        throw new RuntimeException('transactions were not serialized exactly once');
    }

    $unlockedConfig = [
        'lockout_tiers' => [['attempts' => 1000, 'lockout_seconds' => 60]],
        'attempt_window' => 3600,
        'file' => $ledger
    ];
    $success = authenticateWithBruteForce($ip, 'correct-password', $hash, $unlockedConfig);
    if (empty($success['authenticated'])) throw new RuntimeException('valid login was rejected');
    $afterSuccess = json_decode(file_get_contents($ledger), true);
    if (isset($afterSuccess[$ip])) throw new RuntimeException('successful login did not clear attempts');

    $productionConfig = [
        'lockout_tiers' => [
            ['attempts' => 5, 'lockout_seconds' => 300],
            ['attempts' => 10, 'lockout_seconds' => 900],
            ['attempts' => 15, 'lockout_seconds' => 3600]
        ],
        'attempt_window' => 3600,
        'file' => $ledger
    ];
    $lockIp = '192.0.2.81';
    for ($i = 0; $i < 5; $i++) {
        authenticateWithBruteForce($lockIp, 'definitely-wrong', $hash, $productionConfig);
    }
    $blocked = authenticateWithBruteForce($lockIp, 'correct-password', $hash, $productionConfig);
    if (empty($blocked['blocked']) || !empty($blocked['password_checked']) || !empty($blocked['authenticated'])) {
        throw new RuntimeException('lockout did not precede password verification');
    }

    echo "OK: 80/80 parallel attempts, atomic clear and pre-verification lockout\n";
} finally {
    foreach ($processes as [$process, $stdout, $stderr]) {
        if (is_resource($stdout)) fclose($stdout);
        if (is_resource($stderr)) fclose($stderr);
        if (is_resource($process)) proc_terminate($process);
    }
    foreach (['login_attempts.json', 'user_logs.json', 'status.json'] as $testFile) {
        $path = $testDir . DIRECTORY_SEPARATOR . $testFile;
        if (file_exists($path)) unlink($path);
    }
    if (file_exists($gate)) unlink($gate);
    if (is_dir($readyDir)) {
        foreach (glob($readyDir . DIRECTORY_SEPARATOR . '*.ready') ?: [] as $readyFile) unlink($readyFile);
        rmdir($readyDir);
    }
    if (is_dir($testDir)) rmdir($testDir);
    if ($previousDataDir === false) {
        putenv('IOT_ALARM_DATA_DIR');
    } else {
        putenv('IOT_ALARM_DATA_DIR=' . $previousDataDir);
    }
}
