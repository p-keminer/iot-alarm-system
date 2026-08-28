<?php
declare(strict_types=1);

$testDataDir = sys_get_temp_dir() . DIRECTORY_SEPARATOR . 'iot-alarm-security-' . bin2hex(random_bytes(6));
if (!mkdir($testDataDir, 0700, true) && !is_dir($testDataDir)) {
    throw new RuntimeException('test temp directory unavailable');
}
putenv('IOT_ALARM_DATA_DIR=' . $testDataDir);
register_shutdown_function(function() use ($testDataDir): void {
    $tempRoot = realpath(sys_get_temp_dir());
    $resolved = realpath($testDataDir);
    if ($tempRoot === false || $resolved === false || strpos($resolved, $tempRoot . DIRECTORY_SEPARATOR) !== 0) return;
    $iterator = new RecursiveIteratorIterator(
        new RecursiveDirectoryIterator($resolved, FilesystemIterator::SKIP_DOTS),
        RecursiveIteratorIterator::CHILD_FIRST
    );
    foreach ($iterator as $entry) {
        if ($entry->isDir()) rmdir($entry->getPathname());
        else unlink($entry->getPathname());
    }
    rmdir($resolved);
});

define('IOT_ALARM_PROTOCOL_LIBRARY_ONLY', true);
require dirname(__DIR__, 2) . '/web/api.php';

function expectTrue($condition, string $message): void {
    if (!$condition) throw new RuntimeException($message);
}

// Persistente Texte bleiben auch genau an einem Mehrbyte-Grenzwert gueltiges
// UTF-8; Audit-CSV darf keine Tabellenformel aus Benutzereingaben aktivieren.
$utfBoundary = str_repeat('A', 255) . "\xE2\x82\xAC";
$utfLimited = utf8SafeByteLimit($utfBoundary, 256);
expectTrue(strlen($utfLimited) === 255 && preg_match('//u', $utfLimited) === 1,
           'UTF-8 byte limit split a codepoint');
expectTrue(csvSafeCell('=SUM(1,1)') === "'=SUM(1,1)" && csvSafeCell('normal') === 'normal',
           'CSV formula neutralization failed');

$secret = '0123456789abcdef0123456789abcdef01234567'; // gitleaks:allow - published deterministic test vector
$source = 'receiver';
$id = '00112233445566778899aabbccddeeff';
$nonce = 'ffeeddccbbaa99887766554433221100';
$plain = '{"mpass":"wifi-secret","tpass":"debug-secret"}';
$cipher = encryptDeliveryConfig($source, $id, $plain, $nonce, $secret);
expectTrue($cipher === 'dvlY5X9xztes/N6Xx+7aqM9GzrVlqy43HnHKNpnStIKjret60FI/gg/ePBn9MA==', 'config vector mismatch');
$outer = "ALARMv2\n{$source}\n{$id}\n1700000000\nconfig\n{$nonce}\n{$cipher}";
expectTrue(hash_hmac('sha256', $outer, $secret) === '009f2c36c58a653200855209a99dafaba0d9e92aa6d1de22e9bfc051b32de2f4', 'outer HMAC mismatch');
$ack = hash_hmac('sha256', "ALARMv2ACK\n{$source}\n{$id}", $secret);
expectTrue($ack === '0d827486211d85abb9a9df994485c8de4addb8c02189c34626945987b247e3e1', 'ACK vector mismatch');
expectTrue(verifyDeliveryAck($source, $id, $ack, $secret), 'valid ACK rejected');
expectTrue(!verifyDeliveryAck('sender', $id, $ack, $secret), 'ACK source domain separation failed');
expectTrue(!verifyDeliveryAck($source, str_repeat('f', 32), $ack, $secret), 'ACK id binding failed');
expectTrue(!verifyDeliveryAck($source, $id, $ack, ''), 'empty HMAC secret accepted');

// Provisioning is source-separated: neither Bearer nor delivery HMAC crosses nodes.
$senderToken = deviceCredential($settings, 'sender', 'token');
$receiverToken = deviceCredential($settings, 'receiver', 'token');
$senderHmac = deviceCredential($settings, 'sender', 'hmac');
$receiverHmac = deviceCredential($settings, 'receiver', 'hmac');
expectTrue(strlen($senderToken) === 32 && strlen($receiverToken) === 32 &&
           !hash_equals($senderToken, $receiverToken), 'per-device API tokens are not separated');
expectTrue(strlen($senderHmac) === 40 && strlen($receiverHmac) === 40 &&
           !hash_equals($senderHmac, $receiverHmac), 'per-device delivery HMACs are not separated');
$receiverAck = hash_hmac('sha256', "ALARMv2ACK\nreceiver\n{$id}", $receiverHmac);
expectTrue(verifyDeliveryAck('receiver', $id, $receiverAck, $receiverHmac), 'receiver ACK rejected');
expectTrue(!verifyDeliveryAck('receiver', $id, $receiverAck, $senderHmac), 'sender HMAC forged receiver ACK');
expectTrue(encryptDeliveryConfig('receiver', $id, $plain, $nonce, $receiverHmac) !==
           encryptDeliveryConfig('receiver', $id, $plain, $nonce, $senderHmac),
           'cross-device config encryption did not separate keys');

// Offline state-setting commands coalesce, one-shot commands stay FIFO.
$on = queueDelivery('receiver', 'command', 'ALARM_ON');
$off = queueDelivery('receiver', 'command', 'ALARM_OFF');
$reboot = queueDelivery('receiver', 'command', 'REBOOT');
$commands = readJsonLockedStrict($cmdFile);
expectTrue(count($commands['receiver']) === 2, 'state command was not coalesced');
expectTrue($commands['receiver'][0]['id'] === $off['id'], 'latest state command is not first');
expectTrue($commands['receiver'][1]['id'] === $reboot['id'], 'one-shot FIFO order changed');
expectTrue($commands['receiver'][0]['target'] === 'receiver', 'delivery target is not persisted');
expectTrue(getPendingDelivery('receiver')['id'] === $off['id'], 'oldest delivery not returned');
expectTrue(!acknowledgeDelivery('receiver', str_repeat('a', 32)), 'wrong ACK removed a record');
expectTrue(acknowledgeDelivery('receiver', $off['id']), 'exact ACK did not remove record');
expectTrue(getPendingDelivery('receiver')['id'] === $reboot['id'], 'FIFO did not advance after ACK');

// If an ACK response was lost with server power, the same record can reappear
// and the exact, source-bound ACK removes it again without a new effect.
$reappearing = getPendingDelivery('receiver');
expectTrue(acknowledgeDelivery('receiver', $reappearing['id']), 'first ACK failed');
writeJsonAtomicStrict($cmdFile, ['receiver' => [$reappearing]]);
$reAck = hash_hmac('sha256', "ALARMv2ACK\nreceiver\n{$reappearing['id']}", $receiverHmac);
expectTrue(verifyDeliveryAck('receiver', $reappearing['id'], $reAck, $receiverHmac) &&
           acknowledgeDelivery('receiver', $reappearing['id']), 'reappeared durable record was not re-ACKable');

$remoteResetRejected = false;
try { queueDelivery('receiver', 'command', 'RESET'); }
catch (InvalidArgumentException $e) { $remoteResetRejected = true; }
expectTrue($remoteResetRejected, 'remote factory reset is still deliverable');

// Partial pending config is fieldwise merged, not overwritten wholesale.
$config1 = queueDelivery('sender', 'config', ['bssid' => 'backup', 'bpass' => 'old-pass', 'mpass' => 'old']);
$config2 = queueDelivery('sender', 'config', ['mpass' => 'new']);
$configQueue = readJsonLockedStrict($dataDir . 'update_sender.json');
expectTrue(count($configQueue) === 1 && $configQueue[0]['id'] === $config2['id'], 'config was not coalesced');
expectTrue($configQueue[0]['payload'] === ['bssid' => 'backup', 'bpass' => 'old-pass', 'mpass' => 'new'],
           'partial config coalescing lost an earlier field');
$public = publicSettings([
    'refresh_rate' => 2000, 'alarm_pin' => 'x',
    'sender_api_token' => 'a', 'sender_hmac_secret' => 'b',
    'receiver_api_token' => 'c', 'receiver_hmac_secret' => 'd',
    'camera_api_token' => 'e', 'camera_hmac_secret' => 'f',
    'udp_hmac_secret' => 'g', 'esp_token' => 'legacy', 'hmac_secret' => 'legacy'
]);
expectTrue($public === ['refresh_rate' => 2000], 'secret setting leaked through public allowlist');
expectTrue(acknowledgeDelivery('sender', $config2['id']), 'coalesced config ACK failed');

// Complete schema and legacy migration are fail-closed and target-specific.
writeJsonAtomicStrict($cmdFile, ['sender' => [[
    'id' => str_repeat('1', 32), 'sequence' => 1, 'type' => 'command',
    'target' => 'receiver', 'payload' => 'REBOOT'
]]]);
$wrongTargetRejected = false;
try { getPendingDelivery('sender'); } catch (RuntimeException $e) { $wrongTargetRejected = true; }
expectTrue($wrongTargetRejected, 'cross-target queued record accepted');
writeJsonAtomicStrict($cmdFile, ['sender' => 'REBOOT']);
$legacy = getPendingDelivery('sender');
expectTrue(isDeliveryRecord($legacy, 'sender', 'command') && $legacy['payload'] === 'REBOOT',
           'allowlisted legacy command was not migrated safely');
acknowledgeDelivery('sender', $legacy['id']);
writeJsonAtomicStrict($cmdFile, ['sender' => 'ALARM_ON']);
$legacyRejected = false;
try { getPendingDelivery('sender'); } catch (RuntimeException $e) { $legacyRejected = true; }
expectTrue($legacyRejected, 'unsupported legacy command was migrated');
writeJsonAtomicStrict($cmdFile, []);

// System reset clears command/config queues under their lock but keeps replay counters.
queueDelivery('receiver', 'command', 'ALARM_ON');
queueDelivery('sender', 'config', ['mssid' => 'network']);
$deliveryStateBeforeReset = file_get_contents($deliveryStateFile);
resetDeliveryQueues();
expectTrue(readJsonLockedStrict($cmdFile) === [] && !file_exists($dataDir . 'update_sender.json') &&
           !file_exists($dataDir . 'update_receiver.json'), 'system reset left stale delivery state');
expectTrue(file_get_contents($deliveryStateFile) === $deliveryStateBeforeReset,
           'system reset rolled back monotone replay counters');

// Queue limit rejects instead of silently deleting a one-shot command.
$overflowed = false;
try {
    for ($i = 0; $i < DELIVERY_QUEUE_LIMIT + 1; $i++) queueDelivery('sender', 'command', 'REBOOT');
} catch (OverflowException $e) {
    $overflowed = true;
}
expectTrue($overflowed, 'bounded queue did not fail closed');

// Corrupt state must not be interpreted/overwritten as an empty queue.
$before = '{truncated';
file_put_contents($cmdFile, $before);
$failedClosed = false;
try {
    queueDelivery('receiver', 'command', 'REBOOT');
} catch (RuntimeException $e) {
    $failedClosed = true;
}
expectTrue($failedClosed && file_get_contents($cmdFile) === $before, 'corrupt queue was overwritten');

// Firmware transaction contract: pending is durable before effect, ACK only
// after complete; UDP ACK follows persisted alarm state and physical apply.
foreach (['sender', 'receiver'] as $node) {
    $firmware = file_get_contents(dirname(__DIR__, 2) . "/firmware/esp8266/{$node}/{$node}.ino");
    expectTrue(str_contains($firmware, 'char apiHmacToken[41]') &&
               str_contains($firmware, 'doc["token"] = config.udpToken') &&
               str_contains($firmware, 'doc["apitoken"] = config.apiToken') &&
               str_contains($firmware, 'doc["dhmac"] = config.apiHmacToken'),
               "{$node} provisioning fields are not deterministically separated");
    expectTrue(str_contains($firmware, 'beginApiApply') && str_contains($firmware, 'completeApiApply') &&
               str_contains($firmware, 'lastAppliedId') && str_contains($firmware, 'erneuereAckFuerDuplikat'),
               "{$node} durable apply/re-ACK journal missing");
    $apiStart = strpos($firmware, 'bool pruefeApiDelivery');
    $apiEnd = strpos($firmware, 'void berechneDeliveryAckSignatur', $apiStart);
    $apiSection = substr($firmware, $apiStart, $apiEnd - $apiStart + 1000);
    expectTrue(str_contains($apiSection, 'config.apiHmacToken') &&
               !str_contains($apiSection, 'config.udpToken'), "{$node} API crypto still uses UDP key");
    expectTrue(!str_contains($firmware, 'deliveryPayload == "RESET"'), "{$node} remote RESET handler survived");
}
$receiverFirmware = file_get_contents(dirname(__DIR__, 2) . '/firmware/esp8266/receiver/receiver.ino');
$udpApply = strpos($receiverFirmware, 'bool neuerAlarmzustand = istAlarmAn;');
$udpPersist = strpos($receiverFirmware, 'speichereReplayZustand(neuerAlarmzustand)', $udpApply);
$udpHardware = strpos($receiverFirmware, 'HAL::alarmHardwareSetzen(true)', $udpPersist);
$udpAck = strpos($receiverFirmware, 'sendeSigniertesAck(seqString)', $udpHardware);
expectTrue($udpApply !== false && $udpPersist > $udpApply && $udpHardware > $udpPersist && $udpAck > $udpHardware,
           'receiver UDP ACK can precede durable alarm effect');
expectTrue(str_contains($receiverFirmware, 'alarmAktiv = Security::persistierterAlarm') &&
           str_contains($receiverFirmware, 'replayStateCorrupt = true; persistierterAlarm = true'),
           'receiver boot alarm state is not fail-secure');

echo "security protocol tests: ok\n";
