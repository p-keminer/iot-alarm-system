<?php
declare(strict_types=1);

if (PHP_SAPI !== 'cli') {
    fwrite(STDERR, "CLI only\n");
    exit(64);
}

function expectHttp(bool $condition, string $message): void {
    if (!$condition) throw new RuntimeException($message);
}

function rawHttpRequest(int $port, string $method, string $path, array $headers = [], string $body = ''): array {
    $errno = 0;
    $errstr = '';
    $socket = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5.0);
    if ($socket === false) throw new RuntimeException("HTTP connect failed: $errstr");
    stream_set_timeout($socket, 5);
    $headerLines = [
        "$method $path HTTP/1.0",
        'Host: 127.0.0.1',
        'Connection: close',
        'Content-Length: ' . strlen($body)
    ];
    foreach ($headers as $name => $value) $headerLines[] = $name . ': ' . $value;
    $request = implode("\r\n", $headerLines) . "\r\n\r\n" . $body;
    $offset = 0;
    while ($offset < strlen($request)) {
        $written = fwrite($socket, substr($request, $offset));
        if ($written === false || $written === 0) {
            fclose($socket);
            throw new RuntimeException('HTTP request write failed');
        }
        $offset += $written;
    }
    $response = stream_get_contents($socket);
    fclose($socket);
    if (!is_string($response)) throw new RuntimeException('HTTP response read failed');
    [$rawHeaders, $responseBody] = array_pad(explode("\r\n\r\n", $response, 2), 2, '');
    $lines = explode("\r\n", $rawHeaders);
    if (!preg_match('/^HTTP\/\d\.\d\s+(\d{3})/', $lines[0] ?? '', $match)) {
        throw new RuntimeException('invalid HTTP response');
    }
    $parsedHeaders = [];
    foreach (array_slice($lines, 1) as $line) {
        if (!str_contains($line, ':')) continue;
        [$name, $value] = explode(':', $line, 2);
        $parsedHeaders[strtolower(trim($name))][] = trim($value);
    }
    return ['status' => (int)$match[1], 'headers' => $parsedHeaders, 'body' => $responseBody];
}

function postJson(int $port, array $payload, string $token): array {
    return rawHttpRequest($port, 'POST', '/api.php', [
        'Content-Type' => 'application/json',
        'Authorization' => 'Bearer ' . $token
    ], json_encode($payload, JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR));
}

function postForm(int $port, string $action, array $fields, string $cookie): array {
    return rawHttpRequest($port, 'POST', '/api.php?action=' . rawurlencode($action), [
        'Content-Type' => 'application/x-www-form-urlencoded',
        'Cookie' => $cookie
    ], http_build_query($fields));
}

function removeTestDirectory(string $directory): void {
    if (!is_dir($directory)) return;
    $iterator = new RecursiveIteratorIterator(
        new RecursiveDirectoryIterator($directory, FilesystemIterator::SKIP_DOTS),
        RecursiveIteratorIterator::CHILD_FIRST
    );
    foreach ($iterator as $entry) {
        if ($entry->isDir()) rmdir($entry->getPathname());
        else unlink($entry->getPathname());
    }
    rmdir($directory);
}

$testDir = sys_get_temp_dir() . DIRECTORY_SEPARATOR . 'iot-alarm-http-' . bin2hex(random_bytes(8));
if (!mkdir($testDir, 0700, true) && !is_dir($testDir)) throw new RuntimeException('cannot create test directory');
$previousDataDir = getenv('IOT_ALARM_DATA_DIR');
putenv('IOT_ALARM_DATA_DIR=' . $testDir);

// Bootstrap einmal im Testprozess, damit Token/PIN gelesen, aber nie ausgegeben
// werden. Der HTTP-Server erhaelt nur denselben isolierten Datenpfad.
require dirname(__DIR__, 2) . '/web/includes/config.php';
$settings = json_decode(file_get_contents($testDir . DIRECTORY_SEPARATOR . 'settings.json'), true, 32, JSON_THROW_ON_ERROR);
$bootstrap = file_get_contents($testDir . DIRECTORY_SEPARATOR . '.bootstrap-credentials');
if (!is_string($bootstrap) || !preg_match('/^Alarm PIN: (\d{4,12})$/m', $bootstrap, $pinMatch)) {
    throw new RuntimeException('bootstrap PIN unavailable');
}
$alarmPin = $pinMatch[1];
$receiverToken = $settings['receiver_api_token'];
$senderToken = $settings['sender_api_token'];

$probe = stream_socket_server('tcp://127.0.0.1:0', $errno, $errstr);
if ($probe === false) throw new RuntimeException("cannot reserve test port: $errstr");
$address = stream_socket_get_name($probe, false);
fclose($probe);
$port = (int)substr(strrchr($address, ':'), 1);
$router = __DIR__ . DIRECTORY_SEPARATOR . 'http_test_router.php';
$env = getenv();
$env['IOT_ALARM_DATA_DIR'] = $testDir;
$env['ALARM_IPC_SOCKET'] = $testDir . DIRECTORY_SEPARATOR . 'missing-control.sock';
$pipes = [];
$server = proc_open(
    [PHP_BINARY, '-S', "127.0.0.1:$port", $router],
    [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']],
    $pipes,
    dirname(__DIR__, 2),
    $env,
    ['bypass_shell' => true]
);
if (!is_resource($server)) throw new RuntimeException('cannot start PHP test server');
fclose($pipes[0]);

try {
    $deadline = microtime(true) + 10.0;
    $serverReady = false;
    do {
        $ready = @stream_socket_client("tcp://127.0.0.1:$port", $readyErrno, $readyError, 0.2);
        if (is_resource($ready)) { fclose($ready); $serverReady = true; break; }
        usleep(20000);
    } while (microtime(true) < $deadline);
    if (!$serverReady) {
        throw new RuntimeException('PHP test server did not become ready');
    }

    $sessionResponse = rawHttpRequest($port, 'GET', '/__test_session');
    expectHttp($sessionResponse['status'] === 200, 'test session bootstrap failed');
    $sessionPayload = json_decode($sessionResponse['body'], true, 16, JSON_THROW_ON_ERROR);
    $setCookies = $sessionResponse['headers']['set-cookie'] ?? [];
    expectHttp(isset($sessionPayload['csrf_token']) && isset($setCookies[0]) &&
               preg_match('/^(PHPSESSID=[^;]+)/', $setCookies[0], $cookieMatch) === 1,
               'session cookie or CSRF token missing');
    $cookie = $cookieMatch[1];
    $csrf = $sessionPayload['csrf_token'];

    $utfLog = str_repeat('A', 255) . "\xE2\x82\xAC";
    $utfStatus = str_repeat('B', 127) . "\xE2\x82\xAC";
    $receiver = postJson($port, [
        'source' => 'receiver', 'status_msg' => $utfStatus, 'log' => $utfLog,
        'ip' => '192.0.2.10', 'alarm_state' => false,
        'rssi' => -55, 'heap' => 42000, 'uptime' => 123,
        'reset_reason' => "power\ncontrol"
    ], $receiverToken);
    expectHttp($receiver['status'] === 200, 'valid receiver heartbeat failed');
    expectHttp(!isset($receiver['headers']['set-cookie']), 'device heartbeat created a PHP session cookie');
    $sender = postJson($port, [
        'source' => 'sender', 'status_msg' => 'READY', 'ip' => '192.0.2.11',
        'rssi' => -60, 'heap' => 41000, 'uptime' => 456
    ], $senderToken);
    expectHttp($sender['status'] === 200, 'valid sender heartbeat failed');
    expectHttp(!isset($sender['headers']['set-cookie']), 'sender heartbeat created a PHP session cookie');

    $status = json_decode(file_get_contents($testDir . DIRECTORY_SEPARATOR . 'status.json'), true, 32, JSON_THROW_ON_ERROR);
    expectHttp(isset($status['sender'], $status['receiver']), 'heartbeat lost a source status');
    expectHttp(strlen($status['receiver']['status']) === 127 && preg_match('//u', $status['receiver']['status']) === 1,
               'heartbeat split UTF-8 status text');
    expectHttp(!preg_match('/[\x00-\x1f\x7f]/', $status['receiver']['reset_reason']),
               'heartbeat persisted control characters');
    $apiLog = file_get_contents($testDir . DIRECTORY_SEPARATOR . 'api.log');
    expectHttp(is_string($apiLog) && preg_match('//u', $apiLog) === 1, 'API log contains invalid UTF-8');

    $badSource = postJson($port, ['source' => ['receiver']], 'invalid');
    expectHttp($badSource['status'] === 400, 'array source did not fail with 400');
    $badStatus = postJson($port, ['source' => 'receiver', 'status_msg' => ['bad']], $receiverToken);
    expectHttp($badStatus['status'] === 400, 'array status did not fail with 400');

    $badNode = postForm($port, 'send_command', [
        'csrf_token' => $csrf, 'target' => ['receiver'], 'cmd' => 'ALARM_OFF'
    ], $cookie);
    expectHttp($badNode['status'] === 400, 'array command target did not fail with 400');

    $noPin = postForm($port, 'send_command', [
        'csrf_token' => $csrf, 'target' => 'receiver', 'cmd' => 'ALARM_OFF'
    ], $cookie);
    expectHttp($noPin['status'] === 401, 'receiver state command worked without same-request PIN');
    $withPin = postForm($port, 'send_command', [
        'csrf_token' => $csrf, 'target' => 'receiver', 'cmd' => 'ALARM_OFF', 'alarm_pin' => $alarmPin
    ], $cookie);
    expectHttp($withPin['status'] === 200, 'receiver state command with PIN failed');
    $commands = json_decode(file_get_contents($testDir . DIRECTORY_SEPARATOR . 'commands.json'), true, 32, JSON_THROW_ON_ERROR);
    expectHttp(($commands['receiver'][0]['payload'] ?? null) === 'ALARM_OFF', 'authorized command was not queued');

    $auditWithoutGrant = rawHttpRequest($port, 'GET', '/api.php?action=get_user_logs', ['Cookie' => $cookie]);
    expectHttp($auditWithoutGrant['status'] === 403, 'audit log was readable without a PIN grant');
    $exportWithoutGrant = rawHttpRequest($port, 'GET', '/api.php?action=export_user_logs', ['Cookie' => $cookie]);
    expectHttp($exportWithoutGrant['status'] === 403, 'audit CSV was exportable without a PIN grant');
    $grantAudit = postForm($port, 'verify_pin', [
        'csrf_token' => $csrf, 'alarm_pin' => $alarmPin
    ], $cookie);
    expectHttp($grantAudit['status'] === 200, 'correct PIN did not create an audit grant');
    $auditWithGrant = rawHttpRequest($port, 'GET', '/api.php?action=get_user_logs', ['Cookie' => $cookie]);
    expectHttp($auditWithGrant['status'] === 200, 'audit grant did not authorize log access');

    // Ein nicht erreichbarer externer Daemon darf keinen lokalen Teil-Reset
    // von Queue oder Status ausloesen.
    $statusBeforeFailedReset = file_get_contents($testDir . DIRECTORY_SEPARATOR . 'status.json');
    $commandsBeforeFailedReset = file_get_contents($testDir . DIRECTORY_SEPARATOR . 'commands.json');
    $failedReset = postForm($port, 'system_reset', [
        'csrf_token' => $csrf, 'alarm_pin' => $alarmPin
    ], $cookie);
    expectHttp($failedReset['status'] === 503, 'missing reset daemon did not fail closed');
    expectHttp(file_get_contents($testDir . DIRECTORY_SEPARATOR . 'status.json') === $statusBeforeFailedReset,
               'failed reset mutated status before IPC precondition');
    expectHttp(file_get_contents($testDir . DIRECTORY_SEPARATOR . 'commands.json') === $commandsBeforeFailedReset,
               'failed reset mutated delivery queues before IPC precondition');

    $clearNoPin = postForm($port, 'clear_user_logs', ['csrf_token' => $csrf], $cookie);
    expectHttp($clearNoPin['status'] === 401, 'audit clear worked without same-request PIN');
    $clearWithPin = postForm($port, 'clear_user_logs', [
        'csrf_token' => $csrf, 'alarm_pin' => $alarmPin
    ], $cookie);
    expectHttp($clearWithPin['status'] === 200, 'authorized audit clear failed');
    $audit = json_decode(file_get_contents($testDir . DIRECTORY_SEPARATOR . 'user_logs.json'), true, 32, JSON_THROW_ON_ERROR);
    expectHttp(count($audit) === 1 && ($audit[0]['action'] ?? '') === 'Clear User Logs',
               'audit clear did not retain its own event atomically');

    $audit[0]['user_agent'] = '=SUM(1,1)';
    file_put_contents($testDir . DIRECTORY_SEPARATOR . 'user_logs.json', json_encode($audit, JSON_THROW_ON_ERROR));
    $csv = rawHttpRequest($port, 'GET', '/api.php?action=export_user_logs', ['Cookie' => $cookie]);
    expectHttp($csv['status'] === 200 && str_contains($csv['body'], "'=SUM(1,1)"),
               'audit CSV formula was not neutralized');

    // Eigener progressiver PIN-Lock: dritte Falscheingabe sperrt, auch ein
    // anschliessend korrekter PIN darf die aktive Sperre nicht umgehen.
    $wrongPin = $alarmPin === '000000' ? '999999' : '000000';
    for ($attempt = 1; $attempt <= 3; $attempt++) {
        $pinResponse = postForm($port, 'verify_pin', [
            'csrf_token' => $csrf, 'alarm_pin' => $wrongPin
        ], $cookie);
        $expected = $attempt < 3 ? 401 : 429;
        expectHttp($pinResponse['status'] === $expected, "unexpected PIN attempt status $attempt");
    }
    $lockedCorrect = postForm($port, 'verify_pin', [
        'csrf_token' => $csrf, 'alarm_pin' => $alarmPin
    ], $cookie);
    expectHttp($lockedCorrect['status'] === 429, 'correct PIN bypassed active lockout');

    $preauthFile = $testDir . DIRECTORY_SEPARATOR . 'ratelimit' . DIRECTORY_SEPARATOR .
        'esp-auth-' . hash('sha256', '127.0.0.1') . '.json';
    file_put_contents($preauthFile, '{corrupt');
    $preauthFailure = postJson($port, ['source' => 'receiver'], str_repeat('x', 32));
    expectHttp($preauthFailure['status'] === 503, 'corrupt pre-auth ledger failed open');

    $generalRateFile = $testDir . DIRECTORY_SEPARATOR . 'ratelimit' . DIRECTORY_SEPARATOR . md5('127.0.0.1') . '.json';
    file_put_contents($generalRateFile, '{corrupt');
    $generalFailure = postJson($port, ['source' => 'sender', 'status_msg' => 'READY'], $senderToken);
    expectHttp($generalFailure['status'] === 503, 'corrupt authenticated rate ledger failed open');

    echo "HTTP routing/security tests: ok\n";
} finally {
    proc_terminate($server);
    foreach ([$pipes[1], $pipes[2]] as $pipe) {
        if (is_resource($pipe)) { stream_get_contents($pipe); fclose($pipe); }
    }
    proc_close($server);
    removeTestDirectory($testDir);
    if ($previousDataDir === false) putenv('IOT_ALARM_DATA_DIR');
    else putenv('IOT_ALARM_DATA_DIR=' . $previousDataDir);
}
