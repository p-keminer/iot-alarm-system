<?php
declare(strict_types=1);

$dataDir = getenv('IOT_ALARM_DATA_DIR');
if (!is_string($dataDir) || $dataDir === '') throw new RuntimeException('isolated IOT_ALARM_DATA_DIR required');
require dirname(__DIR__, 2) . '/web/includes/config.php';

if (($argv[1] ?? '') !== 'validate') exit(0);

$loaded = json_decode((string)file_get_contents($confFile), true);
$credentialMap = [
    'Sender API token'       => 'sender_api_token',
    'Sender delivery HMAC'   => 'sender_hmac_secret',
    'Receiver API token'     => 'receiver_api_token',
    'Receiver delivery HMAC' => 'receiver_hmac_secret',
    'Camera API token'       => 'camera_api_token',
    'Camera delivery HMAC'   => 'camera_hmac_secret',
    'UDP HMAC secret'        => 'udp_hmac_secret'
];
if (!is_array($loaded) || isset($loaded['esp_token']) || isset($loaded['hmac_secret'])) {
    throw new RuntimeException('generated settings invalid or legacy shared credentials survived');
}
foreach ($credentialMap as $settingKey) {
    $length = strlen((string)($loaded[$settingKey] ?? ''));
    $expectedLength = str_ends_with($settingKey, '_api_token') ? 32 : 40;
    if ($length !== $expectedLength) throw new RuntimeException('generated device credential invalid');
}
$uniqueCredentials = array_map(static fn($key) => $loaded[$key], array_values($credentialMap));
if (count(array_unique($uniqueCredentials)) !== count($uniqueCredentials)) {
    throw new RuntimeException('device and UDP credentials are not separated');
}
$bootstrap = (string)file_get_contents(bootstrapCredentialFile());
preg_match('/^Admin password: (.+)$/m', $bootstrap, $adminMatch);
preg_match('/^Alarm PIN: (\d{6})$/m', $bootstrap, $pinMatch);
if (empty($adminMatch[1]) || empty($pinMatch[1]) ||
    !password_verify(trim($adminMatch[1]), $loaded['password']) ||
    !password_verify($pinMatch[1], $loaded['alarm_pin'])) {
    throw new RuntimeException('bootstrap/settings mismatch');
}
foreach ($credentialMap as $label => $settingKey) {
    if (!preg_match('/^' . preg_quote($label, '/') . ': ([a-f0-9]+)$/m', $bootstrap, $match) ||
        !hash_equals($loaded[$settingKey], $match[1])) {
        throw new RuntimeException('bootstrap-to-settings credential mapping mismatch');
    }
}
echo "bootstrap concurrency probe: ok\n";
