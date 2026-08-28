<?php

// api.php - V5.3 (Security Hardened + File Locking + SD-Card Safe)

header('X-Content-Type-Options: nosniff');
header('X-Frame-Options: DENY');
header('Cache-Control: no-store, no-cache, must-revalidate');

$configuredDataDir = getenv('IOT_ALARM_DATA_DIR');
$dataDir = ((is_string($configuredDataDir) && $configuredDataDir !== '')
    ? rtrim($configuredDataDir, '/\\')
    : __DIR__ . DIRECTORY_SEPARATOR . 'data') . DIRECTORY_SEPARATOR;
if (!is_dir($dataDir)) mkdir($dataDir, 0750, true);

$statusFile   = $dataDir . 'status.json';
$logFile      = $dataDir . 'api.log';
$runtimeLogFile = $dataDir . 'log.txt';
$userLogFile  = $dataDir . 'user_logs.json';
$cmdFile      = $dataDir . 'commands.json';
$confFile     = $dataDir . 'settings.json';
$rateLimitDir = $dataDir . 'ratelimit/';
$deliveryStateFile = $dataDir . 'delivery_state.json';
$deliveryLockFile  = $dataDir . 'delivery_queue.lock';
const DELIVERY_QUEUE_LIMIT = 16;
const ALARM_IPC_DEFAULT_SOCKET = '/run/iot-alarm-monitor/control.sock';
const ALARM_IPC_CONNECT_TIMEOUT_SECONDS = 1.0;
const ALARM_IPC_IO_TIMEOUT_SECONDS = 3.0;
const ALARM_IPC_MAX_REQUEST_BYTES = 4096;
const ALARM_IPC_MAX_RESPONSE_BYTES = 8192;
const ALARM_RECORDING_FILENAME_PATTERN = '/^(?:alarm|manual)_\d{8}_\d{6}(?:_\d{1,3})?\.(?:avi|mp4|mkv)$/';
const AUDIT_PIN_GRANT_SECONDS = 300;

if (!is_dir($rateLimitDir)) mkdir($rateLimitDir, 0750, true);

/**
 * Sends one short-lived JSON-line request to the local alarm monitor daemon.
 * The daemon is the sole owner of the Uno serial connection and recorder.
 */
function alarmIpcRequest($action, $filename = null) {
    $allowedActions = [
        'arm', 'disarm', 'manual_record_start', 'manual_record_stop',
        'delete_recording', 'delete_all_recordings', 'clear_runtime_log'
    ];
    if (!is_string($action) || !in_array($action, $allowedActions, true)) {
        throw new RuntimeException('Control request unavailable');
    }
    if ($action === 'delete_recording') {
        if (!is_string($filename) || !preg_match(ALARM_RECORDING_FILENAME_PATTERN, $filename)) {
            throw new RuntimeException('Control request unavailable');
        }
    } elseif ($filename !== null) {
        throw new RuntimeException('Control request unavailable');
    }

    $configuredSocket = getenv('ALARM_IPC_SOCKET');
    $socketPath = (is_string($configuredSocket) && $configuredSocket !== '')
        ? $configuredSocket
        : ALARM_IPC_DEFAULT_SOCKET;
    if ($socketPath[0] !== '/' || strlen($socketPath) > 100 || preg_match('/[\x00-\x1f\x7f]/', $socketPath)) {
        throw new RuntimeException('Control service unavailable');
    }

    $requestId = bin2hex(random_bytes(16));
    $issuedAt = time();
    $request = [
        'version'    => 1,
        'id'         => $requestId,
        'action'     => $action,
        'issued_at'  => $issuedAt,
        'expires_at' => $issuedAt + 5
    ];
    if ($filename !== null) $request['filename'] = $filename;

    try {
        $payload = json_encode($request, JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR) . "\n";
        if (strlen($payload) > ALARM_IPC_MAX_REQUEST_BYTES) {
            throw new RuntimeException('Control request unavailable');
        }

        $errno = 0;
        $errstr = '';
        $socket = @stream_socket_client(
            'unix://' . $socketPath,
            $errno,
            $errstr,
            ALARM_IPC_CONNECT_TIMEOUT_SECONDS,
            STREAM_CLIENT_CONNECT
        );
        if ($socket === false) throw new RuntimeException('Control service unavailable');

        try {
            stream_set_blocking($socket, true);
            stream_set_timeout($socket, 0, 250000);
            $deadline = microtime(true) + ALARM_IPC_IO_TIMEOUT_SECONDS;

            $offset = 0;
            $payloadLength = strlen($payload);
            while ($offset < $payloadLength) {
                if (microtime(true) >= $deadline) throw new RuntimeException('Control service unavailable');
                $written = @fwrite($socket, substr($payload, $offset));
                $meta = stream_get_meta_data($socket);
                if ($written === false || $written === 0 || !empty($meta['timed_out'])) {
                    throw new RuntimeException('Control service unavailable');
                }
                $offset += $written;
            }
            if (!@fflush($socket)) throw new RuntimeException('Control service unavailable');

            $responseLine = '';
            while (strpos($responseLine, "\n") === false) {
                if (microtime(true) >= $deadline) throw new RuntimeException('Control service unavailable');
                $remaining = ALARM_IPC_MAX_RESPONSE_BYTES + 1 - strlen($responseLine);
                if ($remaining <= 0) throw new RuntimeException('Control service unavailable');
                $chunk = @fread($socket, min(1024, $remaining));
                $meta = stream_get_meta_data($socket);
                if ($chunk === false) throw new RuntimeException('Control service unavailable');
                if (!empty($meta['timed_out'])) continue;
                if ($chunk === '') {
                    if (feof($socket)) throw new RuntimeException('Control service unavailable');
                    continue;
                }
                $responseLine .= $chunk;
            }
        } finally {
            fclose($socket);
        }

        if (strlen($responseLine) > ALARM_IPC_MAX_RESPONSE_BYTES) {
            throw new RuntimeException('Control service unavailable');
        }
        [$firstLine, $trailing] = array_pad(explode("\n", $responseLine, 2), 2, '');
        if (trim($trailing) !== '') throw new RuntimeException('Control service unavailable');
        $response = json_decode($firstLine, true, 16, JSON_THROW_ON_ERROR);
        if (!is_array($response)
            || ($response['ok'] ?? null) !== true
            || !isset($response['id'])
            || !is_string($response['id'])
            || !hash_equals($requestId, $response['id'])
            || !isset($response['result'])
            || !is_array($response['result'])) {
            throw new RuntimeException('Control service unavailable');
        }
        return $response['result'];
    } catch (Throwable $e) {
        throw new RuntimeException('Control service unavailable');
    }
}

// Einheitliche Bootstrap-/Migrationslogik für Dashboard und API. Sie erzeugt
// installationsspezifische Secrets und ersetzt bekannte Alt-Defaults.
require_once __DIR__ . '/includes/config.php';

// parseUserAgent() wird in logUserAction() benötigt (Gerät, OS, Browser aus UA-String)
require_once __DIR__ . '/includes/brute_force.php';

// ============================================================
// SECURITY & LOCKING HELPERS
// ============================================================

/**
 * Sicheres JSON-Lesen mit Shared Lock (LOCK_SH)
 * Respektiert Write-Locks von Python/PHP
 */
function readJsonLocked($file) {
    if (!file_exists($file)) return [];

    $fp = fopen($file, 'r');
    if (!$fp) return [];

    $data = [];
    if (flock($fp, LOCK_SH)) { // 🔒 Shared Lock (Warten auf Writer)
        // Aus dem bereits gesperrten Handle lesen. Ein paralleler atomarer
        // Rename darf weder einen pfadbasierten filesize-Race noch Stat-Cache
        // in die Entscheidung einbringen.
        $json = stream_get_contents($fp);
        if (is_string($json) && trim($json) !== '') $data = json_decode($json, true);
        flock($fp, LOCK_UN);
    }
    fclose($fp);

    return is_array($data) ? $data : [];
}

/** Atomare Read-Modify-Write-Transaktion unter genau einem Exclusive-Lock. */
function updateJsonLockedStrict($file, $callback) {
    $dir = dirname($file);
    if (!is_dir($dir) && !mkdir($dir, 0750, true) && !is_dir($dir))
        throw new RuntimeException('State directory cannot be created');
    $fp = fopen($file, 'c+b');
    if (!$fp || !flock($fp, LOCK_EX)) {
        if ($fp) fclose($fp);
        throw new RuntimeException('State file cannot be locked');
    }
    try {
        rewind($fp);
        $raw = stream_get_contents($fp);
        if ($raw === false) throw new RuntimeException('State file cannot be read');
        if (trim($raw) === '') {
            $current = [];
        } else {
            $current = json_decode($raw, true);
            if (!is_array($current) || json_last_error() !== JSON_ERROR_NONE)
                throw new RuntimeException('State file contains invalid JSON');
        }
        $updated = $callback($current);
        if (!is_array($updated)) throw new RuntimeException('State mutation returned invalid data');
        $encoded = json_encode($updated, JSON_UNESCAPED_SLASHES);
        if ($encoded === false || !rewind($fp) || !ftruncate($fp, 0))
            throw new RuntimeException('State file cannot be rewritten');
        $offset = 0;
        while ($offset < strlen($encoded)) {
            $written = fwrite($fp, substr($encoded, $offset));
            if ($written === false || $written === 0)
                throw new RuntimeException('State write was incomplete');
            $offset += $written;
        }
        if (!fflush($fp) || (function_exists('fsync') && !fsync($fp)))
            throw new RuntimeException('State sync failed');
        @chmod($file, 0640);
        return $updated;
    } finally {
        flock($fp, LOCK_UN);
        fclose($fp);
    }
}

/** Serialisierte Mutation einer kleinen Textdatei mit fsync. */
function updateTextLockedStrict($file, $callback) {
    $dir = dirname($file);
    if (!is_dir($dir) && !mkdir($dir, 0750, true) && !is_dir($dir))
        throw new RuntimeException('Log directory cannot be created');
    $fp = fopen($file, 'c+b');
    if (!$fp || !flock($fp, LOCK_EX)) {
        if ($fp) fclose($fp);
        throw new RuntimeException('Log file cannot be locked');
    }
    try {
        rewind($fp);
        $current = stream_get_contents($fp);
        if ($current === false) throw new RuntimeException('Log file cannot be read');
        $updated = $callback($current);
        if (!is_string($updated)) throw new RuntimeException('Log mutation returned invalid data');
        if (!rewind($fp) || !ftruncate($fp, 0)) throw new RuntimeException('Log file cannot be rewritten');
        $offset = 0;
        while ($offset < strlen($updated)) {
            $written = fwrite($fp, substr($updated, $offset));
            if ($written === false || $written === 0) throw new RuntimeException('Log write was incomplete');
            $offset += $written;
        }
        if (!fflush($fp) || (function_exists('fsync') && !fsync($fp)))
            throw new RuntimeException('Log sync failed');
        @chmod($file, 0640);
    } finally {
        flock($fp, LOCK_UN);
        fclose($fp);
    }
}

/** Begrenzt das PHP-seitige Geraeteprotokoll ohne Rotation-Races. */
function appendApiLogLine($line) {
    global $logFile;
    $line = utf8SafeByteLimit((string)$line, 1024);
    updateTextLockedStrict($logFile, function($current) use ($line) {
        if (strlen($current) + strlen($line) > 524288) {
            $lines = preg_split('/(?<=\n)/', $current, -1, PREG_SPLIT_NO_EMPTY);
            $current = implode('', array_slice($lines ?: [], -200));
        }
        return $current . $line;
    });
}

function readTextTailLocked($file, $limit) {
    if (!file_exists($file)) return [];
    $fp = fopen($file, 'rb');
    if (!$fp || !flock($fp, LOCK_SH)) {
        if ($fp) fclose($fp);
        return [];
    }
    try {
        $raw = stream_get_contents($fp);
        if ($raw === false) return [];
        $lines = preg_split('/(?<=\n)/', $raw, -1, PREG_SPLIT_NO_EMPTY);
        return array_slice($lines ?: [], -max(1, (int)$limit));
    } finally {
        flock($fp, LOCK_UN);
        fclose($fp);
    }
}

/** Kritische Zustandsdateien duerfen bei I/O-/JSON-Fehlern nie als leer gelten. */
function readJsonLockedStrict($file, $allowMissing = false) {
    if (!file_exists($file)) {
        if ($allowMissing) return [];
        throw new RuntimeException('Required state file is missing');
    }
    $fp = fopen($file, 'rb');
    if (!$fp) throw new RuntimeException('State file cannot be opened');
    try {
        if (!flock($fp, LOCK_SH)) throw new RuntimeException('State file cannot be locked');
        $json = stream_get_contents($fp);
        if ($json === false) throw new RuntimeException('State file cannot be read');
        $data = json_decode($json, true);
        if (!is_array($data) || json_last_error() !== JSON_ERROR_NONE) {
            throw new RuntimeException('State file contains invalid JSON');
        }
        return $data;
    } finally {
        @flock($fp, LOCK_UN);
        fclose($fp);
    }
}

/** Vollstaendiger, fsync-gesicherter Temp-Write mit atomarem Rename. */
function writeJsonAtomicStrict($file, $data) {
    $json = json_encode($data, JSON_UNESCAPED_SLASHES);
    if ($json === false) throw new RuntimeException('State cannot be encoded');
    $dir = dirname($file);
    if (!is_dir($dir) || !is_writable($dir)) throw new RuntimeException('State directory is not writable');
    $tmp = $file . '.tmp.' . bin2hex(random_bytes(4));
    $fp = fopen($tmp, 'xb');
    if (!$fp) throw new RuntimeException('Temporary state file cannot be opened');
    if (!@chmod($tmp, 0640)) {
        fclose($fp);
        @unlink($tmp);
        throw new RuntimeException('Temporary state permissions cannot be secured');
    }
    try {
        $offset = 0;
        $length = strlen($json);
        while ($offset < $length) {
            $written = fwrite($fp, substr($json, $offset));
            if ($written === false || $written === 0) throw new RuntimeException('State write was incomplete');
            $offset += $written;
        }
        if (!fflush($fp)) throw new RuntimeException('State flush failed');
        if (function_exists('fsync') && !fsync($fp)) throw new RuntimeException('State fsync failed');
    } catch (Throwable $e) {
        fclose($fp);
        @unlink($tmp);
        throw $e;
    }
    fclose($fp);
    if (!rename($tmp, $file)) {
        @unlink($tmp);
        throw new RuntimeException('State atomic replace failed');
    }
    fsyncDirectoryAfterMutation($dir);
}

function unlinkStateDurably($file) {
    if (!file_exists($file)) return;
    $dir = dirname($file);
    if (!unlink($file)) throw new RuntimeException('State file cannot be removed');
    fsyncDirectoryAfterMutation($dir);
}

/** Führt eine Queue-Operation serialisiert aus. */
function withDeliveryLock($callback) {
    global $deliveryLockFile;
    $fp = fopen($deliveryLockFile, 'c+');
    if (!$fp || !flock($fp, LOCK_EX)) {
        if ($fp) fclose($fp);
        throw new RuntimeException('Delivery queue lock unavailable');
    }
    try {
        return $callback();
    } finally {
        flock($fp, LOCK_UN);
        fclose($fp);
    }
}

/**
 * Zeitunabhängiger Replay-Counter. time() dient nur als hoher Startwert nach
 * einer Server-Neuinstallation; die eigentliche Sicherheit ist monoton.
 */
function nextDeliverySequence($source) {
    global $deliveryStateFile;
    $state = readJsonLockedStrict($deliveryStateFile, true);
    $last = (int)($state[$source] ?? 0);
    $next = max($last + 1, time());
    if ($next <= 0 || $next > 0xFFFFFFFF) {
        throw new RuntimeException('Delivery sequence space exhausted');
    }
    $state[$source] = $next;
    writeJsonAtomicStrict($deliveryStateFile, $state);
    return $next;
}

function makeQueuedDelivery($source, $type, $payload) {
    $record = [
        'id'       => bin2hex(random_bytes(16)),
        'sequence' => nextDeliverySequence($source),
        'type'     => $type,
        'target'   => $source,
        'payload'  => $payload
    ];
    if (!isDeliveryRecord($record, $source, $type)) {
        throw new InvalidArgumentException('Invalid delivery payload for target');
    }
    return $record;
}

function allowedCommandsForTarget($target) {
    return [
        'sender'   => ['REBOOT'],
        'receiver' => ['ALARM_ON', 'ALARM_OFF', 'REBOOT']
    ][$target] ?? [];
}

function isAllowedConfigPayload($target, $payload) {
    if (!in_array($target, ['sender', 'receiver'], true) || !is_array($payload) ||
        $payload === [] || array_is_list($payload)) return false;
    $maxLengths = ['mssid'=>32, 'mpass'=>64, 'bssid'=>32, 'bpass'=>64, 'tpass'=>20];
    foreach ($payload as $key => $value) {
        if (!is_string($key) || !array_key_exists($key, $maxLengths) || !is_string($value) ||
            strlen($value) > $maxLengths[$key]) return false;
    }
    return true;
}

function isDeliveryRecord($value, $expectedTarget = null, $expectedType = null) {
    if (!is_array($value) || !isset($value['id'], $value['sequence'], $value['type'], $value['target']) ||
        !array_key_exists('payload', $value)) return false;
    if (!is_string($value['id']) || !preg_match('/^[a-f0-9]{32}$/', $value['id']) ||
        !is_int($value['sequence']) || $value['sequence'] <= 0 || $value['sequence'] > 0xFFFFFFFF ||
        !is_string($value['type']) || !in_array($value['type'], ['command', 'config'], true) ||
        !is_string($value['target']) || !in_array($value['target'], ['sender', 'receiver'], true)) return false;
    if ($expectedTarget !== null && !hash_equals((string)$expectedTarget, $value['target'])) return false;
    if ($expectedType !== null && !hash_equals((string)$expectedType, $value['type'])) return false;
    if ($value['type'] === 'command') {
        return is_string($value['payload']) &&
               in_array($value['payload'], allowedCommandsForTarget($value['target']), true);
    }
    return isAllowedConfigPayload($value['target'], $value['payload']);
}

/** Migriert Einzelwerte/Einzel-Envelopes in eine verlustfreie FIFO-Liste. */
function normalizeDeliveryQueue($source, $type, $stored) {
    if ($stored === null || $stored === []) return [];
    if (is_array($stored) && isset($stored['id'], $stored['sequence'], $stored['type']) &&
        array_key_exists('payload', $stored)) {
        $record = normalizeLegacyDelivery($source, $type, $stored);
        return [$record];
    }
    if (is_array($stored) && isset($stored[0])) {
        $queue = [];
        foreach ($stored as $record) {
            $queue[] = normalizeLegacyDelivery($source, $type, $record);
        }
        return $queue;
    }
    return [normalizeLegacyDelivery($source, $type, $stored)];
}

/** Speichert Befehle und Konfigurationen als FIFO, bis exakt ihre ID bestaetigt ist. */
function queueDelivery($source, $type, $payload) {
    global $cmdFile, $dataDir;
    return withDeliveryLock(function() use ($source, $type, $payload, $cmdFile, $dataDir) {
        $commands = readJsonLockedStrict($cmdFile, true);
        $commandQueue = normalizeDeliveryQueue($source, 'command', $commands[$source] ?? null);
        $updateFile = $dataDir . 'update_' . $source . '.json';
        $configQueue = file_exists($updateFile)
            ? normalizeDeliveryQueue($source, 'config', readJsonLockedStrict($updateFile))
            : [];

        // Zustandssetzende Alarmbefehle und Konfigurationen koaleszieren. Damit
        // fuehrt ein offline gewesener Knoten beim Wiederkommen keinen veralteten
        // Zwischenzustand aus. Einmalige REBOOT-Befehle bleiben FIFO.
        if ($type === 'command' && in_array($payload, ['ALARM_ON', 'ALARM_OFF'], true)) {
            $commandQueue = array_values(array_filter($commandQueue, function($record) {
                return !in_array($record['payload'] ?? '', ['ALARM_ON', 'ALARM_OFF'], true);
            }));
        } elseif ($type === 'config') {
            $mergedPayload = [];
            foreach ($configQueue as $record) {
                if (!is_array($record['payload'] ?? null)) throw new RuntimeException('Invalid pending config payload');
                $mergedPayload = array_replace($mergedPayload, $record['payload']);
            }
            $payload = array_replace($mergedPayload, $payload);
            $configQueue = [];
        }

        if (count($commandQueue) + count($configQueue) >= DELIVERY_QUEUE_LIMIT) {
            throw new OverflowException('Delivery queue is full');
        }

        $queued = makeQueuedDelivery($source, $type, $payload);
        if ($type === 'command') $commandQueue[] = $queued;
        else $configQueue[] = $queued;

        if ($commandQueue) $commands[$source] = $commandQueue;
        else unset($commands[$source]);
        writeJsonAtomicStrict($cmdFile, $commands);
        if ($configQueue) writeJsonAtomicStrict($updateFile, $configQueue);
        elseif (file_exists($updateFile)) unlinkStateDurably($updateFile);
        return $queued;
    });
}

function normalizeLegacyDelivery($source, $type, $queued) {
    if (is_array($queued) && (isset($queued['id']) || isset($queued['sequence']) ||
        isset($queued['type']) || array_key_exists('payload', $queued))) {
        if (!isset($queued['id'], $queued['sequence'], $queued['type']) || !array_key_exists('payload', $queued)) {
            throw new RuntimeException('Incomplete legacy delivery record');
        }
        if (!isset($queued['target'])) $queued['target'] = $source;
        if (!isDeliveryRecord($queued, $source, $type)) {
            throw new RuntimeException('Invalid legacy delivery record');
        }
        return $queued;
    }
    if ($type === 'command' && (!is_string($queued) ||
        !in_array($queued, allowedCommandsForTarget($source), true))) {
        throw new RuntimeException('Unsupported legacy command');
    }
    if ($type === 'config' && !isAllowedConfigPayload($source, $queued)) {
        throw new RuntimeException('Invalid legacy config');
    }
    return makeQueuedDelivery($source, $type, $queued);
}

/** Entfernt ausschließlich die exakt vom ESP bestätigte Message-ID. */
function acknowledgeDelivery($source, $id) {
    global $cmdFile, $dataDir;
    if (!preg_match('/^[a-f0-9]{32}$/', $id)) return false;

    return withDeliveryLock(function() use ($source, $id, $cmdFile, $dataDir) {
        $removed = false;
        $commands = readJsonLockedStrict($cmdFile, true);
        if (array_key_exists($source, $commands)) {
            $queue = normalizeDeliveryQueue($source, 'command', $commands[$source]);
            $remaining = [];
            foreach ($queue as $record) {
                if (hash_equals((string)($record['id'] ?? ''), $id)) $removed = true;
                else $remaining[] = $record;
            }
            if ($remaining) $commands[$source] = $remaining;
            else unset($commands[$source]);
            writeJsonAtomicStrict($cmdFile, $commands);
        }

        $updateFile = $dataDir . 'update_' . $source . '.json';
        if (file_exists($updateFile)) {
            $queue = normalizeDeliveryQueue($source, 'config', readJsonLockedStrict($updateFile));
            $remaining = [];
            foreach ($queue as $record) {
                if (hash_equals((string)($record['id'] ?? ''), $id)) $removed = true;
                else $remaining[] = $record;
            }
            if ($remaining) writeJsonAtomicStrict($updateFile, $remaining);
            else unlinkStateDurably($updateFile);
        }
        return $removed;
    });
}

function verifyDeliveryAck($source, $id, $signature, $hmacSecret) {
    if (!preg_match('/^[a-f0-9]{32}$/', $id) ||
        !preg_match('/^[a-f0-9]{64}$/', $signature) || strlen($hmacSecret) < 32) return false;
    $material = "ALARMv2ACK\n{$source}\n{$id}";
    return hash_equals(hash_hmac('sha256', $material, $hmacSecret), $signature);
}

/** Liefert die älteste offene Nachricht; sie bleibt bis zum ACK in der Queue. */
function getPendingDelivery($source) {
    global $cmdFile, $dataDir;
    return withDeliveryLock(function() use ($source, $cmdFile, $dataDir) {
        $candidates = [];
        $commands = readJsonLockedStrict($cmdFile, true);
        if (array_key_exists($source, $commands)) {
            $queue = normalizeDeliveryQueue($source, 'command', $commands[$source]);
            if ($queue !== $commands[$source]) {
                $commands[$source] = $queue;
                writeJsonAtomicStrict($cmdFile, $commands);
            }
            if ($queue) $candidates[] = $queue[0];
        }

        $updateFile = $dataDir . 'update_' . $source . '.json';
        if (file_exists($updateFile)) {
            $raw = readJsonLockedStrict($updateFile);
            $queue = normalizeDeliveryQueue($source, 'config', $raw);
            if ($queue !== $raw) writeJsonAtomicStrict($updateFile, $queue);
            if ($queue) $candidates[] = $queue[0];
        }

        if (!$candidates) return null;
        usort($candidates, function($a, $b) {
            return ((int)$a['sequence']) <=> ((int)$b['sequence']);
        });
        return $candidates[0];
    });
}

/** Leert nur zustellbare Records; monotone Replay-Counter bleiben erhalten. */
function resetDeliveryQueues() {
    global $cmdFile, $dataDir;
    return withDeliveryLock(function() use ($cmdFile, $dataDir) {
        writeJsonAtomicStrict($cmdFile, []);
        foreach (['sender', 'receiver'] as $node) {
            unlinkStateDurably($dataDir . 'update_' . $node . '.json');
        }
        return true;
    });
}

function encryptDeliveryConfig($source, $id, $plaintext, $nonce, $hmacSecret) {
    $ciphertext = '';
    $length = strlen($plaintext);
    for ($offset = 0, $block = 0; $offset < $length; $offset += 32, $block++) {
        $material = "ALARMv2ENC\n{$source}\n{$id}\n{$nonce}\n{$block}";
        $stream = hash_hmac('sha256', $material, $hmacSecret, true);
        $chunkLength = min(32, $length - $offset);
        for ($i = 0; $i < $chunkLength; $i++) {
            $ciphertext .= $plaintext[$offset + $i] ^ $stream[$i];
        }
    }
    return base64_encode($ciphertext);
}

function encodeDeliveryEnvelope($source, $queued, $hmacSecret) {
    if (!is_string($hmacSecret) || strlen($hmacSecret) < 32 ||
        !isDeliveryRecord($queued, $source)) return null;
    $type = $queued['type'];
    $plaintext = $type === 'config'
        ? json_encode($queued['payload'], JSON_UNESCAPED_SLASHES)
        : (string)$queued['payload'];
    if ($plaintext === false || strlen($plaintext) > 512) return null;

    $id = (string)$queued['id'];
    $sequence = (int)$queued['sequence'];
    $nonce = $type === 'config' ? bin2hex(random_bytes(16)) : '';
    $payload = $type === 'config'
        ? encryptDeliveryConfig($source, $id, $plaintext, $nonce, $hmacSecret)
        : $plaintext;
    $material = "ALARMv2\n{$source}\n{$id}\n{$sequence}\n{$type}\n{$nonce}\n{$payload}";
    return [
        'version'  => 2,
        'id'       => $id,
        'sequence' => $sequence,
        'type'     => $type,
        'nonce'    => $nonce,
        'payload'  => $payload,
        'sig'      => hash_hmac('sha256', $material, $hmacSecret)
    ];
}

/** Ausschließlich HTTP-sichere, nicht geheime Dashboard-Einstellungen. */
function publicSettings($settings) {
    $allowed = [
        'refresh_rate', 'site_title', 'timeout_active', 'timeout_minutes',
        'camera_port', 'bootstrap_admin_pending', 'bootstrap_pin_pending'
    ];
    return array_intersect_key(is_array($settings) ? $settings : [], array_flip($allowed));
}

function requireAuth($dieOnFail = true) {
    global $confFile;
    if (isset($_SESSION['loggedin']) && $_SESSION['loggedin'] === true) {
        if (file_exists($confFile)) {
            $s = json_decode(file_get_contents($confFile), true);
            if (!empty($s['timeout_active']) && isset($_SESSION['last_activity'])) {
                $timeout = (($s['timeout_minutes'] ?? 5) * 60);
                if (time() - $_SESSION['last_activity'] > $timeout) {
                    session_destroy();
                    if ($dieOnFail) {
                        http_response_code(403);
                        die(json_encode(['error' => 'Session expired']));
                    }
                    return false;
                }
            }
        }
        return true;
    }
    if ($dieOnFail) {
        http_response_code(403);
        die(json_encode(['error' => 'Access Denied']));
    }
    return false;
}

function getCSRFToken() {
    if (empty($_SESSION['csrf_token'])) $_SESSION['csrf_token'] = bin2hex(random_bytes(32));
    return $_SESSION['csrf_token'];
}

function validateCSRF() {
    $token = $_POST['csrf_token'] ?? $_SERVER['HTTP_X_CSRF_TOKEN'] ?? '';
    if (empty($token) || !hash_equals($_SESSION['csrf_token'] ?? '', $token)) {
        http_response_code(403);
        logUserAction("CSRF_BLOCKED", "Ungueltiger CSRF-Token");
        die(json_encode(['error' => 'Invalid CSRF token']));
    }
}

function checkRateLimit($maxRequests = 60, $windowSeconds = 60) {
    global $rateLimitDir;
    $ip   = $_SERVER['REMOTE_ADDR'];
    $file = $rateLimitDir . md5($ip) . '.json';
    $now  = time();

    // LOCK_EX: Atomares Read-Modify-Write verhindert Race-Conditions
    // bei parallelen Requests (z.B. XHR-Floods).
    $fp = fopen($file, 'c+b');
    if (!$fp || !flock($fp, LOCK_EX)) {
        if ($fp) fclose($fp);
        http_response_code(503);
        die(json_encode(['error' => 'Rate-limit storage unavailable']));
    }

    $storageOk = true;
    $limited = false;
    try {
        rewind($fp);
        $raw = stream_get_contents($fp);
        if ($raw === false) {
            $storageOk = false;
        } elseif (trim($raw) === '') {
            $data = [];
        } else {
            $data = json_decode($raw, true);
            if (!is_array($data) || json_last_error() !== JSON_ERROR_NONE) $storageOk = false;
        }
        if ($storageOk) {
            $data = array_values(array_filter($data, function($t) use ($now, $windowSeconds) {
                return is_int($t) && ($now - $t) < $windowSeconds;
            }));
            $limited = count($data) >= $maxRequests;
            if (!$limited) {
                $data[] = $now;
                $storageOk = bfWriteJsonToHandle($fp, $data);
            }
        }
    } finally {
        flock($fp, LOCK_UN);
        fclose($fp);
    }
    if (!$storageOk) {
        http_response_code(503);
        die(json_encode(['error' => 'Rate-limit storage unavailable']));
    }
    if ($limited) {
        http_response_code(429);
        die(json_encode(['error' => 'Too many requests']));
    }
}

function getAuthorizationHeader() {
    if (!empty($_SERVER['HTTP_AUTHORIZATION']))          return $_SERVER['HTTP_AUTHORIZATION'];
    if (!empty($_SERVER['REDIRECT_HTTP_AUTHORIZATION'])) return $_SERVER['REDIRECT_HTTP_AUTHORIZATION'];
    if (!empty($_SERVER['HTTP_X_ESP_TOKEN']))             return 'Bearer ' . $_SERVER['HTTP_X_ESP_TOKEN'];
    if (function_exists('getallheaders')) {
        $headers = getallheaders();
        if ($headers) {
            foreach ($headers as $key => $value) {
                $lower = strtolower($key);
                if ($lower === 'authorization') return $value;
                if ($lower === 'x-esp-token')   return 'Bearer ' . $value;
            }
        }
    }
    return '';
}

function deviceCredential($settings, $source, $kind) {
    if (!in_array($source, ['sender', 'receiver', 'camera'], true) ||
        !in_array($kind, ['token', 'hmac'], true)) return '';
    $suffix = $kind === 'token' ? '_api_token' : '_hmac_secret';
    $value = $settings[$source . $suffix] ?? '';
    return is_string($value) ? $value : '';
}

/** Begrenzt und sampelt fehlgeschlagene Geraeteauthentifizierungen vor Auth. */
function registerEspAuthFailure($source) {
    global $rateLimitDir;
    $remote = $_SERVER['REMOTE_ADDR'] ?? 'unknown';
    $ip = is_string($remote) && filter_var($remote, FILTER_VALIDATE_IP) ? $remote : 'unknown';
    $file = $rateLimitDir . 'esp-auth-' . hash('sha256', $ip) . '.json';
    $now = time();
    $shouldLog = false;
    $blocked = false;
    $fp = fopen($file, 'c+b');
    if (!$fp || !flock($fp, LOCK_EX)) {
        if ($fp) fclose($fp);
        throw new RuntimeException('Pre-auth rate-limit storage unavailable');
    }
    try {
        rewind($fp);
        $raw = stream_get_contents($fp);
        if ($raw === false) throw new RuntimeException('Pre-auth rate-limit storage unavailable');
        $attempts = trim($raw) === '' ? [] : json_decode($raw, true);
        if (!is_array($attempts) || json_last_error() !== JSON_ERROR_NONE) {
            throw new RuntimeException('Pre-auth rate-limit storage corrupt');
        }
        $attempts = array_values(array_filter($attempts, function($value) use ($now) {
            return is_int($value) && $now - $value < 300;
        }));
        $shouldLog = count($attempts) < 5;
        $blocked = count($attempts) >= 60;
        if (!$blocked) $attempts[] = $now;
        if (!bfWriteJsonToHandle($fp, $attempts)) {
            throw new RuntimeException('Pre-auth rate-limit storage unavailable');
        }
    } finally {
        flock($fp, LOCK_UN);
        fclose($fp);
    }
    if ($shouldLog) {
        appendApiLogLine(date("[d.m.Y H:i:s]") . " SECURITY: ESP auth failed from $ip for $source\n");
    }
    return $blocked;
}

function validateESPToken($source, $settings) {
    $authHeader = getAuthorizationHeader();
    $token = '';
    if (preg_match('/^Bearer\s+(.+)$/i', $authHeader, $matches)) {
        $token = $matches[1];
    } elseif (!empty($authHeader)) {
        $token = $authHeader;
    }
    $expectedToken = deviceCredential($settings, $source, 'token');
    if (strlen($token) < 32 || strlen($expectedToken) < 32 || !hash_equals($expectedToken, $token)) {
        try {
            $blocked = registerEspAuthFailure($source);
        } catch (RuntimeException $e) {
            http_response_code(503);
            die(json_encode(['error' => 'Authentication rate-limit storage unavailable']));
        }
        http_response_code($blocked ? 429 : 401);
        die(json_encode(['error' => $blocked ? 'Too many authentication failures' : 'Invalid device token']));
    }
}

function sanitizeNodeName($name) {
    $allowed = ['sender', 'receiver', 'camera'];
    if (!is_string($name)) {
        http_response_code(400);
        die(json_encode(['error' => 'Invalid node type']));
    }
    $name = strtolower(trim($name));
    if (!in_array($name, $allowed, true)) {
        http_response_code(400);
        die(json_encode(['error' => 'Invalid node: ' . htmlspecialchars($name)]));
    }
    return $name;
}

function sanitizeCommand($cmd) {
    $allowed = ['ALARM_ON', 'ALARM_OFF', 'REBOOT'];
    if (!is_string($cmd)) {
        http_response_code(400);
        die(json_encode(['error' => 'Invalid command type']));
    }
    $cmd = strtoupper(trim($cmd));
    if (!in_array($cmd, $allowed, true)) {
        http_response_code(400);
        die(json_encode(['error' => 'Invalid command: ' . htmlspecialchars($cmd)]));
    }
    return $cmd;
}

function isHttpsRequest() {
    return (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off') ||
           ((int)($_SERVER['SERVER_PORT'] ?? 0) === 443);
}

/** PINs werden ausschließlich über TLS übertragen und nur gegen bcrypt geprüft. */
function verifyPin($pin, $settings) {
    if (!isHttpsRequest()) {
        http_response_code(426);
        die(json_encode(['error' => 'Alarm-PIN wird nur ueber HTTPS akzeptiert.']));
    }
    global $alarmPinBruteForceConfig;
    $candidate = is_string($pin) && preg_match('/^\d{4,12}$/', $pin) ? $pin : '';
    $stored_bcrypt = $settings['alarm_pin'] ?? '';
    if (!is_string($stored_bcrypt) || empty($stored_bcrypt) ||
        (password_get_info($stored_bcrypt)['algoName'] ?? 'unknown') !== 'bcrypt') {
        http_response_code(503);
        die(json_encode(['error' => 'Alarm-PIN ist nicht sicher provisioniert.']));
    }
    $remote = $_SERVER['REMOTE_ADDR'] ?? 'unknown';
    $ip = is_string($remote) && filter_var($remote, FILTER_VALIDATE_IP) ? $remote : 'unknown';
    $result = authenticateWithBruteForce('alarm-pin:' . $ip, $candidate, $stored_bcrypt, $alarmPinBruteForceConfig);
    if (!empty($result['storage_error'])) {
        http_response_code(503);
        die(json_encode(['error' => 'PIN-Schutzspeicher ist nicht verfuegbar.']));
    }
    if (!empty($result['blocked'])) {
        $retryAfter = max(60, (int)($result['lockout_remaining'] ?? 60));
        header('Retry-After: ' . $retryAfter);
        http_response_code(429);
        die(json_encode(['error' => 'Zu viele falsche PIN-Versuche.']));
    }
    return !empty($result['authenticated']);
}

/** Bindet eine kurze Audit-Freigabe an den aktuell gespeicherten PIN-Hash. */
function auditPinFingerprint($settings) {
    $storedHash = is_array($settings) ? ($settings['alarm_pin'] ?? '') : '';
    if (!is_string($storedHash) || $storedHash === '') return '';
    return hash('sha256', "iot-alarm-audit-grant-v1\0" . $storedHash);
}

function grantAuditPinAccess($settings) {
    $fingerprint = auditPinFingerprint($settings);
    if ($fingerprint === '') throw new RuntimeException('Alarm PIN is not provisioned');
    $_SESSION['audit_pin_grant'] = [
        'expires_at'  => time() + AUDIT_PIN_GRANT_SECONDS,
        'fingerprint' => $fingerprint
    ];
}

/** Auditdaten bleiben auch fuer eine bereits angemeldete Sitzung PIN-geschuetzt. */
function requireAuditPinGrant($settings) {
    $grant = $_SESSION['audit_pin_grant'] ?? null;
    $expected = auditPinFingerprint($settings);
    $valid = is_array($grant)
        && isset($grant['expires_at'], $grant['fingerprint'])
        && is_int($grant['expires_at'])
        && $grant['expires_at'] >= time()
        && is_string($grant['fingerprint'])
        && $expected !== ''
        && hash_equals($expected, $grant['fingerprint']);
    if ($valid) return;

    unset($_SESSION['audit_pin_grant']);
    http_response_code(403);
    die(json_encode(['error' => 'Audit-PIN-Freigabe erforderlich.']));
}

/** Neutralisiert Tabellenformeln, ohne den eigentlichen Auditwert zu verlieren. */
function csvSafeCell($value) {
    $text = is_scalar($value) || $value === null ? (string)$value : '[invalid]';
    return preg_match('/^[\x00-\x20]*[=+\-@]/', $text) ? "'" . $text : $text;
}

function buildUserAuditEntry($action, $details = "") {
    $ip    = $_SERVER['REMOTE_ADDR'];
    $agent = $_SERVER['HTTP_USER_AGENT'] ?? 'Unknown';

    // User-Agent parsen: Gerät, OS und Browser aus dem UA-String extrahieren.
    // parseUserAgent() ist in includes/brute_force.php definiert (shared).
    $uaInfo = parseUserAgent($agent);

    return [
        'timestamp'   => time(),
        'date'        => date("d.m.Y H:i:s"),
        'ip'          => $ip,
        // Gerätename: "Samsung SM-S918B (Android 14)" statt nur IP
        'device_name' => $uaInfo['device'] . ' (' . $uaInfo['os'] . ')',
        'browser'     => $uaInfo['browser'],
        'user_agent'  => utf8SafeByteLimit($agent, 256),
        'action'      => utf8SafeByteLimit($action, 50),
        'details'     => utf8SafeByteLimit($details, 256)
    ];
}

function logUserAction($action, $details = "") {
    $newLog = buildUserAuditEntry($action, $details);
    // Ein gemeinsamer Exclusive-Lock serialisiert alle Audit-Log-Autoren.
    if (!appendUserLogLocked($newLog))
        error_log('AUDIT_LOG_WRITE_FAILED action=' . utf8SafeByteLimit($action, 50));
}

// Deterministische Protokolltests laden nur die oben definierten Helfer.
// Die Konstante ist nur aus PHP-Code setzbar und kann nicht per HTTP aktiviert werden.
if (defined('IOT_ALARM_PROTOCOL_LIBRARY_ONLY') && IOT_ALARM_PROTOCOL_LIBRARY_ONLY === true) return;

// ============================================================
// SESSION
// ============================================================
ini_set('session.cookie_httponly', 1);
ini_set('session.cookie_samesite', 'Strict');
ini_set('session.use_strict_mode', 1);
if (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off') {
    ini_set('session.cookie_secure', 1);
}
// ESP-Heartbeats authentisieren sich ausschliesslich per source-gebundenem
// Bearer/HMAC. Ohne diese Trennung wuerde jeder Cookie-lose 2-s-Heartbeat eine
// nutzlose PHP-Session auf der SD-Karte erzeugen.
$isDeviceRequest = ($_SERVER['REQUEST_METHOD'] ?? '') === 'POST' && !isset($_GET['action']);
if (!$isDeviceRequest) session_start();

// ============================================================
// A. DASHBOARD AKTIONEN
// ============================================================

$_rawPostBody = '';
if ($_SERVER['REQUEST_METHOD'] === 'POST' && empty($_POST) && isset($_GET['action'])) {
    $_rawPostBody = file_get_contents('php://input');
    if (!empty($_rawPostBody)) {
        parse_str($_rawPostBody, $_POST);
    }
}

if (isset($_GET['action'])) {
    $action = $_GET['action'];

    if ($action === 'ping_activity') {
        if (requireAuth(false)) {
            $_SESSION['last_activity'] = time();
            echo json_encode(['status' => 'ok']);
        }
        exit;
    }
    if ($action === 'get_csrf_token') {
        if (requireAuth(false)) echo json_encode(['csrf_token' => getCSRFToken()]);
        exit;
    }

    requireAuth();
    checkRateLimit(60, 60);

    if ($action === 'get_user_logs') {
        requireAuditPinGrant(readJsonLocked($confFile));
        header('Content-Type: application/json');
        echo json_encode(loadUserLogs(), JSON_UNESCAPED_SLASHES);
        exit;
    }
    if ($action === 'export_user_logs') {
        requireAuditPinGrant(readJsonLocked($confFile));
        $logs = loadUserLogs();
        header('Content-Type: text/csv; charset=utf-8');
        header('Content-Disposition: attachment; filename="user_logs_' . date('Y-m-d_H-i-s') . '.csv"');
        $out = fopen('php://output', 'w');
        fputcsv($out, ['Zeitstempel','Datum','IP','Geraet','Browser','Aktion','Details','User Agent']);
        foreach ($logs as $l) fputcsv($out, array_map('csvSafeCell', [
            $l['timestamp'] ?? '', $l['date'] ?? '', $l['ip'] ?? '',
            $l['device_name'] ?? 'Unbekannt',
            $l['browser'] ?? 'Unbekannt',
            $l['action'] ?? '', $l['details'] ?? '',
            $l['user_agent'] ?? ''
        ]));
        fclose($out);
        exit;
    }

    if ($action === 'get_esp_token') {
        $s = json_decode(file_get_contents($confFile), true);
        echo json_encode(['configured' => [
            'sender' => !empty($s['sender_api_token']),
            'receiver' => !empty($s['receiver_api_token']),
            'camera' => !empty($s['camera_api_token'])
        ]]);
        exit;
    }

    if ($action === 'export_telemetry') {
        $csvFile = $dataDir . 'telemetry.csv';
        if (!file_exists($csvFile)) {
            http_response_code(404);
            die('Keine Telemetrie-Daten vorhanden.');
        }
        header('Content-Type: text/csv; charset=utf-8');
        header('Content-Disposition: attachment; filename="telemetry_' . date('Y-m-d_H-i-s') . '.csv"');
        echo "timestamp,source,rssi,heap\n";
        readfile($csvFile);
        exit;
    }

    if ($action === 'get_recordings') {
        $recDir = $dataDir . 'recordings';
        $files = [];
        if (is_dir($recDir)) {
            // alarm_ = automatische Alarm-Aufnahmen, manual_ = manuell gestartet
            $patterns = [
                $recDir . '/alarm_*.{avi,mp4,mkv}',
                $recDir . '/manual_*.{avi,mp4,mkv}'
            ];
            foreach ($patterns as $pattern) {
                foreach (glob($pattern, GLOB_BRACE) as $f) {
                    if (!is_file($f) || is_link($f)) continue;
                    $files[] = [
                        'name'      => basename($f),
                        'size'      => filesize($f),
                        'size_mb'   => round(filesize($f) / 1048576, 1),
                        'date'      => date('d.m.Y H:i:s', filemtime($f)),
                        'timestamp' => filemtime($f)
                    ];
                }
            }
        }
        usort($files, function($a, $b) { return $b['timestamp'] - $a['timestamp']; });
        echo json_encode(['recordings' => $files]);
        exit;
    }

    if ($action === 'download_recording' && isset($_GET['file'])) {
        $filename = $_GET['file'];
        if (!is_string($filename) || !preg_match(ALARM_RECORDING_FILENAME_PATTERN, $filename)) {
            http_response_code(404);
            die('Datei nicht gefunden');
        }
        $filepath = $dataDir . 'recordings/' . $filename;
        // Erlaubt: alarm_YYYYMMDD_HHMMSS.ext (Alarm-Aufnahmen) UND manual_YYYYMMDD_HHMMSS.ext (manuell)
        if (!is_file($filepath) || is_link($filepath)) {
            http_response_code(404);
            die('Datei nicht gefunden');
        }
        header('Content-Type: application/octet-stream');
        header('Content-Disposition: attachment; filename="' . $filename . '"');
        header('Content-Length: ' . filesize($filepath));
        readfile($filepath);
        exit;
    }

    // === ALARM STATUS (Locked Read) ===
    if ($action === 'get_alarm_status') {
        $statusFile2 = $dataDir . 'alarm_monitor.json';

        if (file_exists($statusFile2)) {
            $data = readJsonLocked($statusFile2); // 🔒 Locked Read
            echo json_encode($data);
        } else {
            echo json_encode(['state' => 'not_running', 'timestamp' => null]);
        }
        exit;
    }

    if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
        http_response_code(405);
        die(json_encode(['error' => 'POST required']));
    }
    validateCSRF();

    if ($action === 'delete_recording' && isset($_POST['file'])) {
        $filename = $_POST['file'];
        // Erlaubt: alarm_YYYYMMDD_HHMMSS.ext (Alarm-Aufnahmen) UND manual_YYYYMMDD_HHMMSS.ext (manuell)
        if (!is_string($filename) || !preg_match(ALARM_RECORDING_FILENAME_PATTERN, $filename)) {
            http_response_code(400);
            die(json_encode(['error' => 'Ungueltiger Dateiname']));
        }
        try {
            $result = alarmIpcRequest('delete_recording', $filename);
        } catch (RuntimeException $e) {
            http_response_code(503);
            die(json_encode(['error' => 'Lokaler Steuerdienst nicht verfuegbar.']));
        }
        $deleted = max(0, (int)($result['deleted'] ?? 0));
        logUserAction("Recordings", "Aufnahme ueber Steuerdienst geloescht: $filename");
        echo json_encode([
            'status' => 'ok',
            'message' => "Aufnahme geloescht: $filename",
            'deleted' => $deleted
        ]);
        exit;
    }

    if ($action === 'delete_all_recordings') {
        try {
            $result = alarmIpcRequest('delete_all_recordings');
        } catch (RuntimeException $e) {
            http_response_code(503);
            die(json_encode(['error' => 'Lokaler Steuerdienst nicht verfuegbar.']));
        }
        $count = max(0, (int)($result['deleted'] ?? 0));
        logUserAction("Recordings", "$count inaktive Aufnahmen ueber Steuerdienst geloescht");
        echo json_encode([
            'status' => 'ok',
            'message' => "$count inaktive Aufnahmen geloescht",
            'deleted' => $count
        ]);
        exit;
    }

    // === MANUELLE AUFNAHME: STARTEN ===
    // PIN-geschuetzt; der Alarm-Monitor besitzt exklusiv den Recorder.
    if ($action === 'start_manual_recording') {
        $pin = $_POST['alarm_pin'] ?? '';
        $settings = json_decode(file_get_contents($confFile), true) ?? [];
        if (!verifyPin($pin, $settings)) {
            http_response_code(401);
            logUserAction("ManualRecord", "PIN falsch - Start verweigert");
            die(json_encode(['error' => 'Falscher PIN!']));
        }

        try {
            $result = alarmIpcRequest('manual_record_start');
        } catch (RuntimeException $e) {
            http_response_code(503);
            logUserAction("ManualRecord", "Lokaler Steuerdienst nicht verfuegbar");
            die(json_encode(['error' => 'Lokaler Steuerdienst nicht verfuegbar.']));
        }

        $recording = ($result['recording'] ?? false) === true;
        logUserAction("ManualRecord", "Aufnahmeanforderung an Steuerdienst gesendet");
        echo json_encode([
            'status' => 'ok',
            'message' => $recording
                ? 'Manuelle Aufnahme gestartet.'
                : 'Aufnahmeanforderung angenommen.',
            'recording' => $recording
        ]);
        exit;
    }

    // === MANUELLE AUFNAHME: STOPPEN ===
    if ($action === 'stop_manual_recording') {
        $pin = $_POST['alarm_pin'] ?? '';
        $settings = json_decode(file_get_contents($confFile), true) ?? [];
        if (!verifyPin($pin, $settings)) {
            http_response_code(401);
            logUserAction("ManualRecord", "PIN falsch - Stop verweigert");
            die(json_encode(['error' => 'Falscher PIN!']));
        }

        try {
            $result = alarmIpcRequest('manual_record_stop');
        } catch (RuntimeException $e) {
            http_response_code(503);
            logUserAction("ManualRecord", "Lokaler Steuerdienst nicht verfuegbar");
            die(json_encode(['error' => 'Lokaler Steuerdienst nicht verfuegbar.']));
        }

        $recording = ($result['recording'] ?? false) === true;
        logUserAction("ManualRecord", "Stoppanforderung an Steuerdienst gesendet");
        echo json_encode([
            'status' => 'ok',
            'message' => $recording
                ? 'Manuelle Anforderung beendet; Alarmaufnahme bleibt aktiv.'
                : 'Manuelle Aufnahme gestoppt.',
            'recording' => $recording
        ]);
        exit;
    }

    // === PIN-VERIFIKATION (für Admin-Bereich, kein Folgebefehl) ===
    if ($action === 'verify_pin') {
        $pin        = $_POST['alarm_pin']  ?? '';
        $s          = json_decode(file_get_contents($confFile), true) ?? [];

        if (!verifyPin($pin, $s)) {
            http_response_code(401);
            logUserAction("Admin", "PIN falsch - Admin-Zugriff verweigert");
            die(json_encode(['error' => 'Falscher PIN!']));
        }
        grantAuditPinAccess($s);
        logUserAction("Admin", "Admin-Bereich geoeffnet");
        echo json_encode(['status' => 'ok']);
        exit;
    }

    if ($action === 'clear_logs' && isset($_POST['target'])) {
        $target = sanitizeNodeName($_POST['target']);
        if ($target === 'camera') {
            try {
                alarmIpcRequest('clear_runtime_log');
            } catch (RuntimeException $e) {
                http_response_code(503);
                die(json_encode(['error' => 'Runtime-Log konnte nicht sicher geloescht werden.']));
            }
        }
        updateTextLockedStrict($logFile, function($current) use ($target) {
            $lines = preg_split('/(?<=\n)/', $current, -1, PREG_SPLIT_NO_EMPTY);
            $filtered = array_filter($lines ?: [], function($line) use ($target) {
                return strpos($line, $target . ':') === false;
            });
            return implode('', $filtered);
        });
        logUserAction("Clear Logs", "Logs fuer '$target' geloescht");
        echo json_encode(['status' => 'ok', 'message' => "Logs geloescht"]);
        exit;
    }

    if ($action === 'clear_user_logs') {
        $pin = $_POST['alarm_pin'] ?? '';
        $currentSettings = readJsonLockedStrict($confFile);
        if (!verifyPin($pin, $currentSettings)) {
            http_response_code(401);
            logUserAction("Clear User Logs", "PIN falsch - Zugriff verweigert");
            die(json_encode(['error' => 'Falscher PIN!']));
        }
        $clearEntry = buildUserAuditEntry("Clear User Logs", "Geloescht");
        if (!replaceUserLogsWithAuditLocked($clearEntry)) {
            http_response_code(500);
            echo json_encode(['error' => 'User-Logs konnten nicht geloescht werden.']);
            exit;
        }
        echo json_encode(['status' => 'ok', 'message' => 'User-Logs geloescht.']);
        exit;
    }

    if ($action === 'clear_telemetry') {
        $csvFile = $dataDir . 'telemetry.csv';
        if (file_exists($csvFile)) @unlink($csvFile);
        if (file_exists($csvFile . '.old')) @unlink($csvFile . '.old');
        logUserAction("Clear Telemetry", "Telemetrie-Daten geloescht");
        echo json_encode(['status' => 'ok', 'message' => 'Telemetrie geloescht.']);
        exit;
    }

    if ($action === 'clear_all_logs') {
        try {
            alarmIpcRequest('clear_runtime_log');
            updateTextLockedStrict($logFile, fn($current) => '');
        } catch (RuntimeException $e) {
            http_response_code(503);
            die(json_encode(['error' => 'System-Logs konnten nicht sicher geloescht werden.']));
        }
        logUserAction("Clear All Logs", "System-Logs geloescht");
        echo json_encode(['status' => 'ok', 'message' => 'System-Logs geloescht.']);
        exit;
    }

    if ($action === 'save_settings' && isset($_POST['settings'])) {
        $new = json_decode($_POST['settings'], true);
        if (!$new) { http_response_code(400); die(json_encode(['error' => 'Invalid JSON'])); }
        foreach (['password','current_password','alarm_pin','current_alarm_pin','admin_title_pin','site_title'] as $stringField) {
            if (isset($new[$stringField]) && !is_string($new[$stringField])) {
                http_response_code(400);
                die(json_encode(['error' => 'Invalid settings field type']));
            }
        }
        $settingsLock = fopen(dirname($confFile) . DIRECTORY_SEPARATOR . 'settings.bootstrap.lock', 'c+');
        if (!$settingsLock || !flock($settingsLock, LOCK_EX)) {
            if ($settingsLock) fclose($settingsLock);
            http_response_code(503);
            die(json_encode(['error' => 'Settings lock unavailable']));
        }
        $cur = readJsonLockedStrict($confFile);

        // Sicherheitscheck: Titel 'admin' aktiviert den Admin-Bereich →
        // PIN-Verifikation nötig (verhindert unbeabschichtigtes oder unbefugtes Setzen).
        if (strtolower(trim($new['site_title'] ?? '')) === 'admin') {
            if (!empty($cur['bootstrap_pin_pending'])) {
                http_response_code(409);
                die(json_encode(['error' => 'Bitte zuerst einen persönlichen Alarm-PIN einrichten.']));
            }
            // Der Client uebertraegt den PIN nur innerhalb der TLS-Sitzung.
            $pin = $new['admin_title_pin'] ?? '';
            if (!verifyPin($pin, $cur)) {
                http_response_code(401);
                logUserAction("Config", "PIN falsch - Admin-Titel verweigert");
                die(json_encode(['error' => 'Der Titel "admin" erfordert PIN-Bestätigung. Falscher PIN!']));
            }
        }

        // Passwort-Änderung: aktuelles Passwort muss korrekt sein.
        // password_verify() vergleicht Klartext gegen gespeicherten bcrypt-Hash.
        if (!empty($new['password'])) {
            if (strlen($new['password']) < 12 || strlen($new['password']) > 72) {
                http_response_code(400);
                die(json_encode(['error' => 'Das neue Admin-Passwort muss 12 bis 72 Zeichen lang sein.']));
            }
            $current_pw = $new['current_password'] ?? '';
            if (empty($current_pw) || !password_verify($current_pw, $cur['password'])) {
                http_response_code(401);
                logUserAction("Config", "Altes Passwort falsch - Passwortaenderung verweigert");
                die(json_encode(['error' => 'Falsches aktuelles Passwort. Passwort nicht geändert.']));
            }
        }

        // Alarm-PIN-Aenderung: aktueller PIN muss korrekt sein.
        if (!empty($new['alarm_pin'])) {
            if (!preg_match('/^\d{4,12}$/', $new['alarm_pin'])) {
                http_response_code(400);
                die(json_encode(['error' => 'Der Alarm-PIN muss aus 4 bis 12 Ziffern bestehen.']));
            }
            $current_pin = $new['current_alarm_pin'] ?? '';
            // Beim einmaligen Bootstrap genügt die bereits authentifizierte,
            // CSRF-geschützte Admin-Session. Danach ist immer der alte PIN nötig.
            if (empty($cur['bootstrap_pin_pending']) && !verifyPin($current_pin, $cur)) {
                http_response_code(401);
                logUserAction("Config", "Alter PIN falsch - PIN-Aenderung verweigert");
                die(json_encode(['error' => 'Falscher aktueller Alarm-PIN. PIN nicht geändert.']));
            }
        }

        if (!empty($new['password'])) {
            $new['password'] = password_hash($new['password'], PASSWORD_BCRYPT);
            $new['bootstrap_admin_pending'] = false;
        }
        else {
            $new['password'] = $cur['password'];
            $new['bootstrap_admin_pending'] = (bool)($cur['bootstrap_admin_pending'] ?? false);
        }
        if (!empty($new['alarm_pin'])) {
            $plain_pin = $new['alarm_pin'];
            $new['alarm_pin'] = password_hash($plain_pin, PASSWORD_BCRYPT);
            $new['bootstrap_pin_pending'] = false;
        } else {
            $new['alarm_pin'] = $cur['alarm_pin'];
            $new['bootstrap_pin_pending'] = (bool)($cur['bootstrap_pin_pending'] ?? false);
        }
        // Secrets sind serververwaltet: Clientwerte werden vollständig ignoriert,
        // aber beim Speichern zuverlässig beibehalten.
        $deviceSecretKeys = [
            'sender_api_token','sender_hmac_secret',
            'receiver_api_token','receiver_hmac_secret',
            'camera_api_token','camera_hmac_secret','udp_hmac_secret'
        ];
        foreach ($deviceSecretKeys as $secretKey) $new[$secretKey] = $cur[$secretKey];

        if (!isset($new['timeout_active'])) $new['timeout_active'] = false;
        $new['timeout_minutes'] = min(max((int)($new['timeout_minutes'] ?? 5), 1), 120);
        $new['refresh_rate'] = max(1000, min((int)($new['refresh_rate'] ?? 2000), 30000));
        $new['camera_port'] = (int)($new['camera_port'] ?? $cur['camera_port'] ?? 8082);
        // htmlspecialchars() NUR beim Ausgeben (dashboard.php/index.php), nicht beim Speichern.
        // Doppeltes Escaping würde z.B. "Müllers IoT" als "M&uuml;llers IoT" speichern.
        $new['site_title'] = utf8SafeByteLimit($new['site_title'] ?? 'IoT-AlarmSystem', 50);

        $allowed = [
            'password','refresh_rate','site_title','timeout_active','timeout_minutes',
            'sender_api_token','sender_hmac_secret',
            'receiver_api_token','receiver_hmac_secret',
            'camera_api_token','camera_hmac_secret','udp_hmac_secret',
            'camera_port','alarm_pin',
            'bootstrap_admin_pending','bootstrap_pin_pending'
        ];
        $final = [];
        foreach ($allowed as $k) $final[$k] = $new[$k] ?? $cur[$k] ?? null;

        writeSettingsAtomic($confFile, $final);
        flock($settingsLock, LOCK_UN);
        fclose($settingsLock);
        logUserAction("Config", "Einstellungen geaendert");
        echo json_encode(['status' => 'ok', 'message' => 'Gespeichert.']);
        exit;
    }


    if ($action === 'system_reset') {
        // PIN-Pflicht: System Reset ist destruktiv (löscht alle Daten unwiderruflich)
        $pin        = $_POST['alarm_pin']  ?? '';
        $s          = json_decode(file_get_contents($confFile), true) ?? [];

        if (!verifyPin($pin, $s)) {
            http_response_code(401);
            logUserAction("SYSTEM RESET", "PIN falsch - Zugriff verweigert");
            die(json_encode(['error' => 'Falscher PIN!']));
        }

        try {
            // Der Daemon-IPC ist die einzige fallible externe Vorbedingung.
            // Schlaegt sie fehl, bleiben Queue und Dashboardstatus unangetastet.
            alarmIpcRequest('clear_runtime_log');
            // Der monotone delivery_state.json-Counter bleibt absichtlich bestehen,
            // damit alte Envelopes nach einem Reset nie erneut gelten.
            resetDeliveryQueues();
            updateJsonLockedStrict($statusFile, fn($current) => []);
            updateTextLockedStrict($logFile, fn($current) => '');
        } catch (RuntimeException $e) {
            http_response_code(503);
            die(json_encode(['error' => 'Systemzustand konnte nicht sicher zurueckgesetzt werden.']));
        }
        logUserAction("SYSTEM RESET", "Alle Daten geloescht");
        echo json_encode(['status' => 'ok', 'message' => 'System Reset.']);
        exit;
    }

    if ($action === 'send_command' && isset($_POST['target']) && isset($_POST['cmd'])) {
        $target = sanitizeNodeName($_POST['target']);
        $cmd    = sanitizeCommand($_POST['cmd']);
        if (!in_array($cmd, allowedCommandsForTarget($target), true)) {
            http_response_code(400);
            die(json_encode(['error' => 'Command not supported by target']));
        }

        // Jeder zustandsaendernde Receiver-Befehl und REBOOT brauchen den
        // Alarm-PIN im selben Request; eine Sitzung allein reicht nicht.
        if ($cmd === 'REBOOT' || ($target === 'receiver' && in_array($cmd, ['ALARM_ON', 'ALARM_OFF'], true))) {
            $pin        = $_POST['alarm_pin']  ?? '';
            $s          = json_decode(file_get_contents($confFile), true) ?? [];

            if (!verifyPin($pin, $s)) {
                http_response_code(401);
                logUserAction("Command", "PIN falsch fuer $cmd auf $target - Zugriff verweigert");
                die(json_encode(['error' => 'Falscher PIN!']));
            }
        }

        try {
            queueDelivery($target, 'command', $cmd);
        } catch (OverflowException $e) {
            http_response_code(409);
            die(json_encode(['error' => 'Geraete-Warteschlange ist voll; erst ausstehende Befehle zustellen.']));
        } catch (RuntimeException $e) {
            http_response_code(503);
            die(json_encode(['error' => 'Geraete-Warteschlange ist momentan nicht sicher verfuegbar.']));
        }

        $status = readJsonLocked($statusFile);
        $isOnline = isset($status[$target]['last_seen']) && (time() - $status[$target]['last_seen'] < 60);

        logUserAction("Command", "'$cmd' -> '$target'" . ($isOnline ? "" : " (offline)"));
        $msg = $isOnline
            ? "'$cmd' an '$target' gesendet."
            : "'$cmd' fuer '$target' hinterlegt (offline - wird beim naechsten Heartbeat ausgefuehrt).";
        echo json_encode(['status' => 'ok', 'message' => $msg]);
        exit;
    }

    if ($action === 'save_node_config' && isset($_POST['target'])) {
        $target = sanitizeNodeName($_POST['target']);
        if (!in_array($target, ['sender', 'receiver'], true)) {
            http_response_code(400);
            die(json_encode(['error' => 'Remote-Konfiguration wird nur von Sender/Receiver unterstuetzt.']));
        }
        $config = json_decode($_POST['config'] ?? '{}', true);
        if (!$config) { http_response_code(400); die(json_encode(['error' => 'Invalid config'])); }
        unset($config['apiip']); // Defense in Depth: apiip serverseitig entfernen
        // Nur bekannte ESP-Felder zulassen; keine beliebigen Schlüssel in die
        // signierte Remote-Konfiguration übernehmen.
        $allowedConfigKeys = ['mssid','mpass','bssid','bpass','tpass'];
        $config = array_intersect_key($config, array_flip($allowedConfigKeys));
        if (!$config) { http_response_code(400); die(json_encode(['error' => 'Keine gueltigen Config-Felder'])); }
        $maxLengths = ['mssid'=>32, 'mpass'=>64, 'bssid'=>32, 'bpass'=>64, 'tpass'=>20];
        foreach ($config as $key => $value) {
            if (!is_string($value) || strlen($value) > $maxLengths[$key]) {
                http_response_code(400);
                die(json_encode(['error' => 'Ungueltiger Config-Wert fuer ' . $key]));
            }
        }
        try {
            queueDelivery($target, 'config', $config);
        } catch (OverflowException $e) {
            http_response_code(409);
            die(json_encode(['error' => 'Geraete-Warteschlange ist voll; erst ausstehende Befehle zustellen.']));
        } catch (RuntimeException $e) {
            http_response_code(503);
            die(json_encode(['error' => 'Geraete-Warteschlange ist momentan nicht sicher verfuegbar.']));
        }
        logUserAction("Node Update", "Config fuer '$target'");
        echo json_encode(['status' => 'ok', 'message' => "Config fuer '$target' hinterlegt."]);
        exit;
    }

    if ($action === 'regenerate_esp_token') {
        $settingsLockFile = dirname($confFile) . DIRECTORY_SEPARATOR . 'settings.bootstrap.lock';
        $settingsLock = fopen($settingsLockFile, 'c+');
        if (!$settingsLock || !flock($settingsLock, LOCK_EX)) {
            if ($settingsLock) fclose($settingsLock);
            http_response_code(503);
            die(json_encode(['error' => 'Settings lock unavailable']));
        }
        try {
            $s = json_decode(file_get_contents($confFile), true) ?? [];
            $newTokens = [
                'sender_api_token' => bin2hex(random_bytes(16)),
                'receiver_api_token' => bin2hex(random_bytes(16)),
                'camera_api_token' => bin2hex(random_bytes(16))
            ];
            writeBootstrapCredentials([
                'Sender API token' => $newTokens['sender_api_token'],
                'Receiver API token' => $newTokens['receiver_api_token'],
                'Camera API token' => $newTokens['camera_api_token']
            ]);
            $s = array_replace($s, $newTokens);
            writeSettingsAtomic($confFile, $s);
        } finally {
            flock($settingsLock, LOCK_UN);
            fclose($settingsLock);
        }
        logUserAction("Security", "ESP-Token regeneriert");
        echo json_encode(['status' => 'ok', 'message' => 'Neuer Token lokal in der Bootstrap-Datei abgelegt.']);
        exit;
    }

    if ($action === 'set_alarm_state' && isset($_POST['cmd'])) {
        $cmd = $_POST['cmd'];
        $allowedCommands = ['SCHARF', 'UNSCHARF', '1', '0'];
        if (!is_string($cmd) || !in_array($cmd, $allowedCommands, true)) {
            http_response_code(400);
            die(json_encode(['error' => 'Unerlaubter Befehl']));
        }

        $pin = $_POST['alarm_pin'] ?? '';
        $settings = json_decode(file_get_contents($confFile), true) ?? [];
        if (!verifyPin($pin, $settings)) {
            http_response_code(401);
            logUserAction("Alarm", "PIN falsch - Zugriff verweigert");
            die(json_encode(['error' => 'Falscher PIN!']));
        }

        $armed = $cmd === 'SCHARF' || $cmd === '1';
        $label = $armed ? 'SCHARF' : 'UNSCHARF';
        try {
            $result = alarmIpcRequest($armed ? 'arm' : 'disarm');
        } catch (RuntimeException $e) {
            http_response_code(503);
            logUserAction("Alarm", "$label ueber lokalen Steuerdienst fehlgeschlagen");
            die(json_encode(['error' => 'Lokaler Steuerdienst nicht verfuegbar.']));
        }

        if (($result['confirmed'] ?? false) !== true
            || ($result['armed'] ?? null) !== $armed) {
            http_response_code(503);
            logUserAction("Alarm", "$label ohne gueltige Statusbestaetigung");
            die(json_encode(['error' => 'Alarmstatus konnte nicht bestaetigt werden.']));
        }

        logUserAction("Alarm", "$label ueber lokalen Steuerdienst bestaetigt");
        $logEntry = date("[d.m.Y H:i:s]") . " camera: Alarm $label bestaetigt (Alarm-Monitor)\n";
        appendApiLogLine($logEntry);
        echo json_encode([
            'status' => 'ok',
            'message' => "Alarm $label bestaetigt.",
            'confirmed' => true,
            'armed' => $armed,
            'interface' => 'alarm-monitor'
        ]);
        exit;
    }

    http_response_code(400);
    die(json_encode(['error' => 'Unknown action']));
}

// ============================================================
// B. ESP KOMMUNIKATION
// ============================================================
if ($_SERVER['REQUEST_METHOD'] === 'POST' && !isset($_GET['action'])) {
    // Unauthentifizierter Müll darf das Kontingent legitimer Geräte nicht
    // verbrauchen. Erst Token prüfen, dann den authentifizierten Client zählen.
    $json = file_get_contents('php://input');
    if (strlen($json) > 4096) { http_response_code(413); die(json_encode(['error' => 'Too large'])); }

    $data = json_decode($json, true);
    if (is_array($data) && isset($data['source']) && is_string($data['source'])) {
        $allowedSources = ['sender', 'receiver', 'camera'];
        $source = strtolower(trim($data['source']));
        if (!in_array($source, $allowedSources, true)) { http_response_code(400); die(json_encode(['error' => 'Invalid source'])); }
        validateESPToken($source, $settings);
        checkRateLimit(60, 60);
        $hmac_secret = deviceCredential($settings, $source, 'hmac');

        if (isset($data['delivery_ack']) || isset($data['delivery_ack_sig'])) {
            $ackId  = is_string($data['delivery_ack'] ?? null) ? strtolower($data['delivery_ack']) : '';
            $ackSig = is_string($data['delivery_ack_sig'] ?? null) ? strtolower($data['delivery_ack_sig']) : '';
            if (!verifyDeliveryAck($source, $ackId, $ackSig, $hmac_secret)) {
                http_response_code(401);
                echo json_encode(['error' => 'Invalid delivery acknowledgement']);
                exit;
            }
            try {
                acknowledgeDelivery($source, $ackId);
            } catch (RuntimeException $e) {
                http_response_code(503);
                echo json_encode(['error' => 'Delivery acknowledgement cannot be committed safely']);
                exit;
            }
        }

        $hasDeviceLog = array_key_exists('log', $data);
        if ($hasDeviceLog) {
            if (!is_string($data['log'])) { http_response_code(400); die(json_encode(['error' => 'Invalid log field'])); }
            $logMsg = preg_replace('/[\x00-\x1F\x7F]/', '', utf8SafeByteLimit($data['log'], 256));
            $entry = date("[d.m.Y H:i:s]") . " $source: " . $logMsg . "\n";
            appendApiLogLine($entry);
        }

        if (array_key_exists('status_msg', $data)) {
            if (!is_string($data['status_msg']) ||
                (array_key_exists('ip', $data) && !is_string($data['ip'])) ||
                (array_key_exists('reset_reason', $data) && !is_string($data['reset_reason'])) ||
                (array_key_exists('alarm_state', $data) && !is_bool($data['alarm_state'])) ||
                (array_key_exists('rssi', $data) && !is_int($data['rssi'])) ||
                (array_key_exists('heap', $data) && !is_int($data['heap'])) ||
                (array_key_exists('uptime', $data) && !is_int($data['uptime']))) {
                http_response_code(400);
                die(json_encode(['error' => 'Invalid status field type']));
            }
            $statusText = preg_replace('/[\x00-\x1F\x7F]/', ' ', utf8SafeByteLimit($data['status_msg'], 128));
            $resetReason = preg_replace('/[\x00-\x1F\x7F]/', ' ', utf8SafeByteLimit($data['reset_reason'] ?? 'unknown', 32));
            $sd = [
                'last_seen' => time(),
                'ip'        => filter_var($data['ip'] ?? '0.0.0.0', FILTER_VALIDATE_IP) ?: '0.0.0.0',
                'status'    => $statusText,
                'alarm'     => ($source === "receiver" && isset($data['alarm_state'])) ? $data['alarm_state'] : false,
                'rssi'      => max(-200, min(100, $data['rssi'] ?? 0)),
                'heap'      => max(0, min(67108864, $data['heap'] ?? 0)),
                'reset_reason' => $resetReason,
                'uptime'    => max(0, min(4294967295, $data['uptime'] ?? 0))
            ];

            $autoLogLine = null;
            try {
                updateJsonLockedStrict($statusFile, function($current) use ($source, $sd, $hasDeviceLog, &$autoLogLine) {
                    $previous = is_array($current[$source] ?? null) ? $current[$source] : [];
                    $lastLogTime = is_int($previous['last_log_time'] ?? null) ? $previous['last_log_time'] : 0;
                    $statusChanged = ($previous['status'] ?? '') !== $sd['status'];
                    $merged = $sd;
                    if (!$hasDeviceLog && (time() - $lastLogTime >= 60 || $statusChanged)) {
                        $merged['last_log_time'] = time();
                        $autoMsg = $sd['status'] !== '' ? $sd['status'] : 'Heartbeat';
                        $autoLogLine = date("[d.m.Y H:i:s]") . " $source: $autoMsg\n";
                    } else {
                        $merged['last_log_time'] = $lastLogTime > 0 ? $lastLogTime : time();
                    }
                    $current[$source] = array_merge($previous, $merged);
                    return $current;
                });
            } catch (RuntimeException $e) {
                http_response_code(503);
                die(json_encode(['error' => 'Device status cannot be committed safely']));
            }
            if ($autoLogLine !== null) appendApiLogLine($autoLogLine);

            $csvFile = $dataDir . 'telemetry.csv';
            $telemetryLine = time().",$source,{$sd['rssi']},{$sd['heap']}\n";
            updateTextLockedStrict($csvFile, function($current) use ($telemetryLine) {
                if (strlen($current) + strlen($telemetryLine) > 1048576) {
                    $lines = preg_split('/(?<=\n)/', $current, -1, PREG_SPLIT_NO_EMPTY);
                    $current = implode('', array_slice($lines ?: [], -5000));
                }
                return $current . $telemetryLine;
            });
        }

        $response = ["logging_active" => true, "protocol" => 2];
        if (strlen($hmac_secret) < 32) {
            http_response_code(503);
            echo json_encode(['error' => 'Device delivery is not provisioned']);
            exit;
        }

        // Eine Nachricht pro Heartbeat hält den ESP-RAM klein. Sie wird erst
        // nach ACK ihrer Message-ID gelöscht; bei Paketverlust bleibt sie liegen.
        try {
            $pending = getPendingDelivery($source);
        } catch (RuntimeException $e) {
            http_response_code(503);
            echo json_encode(['error' => 'Delivery queue cannot be read safely']);
            exit;
        }
        if ($pending !== null) {
            $envelope = encodeDeliveryEnvelope($source, $pending, $hmac_secret);
            if ($envelope === null) {
                http_response_code(500);
                echo json_encode(['error' => 'Invalid queued delivery']);
                exit;
            }
            $response['delivery'] = $envelope;
        }

        header('Content-Type: application/json');
        echo json_encode($response);
    } else {
        http_response_code(400);
        echo json_encode(['error' => 'Invalid payload']);
    }
    exit;
}

// ============================================================
// C. GET ALL (Dashboard)
// ============================================================
if ($_SERVER['REQUEST_METHOD'] === 'GET' && isset($_GET['get']) && $_GET['get'] === 'all') {
    requireAuth();

    // === STATUS LOCKING (Locked Read) ===
    $status = readJsonLocked($statusFile); // 🔒 Locked Read
    // ====================================

    foreach ($status as $k => $v) {
        $status[$k]['online'] = (isset($v['last_seen']) && time() - $v['last_seen'] < 60);
    }

    $piUptime = 0;
    if (file_exists('/proc/uptime')) {
        $parts = explode(' ', file_get_contents('/proc/uptime'));
        $piUptime = (int)floatval($parts[0]);
    }

    $status['pi'] = [
        'last_seen'    => time(),
        'ip'           => $_SERVER['SERVER_ADDR'] ?? '127.0.0.1',
        'status'       => 'Running',
        'online'       => true,
        'rssi'         => 0,
        'heap'         => 0,
        'uptime'       => $piUptime,
        'reset_reason' => 'N/A',
        'cpu_temp'     => 0,
        'cpu_load'     => '0'
    ];

    if (file_exists('/sys/class/thermal/thermal_zone0/temp')) {
        $raw = trim(file_get_contents('/sys/class/thermal/thermal_zone0/temp'));
        $status['pi']['cpu_temp'] = round((int)$raw / 1000, 1);
    }

    if (file_exists('/proc/loadavg')) {
        $parts = explode(' ', trim(file_get_contents('/proc/loadavg')));
        $status['pi']['cpu_load'] = $parts[0] ?? '0';
    }

    $logs = array_merge(readTextTailLocked($logFile, 20), readTextTailLocked($runtimeLogFile, 20));
    usort($logs, function($left, $right) {
        $parse = function($line) {
            if (!preg_match('/^\[(\d{2}\.\d{2}\.\d{4} \d{2}:\d{2}:\d{2})\]/', $line, $match)) return 0;
            $date = DateTime::createFromFormat('!d.m.Y H:i:s', $match[1]);
            return $date ? $date->getTimestamp() : 0;
        };
        return $parse($left) <=> $parse($right);
    });
    $logs = array_slice($logs, -20);
    // Strikte Positivliste: Weder Hashes noch Geräte-/HMAC-/API-Tokens dürfen
    // im Browser landen. Auch neue Secrets sind dadurch standardmäßig privat.
    $settings = publicSettings(readJsonLocked($confFile));

    // === ALARM MONITOR LOCKING (Locked Read) ===
    $alarmMonFile = $dataDir . 'alarm_monitor.json';
    $alarmMon = file_exists($alarmMonFile)
        ? readJsonLocked($alarmMonFile) // 🔒 Locked Read
        : ['state' => 'not_running'];
    // ===========================================

    header('Content-Type: application/json');
    echo json_encode([
        "status"        => $status,
        "logs"          => $logs,
        "config"        => $settings,
        "alarm_monitor" => $alarmMon
    ], JSON_INVALID_UTF8_SUBSTITUTE);
    exit;
}
?>
