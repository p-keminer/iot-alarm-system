<?php

if (PHP_SAPI !== 'cli' || $argc !== 6) {
    fwrite(STDERR, "usage: brute_force_worker.php <ledger> <ip> <hash-b64> <gate> <ready>\n");
    exit(64);
}

require_once dirname(__DIR__, 2) . '/web/includes/brute_force.php';

$hash = base64_decode($argv[3], true);
if ($hash === false) {
    fwrite(STDERR, "invalid password hash encoding\n");
    exit(65);
}

$_SERVER['HTTP_USER_AGENT'] = 'security-protocol-parallel-probe';
$config = [
    // Die Probe soll alle 80 Verifikationen ausführen; das Produktions-Lockout
    // wird separat durch dieselbe Statusfunktion berechnet.
    'lockout_tiers' => [
        ['attempts' => 1000, 'lockout_seconds' => 60]
    ],
    'attempt_window' => 3600,
    'file' => $argv[1]
];

if (file_put_contents($argv[5], 'ready') === false) {
    fwrite(STDERR, "cannot signal ready state\n");
    exit(66);
}
$deadline = microtime(true) + 15.0;
while (!file_exists($argv[4])) {
    if (microtime(true) >= $deadline) {
        fwrite(STDERR, "start gate timeout\n");
        exit(67);
    }
    usleep(1000);
}

$result = authenticateWithBruteForce($argv[2], 'definitely-wrong', $hash, $config);
echo json_encode($result, JSON_UNESCAPED_SLASHES), PHP_EOL;

if (!empty($result['storage_error']) || empty($result['password_checked']) || !empty($result['authenticated'])) {
    exit(1);
}
exit(0);
