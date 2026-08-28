<?php

if (PHP_SAPI !== 'cli' || $argc !== 4) {
    fwrite(STDERR, "usage: audit_log_worker.php <index> <gate> <ready>\n");
    exit(64);
}

require_once dirname(__DIR__, 2) . '/web/includes/brute_force.php';

if (file_put_contents($argv[3], 'ready') === false) exit(65);
$deadline = microtime(true) + 15.0;
while (!file_exists($argv[2])) {
    if (microtime(true) >= $deadline) exit(66);
    usleep(1000);
}

$entry = [
    'timestamp' => time(),
    'action' => 'PARALLEL-TEST',
    'worker' => (int)$argv[1]
];
exit(appendUserLogLocked($entry, 100) ? 0 : 1);
