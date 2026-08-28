<?php
declare(strict_types=1);

if (PHP_SAPI !== 'cli' || $argc !== 4) {
    fwrite(STDERR, "usage: status_rmw_worker.php <index> <gate> <ready>\n");
    exit(64);
}

define('IOT_ALARM_PROTOCOL_LIBRARY_ONLY', true);
require dirname(__DIR__, 2) . '/web/api.php';

if (file_put_contents($argv[3], 'ready') === false) exit(65);
$deadline = microtime(true) + 30.0;
while (!file_exists($argv[2])) {
    if (microtime(true) >= $deadline) exit(66);
    usleep(1000);
}

$index = (int)$argv[1];
$source = ($index % 2 === 0) ? 'sender' : 'receiver';
try {
    updateJsonLockedStrict($statusFile, function(array $current) use ($source, $index): array {
        if (!isset($current[$source]) || !is_array($current[$source])) $current[$source] = [];
        if (!isset($current[$source]['workers']) || !is_array($current[$source]['workers'])) {
            $current[$source]['workers'] = [];
        }
        $current[$source]['workers'][(string)$index] = $index;
        return $current;
    });
} catch (Throwable $e) {
    fwrite(STDERR, $e->getMessage() . "\n");
    exit(67);
}
exit(0);
