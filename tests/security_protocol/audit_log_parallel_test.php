<?php

if (PHP_SAPI !== 'cli') {
    fwrite(STDERR, "CLI only\n");
    exit(64);
}

$workerCount = 40;
$testDir = sys_get_temp_dir() . DIRECTORY_SEPARATOR . 'iot-alarm-audit-' . bin2hex(random_bytes(8));
if (!mkdir($testDir, 0700, true) && !is_dir($testDir)) {
    throw new RuntimeException('cannot create isolated test directory');
}

$previousDataDir = getenv('IOT_ALARM_DATA_DIR');
putenv('IOT_ALARM_DATA_DIR=' . $testDir);
$worker = __DIR__ . DIRECTORY_SEPARATOR . 'audit_log_worker.php';
$gate = $testDir . DIRECTORY_SEPARATOR . 'start.gate';
$readyDir = $testDir . DIRECTORY_SEPARATOR . 'ready';
mkdir($readyDir, 0700, true);
$processes = [];

try {
    for ($i = 0; $i < $workerCount; $i++) {
        $pipes = [];
        $process = proc_open(
            [PHP_BINARY, $worker, (string)$i, $gate, $readyDir . DIRECTORY_SEPARATOR . $i . '.ready'],
            [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']],
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
    while (count(glob($readyDir . DIRECTORY_SEPARATOR . '*.ready') ?: []) !== $workerCount) {
        if (microtime(true) >= $readyDeadline) throw new RuntimeException('worker barrier timeout');
        usleep(10000);
    }
    if (file_put_contents($gate, 'go') === false) throw new RuntimeException('cannot open start gate');

    foreach ($processes as [$process, $stdout, $stderr, $index]) {
        stream_get_contents($stdout);
        $error = stream_get_contents($stderr);
        fclose($stdout);
        fclose($stderr);
        $exitCode = proc_close($process);
        if ($exitCode !== 0) throw new RuntimeException("worker $index failed: " . trim($error));
    }
    $processes = [];

    $logs = json_decode(file_get_contents($testDir . DIRECTORY_SEPARATOR . 'user_logs.json'), true);
    if (!is_array($logs) || count($logs) !== $workerCount) {
        throw new RuntimeException('parallel audit entries were lost');
    }
    $workers = array_map(fn($entry) => $entry['worker'] ?? -1, $logs);
    sort($workers, SORT_NUMERIC);
    if ($workers !== range(0, $workerCount - 1)) throw new RuntimeException('audit entries are not unique');

    echo "OK: 40/40 audit-log appends serialized without lost updates\n";
} finally {
    foreach ($processes as [$process, $stdout, $stderr]) {
        if (is_resource($stdout)) fclose($stdout);
        if (is_resource($stderr)) fclose($stderr);
        if (is_resource($process)) proc_terminate($process);
    }
    foreach (glob($readyDir . DIRECTORY_SEPARATOR . '*.ready') ?: [] as $file) unlink($file);
    foreach (['start.gate', 'user_logs.json'] as $name) {
        $path = $testDir . DIRECTORY_SEPARATOR . $name;
        if (file_exists($path)) unlink($path);
    }
    if (is_dir($readyDir)) rmdir($readyDir);
    if (is_dir($testDir)) rmdir($testDir);
    if ($previousDataDir === false) putenv('IOT_ALARM_DATA_DIR');
    else putenv('IOT_ALARM_DATA_DIR=' . $previousDataDir);
}
