<?php
declare(strict_types=1);

if (PHP_SAPI !== 'cli') {
    fwrite(STDERR, "CLI only\n");
    exit(64);
}

$workerCount = 40;
$testDir = sys_get_temp_dir() . DIRECTORY_SEPARATOR . 'iot-alarm-status-' . bin2hex(random_bytes(8));
if (!mkdir($testDir, 0700, true) && !is_dir($testDir)) {
    throw new RuntimeException('cannot create isolated test directory');
}

$previousDataDir = getenv('IOT_ALARM_DATA_DIR');
putenv('IOT_ALARM_DATA_DIR=' . $testDir);
// Einmaliger Bootstrap vor dem Parallelfenster: Der Test misst ausschliesslich
// die Status-RMW-Sperre, nicht parallel erzeugte bcrypt-Erstzugangsdaten.
define('IOT_ALARM_PROTOCOL_LIBRARY_ONLY', true);
require dirname(__DIR__, 2) . '/web/api.php';
$worker = __DIR__ . DIRECTORY_SEPARATOR . 'status_rmw_worker.php';
$gate = $testDir . DIRECTORY_SEPARATOR . 'start.gate';
$readyDir = $testDir . DIRECTORY_SEPARATOR . 'ready';
mkdir($readyDir, 0700, true);
$processes = [];

try {
    // Vorhandene Felder duerfen durch parallele Heartbeat-Mutationen nicht
    // verschwinden.
    file_put_contents($testDir . DIRECTORY_SEPARATOR . 'status.json', json_encode([
        'sender' => ['sentinel' => 'sender'],
        'receiver' => ['sentinel' => 'receiver']
    ], JSON_THROW_ON_ERROR));

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

    $readyDeadline = microtime(true) + 30.0;
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

    $status = json_decode(file_get_contents($testDir . DIRECTORY_SEPARATOR . 'status.json'), true, 32, JSON_THROW_ON_ERROR);
    if (($status['sender']['sentinel'] ?? null) !== 'sender' ||
        ($status['receiver']['sentinel'] ?? null) !== 'receiver') {
        throw new RuntimeException('existing status fields were lost');
    }
    $senderWorkers = array_map('intval', array_keys($status['sender']['workers'] ?? []));
    $receiverWorkers = array_map('intval', array_keys($status['receiver']['workers'] ?? []));
    sort($senderWorkers, SORT_NUMERIC);
    sort($receiverWorkers, SORT_NUMERIC);
    if ($senderWorkers !== range(0, 38, 2) || $receiverWorkers !== range(1, 39, 2)) {
        throw new RuntimeException('parallel status updates were lost');
    }
    echo "OK: 40/40 status RMW updates serialized without lost updates\n";
} finally {
    foreach ($processes as [$process, $stdout, $stderr]) {
        if (is_resource($stdout)) fclose($stdout);
        if (is_resource($stderr)) fclose($stderr);
        if (is_resource($process)) proc_terminate($process);
    }
    if (is_dir($testDir)) {
        $iterator = new RecursiveIteratorIterator(
            new RecursiveDirectoryIterator($testDir, FilesystemIterator::SKIP_DOTS),
            RecursiveIteratorIterator::CHILD_FIRST
        );
        foreach ($iterator as $entry) {
            if ($entry->isDir()) rmdir($entry->getPathname());
            else unlink($entry->getPathname());
        }
        rmdir($testDir);
    }
    if ($previousDataDir === false) putenv('IOT_ALARM_DATA_DIR');
    else putenv('IOT_ALARM_DATA_DIR=' . $previousDataDir);
}
