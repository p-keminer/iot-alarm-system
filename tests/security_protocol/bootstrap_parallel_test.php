<?php
declare(strict_types=1);

function removeTree(string $root): void {
    if (!is_dir($root)) return;
    $items = new RecursiveIteratorIterator(
        new RecursiveDirectoryIterator($root, FilesystemIterator::SKIP_DOTS),
        RecursiveIteratorIterator::CHILD_FIRST
    );
    foreach ($items as $item) {
        if ($item->isDir()) rmdir($item->getPathname());
        else unlink($item->getPathname());
    }
    rmdir($root);
}

function startProbe(string $dataDir, string $bootstrapFile, bool $validate = true): array {
    $command = [PHP_BINARY, __DIR__ . DIRECTORY_SEPARATOR . 'bootstrap_probe.php'];
    if ($validate) $command[] = 'validate';
    $pipes = [];
    $process = proc_open($command, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, [
        'IOT_ALARM_DATA_DIR' => $dataDir,
        'IOT_ALARM_BOOTSTRAP_FILE' => $bootstrapFile
    ]);
    if (!is_resource($process)) throw new RuntimeException('bootstrap probe could not start');
    return [$process, $pipes];
}

$root = sys_get_temp_dir() . DIRECTORY_SEPARATOR . 'iot-alarm-bootstrap-' . bin2hex(random_bytes(6));
if (!mkdir($root, 0700, true) && !is_dir($root)) throw new RuntimeException('temp root unavailable');
register_shutdown_function(static fn() => removeTree($root));

$raceData = $root . DIRECTORY_SEPARATOR . 'race-data';
$raceBootstrap = $root . DIRECTORY_SEPARATOR . 'race-bootstrap.txt';
$workers = [];
for ($i = 0; $i < 16; $i++) $workers[] = startProbe($raceData, $raceBootstrap);
foreach ($workers as [$process, $pipes]) {
    $stdout = stream_get_contents($pipes[1]);
    $stderr = stream_get_contents($pipes[2]);
    fclose($pipes[1]); fclose($pipes[2]);
    $code = proc_close($process);
    if ($code !== 0 || !str_contains((string)$stdout, 'bootstrap concurrency probe: ok')) {
        throw new RuntimeException('parallel bootstrap failed: ' . trim((string)$stderr));
    }
}

// A regular file where a directory is required makes the failure independent
// of root/chmod semantics. No settings file may be activated without credentials.
$blockedParent = $root . DIRECTORY_SEPARATOR . 'not-a-directory';
file_put_contents($blockedParent, 'blocked');
$blockedData = $root . DIRECTORY_SEPARATOR . 'blocked-data';
[$process, $pipes] = startProbe($blockedData, $blockedParent . DIRECTORY_SEPARATOR . 'credentials', false);
stream_get_contents($pipes[1]);
$stderr = stream_get_contents($pipes[2]);
fclose($pipes[1]); fclose($pipes[2]);
$code = proc_close($process);
if ($code === 0 || file_exists($blockedData . DIRECTORY_SEPARATOR . 'settings.json')) {
    throw new RuntimeException('unwritable bootstrap destination did not fail before settings activation: ' . trim((string)$stderr));
}

echo "bootstrap parallel/failure tests: ok\n";
