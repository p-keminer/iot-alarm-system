<?php
// ============================================================
// includes/brute_force.php - Brute-Force-Schutz Hilfsfunktionen
// ============================================================
// Enthält die kanonischen Funktionen für den progressiven
// Brute-Force-Schutz. Wird von auth.php und login.php eingebunden.
// Alle persistenten Pfade werden aus IOT_ALARM_DATA_DIR bzw. aus
// dem absoluten Standardpfad web/data abgeleitet.
// ============================================================

/** Liefert das zentrale Datenverzeichnis unabhängig vom Prozess-CWD. */
function iotAlarmDataDir() {
    $configured = getenv('IOT_ALARM_DATA_DIR');
    if (is_string($configured) && $configured !== '') {
        return rtrim($configured, '/\\');
    }
    return dirname(__DIR__) . DIRECTORY_SEPARATOR . 'data';
}

/** Baut einen absoluten Pfad innerhalb des zentralen Datenverzeichnisses. */
function iotAlarmDataPath($name) {
    return iotAlarmDataDir() . DIRECTORY_SEPARATOR . ltrim($name, '/\\');
}

/** Byte-Limit ohne abgeschnittene UTF-8-Codepoints oder ungueltige Persistenz. */
function utf8SafeByteLimit($value, $maxBytes) {
    $text = is_string($value) ? $value : '';
    $maxBytes = max(0, (int)$maxBytes);
    $slice = strlen($text) <= $maxBytes ? $text : substr($text, 0, $maxBytes);
    while ($slice !== '' && preg_match('//u', $slice) !== 1) {
        $slice = substr($slice, 0, -1);
    }
    return $slice;
}

// ============================================================
// HILFSFUNKTION: User-Logs laden
// ============================================================
function loadUserLogs() {
    return bfReadJsonLocked(iotAlarmDataPath('user_logs.json'));
}

/**
 * Fuegt genau einen Audit-Eintrag unter einer exklusiven Transaktion ein.
 * Lesen, Begrenzen und Schreiben teilen denselben Lock, damit parallele
 * Login-/Logout-Requests keine bereits geschriebenen Eintraege verlieren.
 */
function appendUserLogLocked($entry, $limit = 100) {
    if (!is_array($entry) || $limit < 1) return false;
    $file = iotAlarmDataPath('user_logs.json');
    if (!bfEnsureParentDirectory($file)) return false;
    $fp = fopen($file, 'c+b');
    if (!$fp || !flock($fp, LOCK_EX)) {
        if ($fp) fclose($fp);
        return false;
    }

    try {
        $valid = true;
        $logs = bfReadJsonFromHandle($fp, $valid);
        if (!$valid) return false;
        array_unshift($logs, $entry);
        $logs = array_slice($logs, 0, $limit);
        return bfWriteJsonToHandle($fp, $logs);
    } finally {
        flock($fp, LOCK_UN);
        fclose($fp);
    }
}

/** Ersetzt das Auditlog und behaelt den Loeschvorgang im selben Commit. */
function replaceUserLogsWithAuditLocked($entry) {
    if (!is_array($entry)) return false;
    $file = iotAlarmDataPath('user_logs.json');
    if (!bfEnsureParentDirectory($file)) return false;
    $fp = fopen($file, 'c+b');
    if (!$fp || !flock($fp, LOCK_EX)) {
        if ($fp) fclose($fp);
        return false;
    }
    try {
        return bfWriteJsonToHandle($fp, [$entry]);
    } finally {
        flock($fp, LOCK_UN);
        fclose($fp);
    }
}

// ============================================================
// BRUTE-FORCE HILFSFUNKTIONEN
// ============================================================

/**
 * Prüft den Brute-Force-Status für eine IP-Adresse.
 *
 * @param string $ip       - Die zu prüfende IP-Adresse
 * @param array  $bfConfig - Brute-Force-Konfiguration
 * @return array           - [blocked, remaining_attempts, lockout_remaining, total_attempts, tier]
 */
function checkBruteForce($ip, $bfConfig) {
    $attemptFile = $bfConfig['file'];
    if (!bfEnsureParentDirectory($attemptFile)) return bfStorageFailureStatus();

    $fp = fopen($attemptFile, 'c+b');
    if (!$fp || !flock($fp, LOCK_EX)) {
        if ($fp) fclose($fp);
        return bfStorageFailureStatus();
    }

    try {
        $valid = true;
        $attempts = bfReadJsonFromHandle($fp, $valid);
        if (!$valid) return bfStorageFailureStatus();
        $now = time();
        $changed = bfPruneAttempts($attempts, $ip, $now, $bfConfig);
        if ($changed && !bfWriteJsonToHandle($fp, $attempts)) return bfStorageFailureStatus();
        return bfStatusForEntries($attempts[$ip] ?? [], $now, $bfConfig);
    } finally {
        flock($fp, LOCK_UN);
        fclose($fp);
    }
}

/** Fail-closed status if the attempt ledger cannot be locked or persisted. */
function bfStorageFailureStatus() {
    return [
        'blocked'            => true,
        'remaining_attempts' => 0,
        'lockout_remaining'  => 0,
        'total_attempts'     => 0,
        'tier'               => 0,
        'storage_error'      => true
    ];
}

function bfAttemptTimestamp($entry) {
    if (is_array($entry) && isset($entry['time']) && is_numeric($entry['time'])) return (int)$entry['time'];
    if (is_numeric($entry)) return (int)$entry;
    return 0;
}

/** Entfernt abgelaufene/ungültige Einträge. Der Aufrufer hält den Lock. */
function bfPruneAttempts(&$attempts, $ip, $now, $bfConfig) {
    if (!isset($attempts[$ip]) || !is_array($attempts[$ip])) {
        if (isset($attempts[$ip])) {
            unset($attempts[$ip]);
            return true;
        }
        return false;
    }
    $before = count($attempts[$ip]);
    $window = max(1, (int)$bfConfig['attempt_window']);
    $attempts[$ip] = array_values(array_filter($attempts[$ip], function($entry) use ($now, $window) {
        $timestamp = bfAttemptTimestamp($entry);
        return $timestamp > 0 && ($now - $timestamp) < $window;
    }));
    if (empty($attempts[$ip])) unset($attempts[$ip]);
    return $before !== count($attempts[$ip] ?? []);
}

/** Berechnet den Status aus einem bereits unter Lock gelesenen Snapshot. */
function bfStatusForEntries($entries, $now, $bfConfig) {
    $entries = is_array($entries) ? $entries : [];
    $totalAttempts = count($entries);
    $tiers = $bfConfig['lockout_tiers'];
    if ($totalAttempts === 0) {
        return [
            'blocked' => false,
            'remaining_attempts' => $tiers[0]['attempts'],
            'lockout_remaining' => 0,
            'total_attempts' => 0,
            'tier' => 0,
            'storage_error' => false
        ];
    }

    $lastAttemptTime = 0;
    foreach ($entries as $entry) $lastAttemptTime = max($lastAttemptTime, bfAttemptTimestamp($entry));
    $currentTier = 0;
    for ($i = count($tiers) - 1; $i >= 0; $i--) {
        if ($totalAttempts >= $tiers[$i]['attempts']) {
            $currentTier = $i + 1;
            $lockoutEnd = $lastAttemptTime + $tiers[$i]['lockout_seconds'];
            if ($now < $lockoutEnd) {
                return [
                    'blocked' => true,
                    'remaining_attempts' => 0,
                    'lockout_remaining' => $lockoutEnd - $now,
                    'total_attempts' => $totalAttempts,
                    'tier' => $currentTier,
                    'storage_error' => false
                ];
            }
            break;
        }
    }

    $nextTierAttempts = $tiers[0]['attempts'];
    foreach ($tiers as $tier) {
        if ($totalAttempts < $tier['attempts']) {
            $nextTierAttempts = $tier['attempts'];
            break;
        }
    }
    return [
        'blocked' => false,
        'remaining_attempts' => max(0, $nextTierAttempts - $totalAttempts),
        'lockout_remaining' => 0,
        'total_attempts' => $totalAttempts,
        'tier' => $currentTier,
        'storage_error' => false
    ];
}

/**
 * Liest eine JSON-Datei mit Shared Lock (LOCK_SH) – wartet auf aktive Write-Locks.
 * Verhindert inkonsistentes Lesen während eines gleichzeitigen Schreibvorgangs.
 *
 * @param string $file - Pfad zur JSON-Datei
 * @return array       - Geparster Inhalt oder leeres Array bei Fehler
 */
function bfReadJsonLocked($file) {
    if (!file_exists($file)) return [];
    $fp = fopen($file, 'rb');
    if (!$fp) return [];
    $data = [];
    if (flock($fp, LOCK_SH)) {
        $valid = true;
        $data = bfReadJsonFromHandle($fp, $valid);
        if (!$valid) $data = [];
        flock($fp, LOCK_UN);
    }
    fclose($fp);
    return is_array($data) ? $data : [];
}

function bfEnsureParentDirectory($file) {
    $dir = dirname($file);
    if (is_dir($dir)) return true;
    @mkdir($dir, 0750, true);
    return is_dir($dir);
}

/** Liest genau den Snapshot des bereits gesperrten Handles. */
function bfReadJsonFromHandle($fp, &$valid) {
    $valid = true;
    rewind($fp);
    $json = stream_get_contents($fp);
    if ($json === false) {
        $valid = false;
        return [];
    }
    if (trim($json) === '') return [];
    $data = json_decode($json, true);
    if (!is_array($data) || json_last_error() !== JSON_ERROR_NONE) {
        $valid = false;
        return [];
    }
    return $data;
}

/** Schreibt und synchronisiert über das bereits exklusiv gesperrte Handle. */
function bfWriteJsonToHandle($fp, $data) {
    $json = json_encode($data, JSON_UNESCAPED_SLASHES);
    if ($json === false || !rewind($fp) || !ftruncate($fp, 0)) return false;
    $offset = 0;
    $length = strlen($json);
    while ($offset < $length) {
        $written = fwrite($fp, substr($json, $offset));
        if ($written === false || $written === 0) return false;
        $offset += $written;
    }
    if (!fflush($fp)) return false;
    if (function_exists('fsync') && !fsync($fp)) return false;
    return true;
}

/**
 * Schreibt eine JSON-Datei mit Exclusive Lock (LOCK_EX) – atomares Read-Modify-Write.
 * Verhindert Race Conditions bei gleichzeitigen Login-Anfragen.
 *
 * @param string $file - Pfad zur JSON-Datei
 * @param array  $data - Zu schreibende Daten
 */
function bfWriteJsonLocked($file, $data) {
    if (!bfEnsureParentDirectory($file)) return false;
    $fp = fopen($file, 'c+b');
    if (!$fp) return false;
    $written = false;
    if (flock($fp, LOCK_EX)) {
        $written = bfWriteJsonToHandle($fp, $data);
        flock($fp, LOCK_UN);
    }
    fclose($fp);
    return $written;
}

/**
 * Führt Statusprüfung, password_verify und Ledger-Update in genau einer
 * exklusiven Sperrtransaktion aus. Parallel-Requests können dadurch weder
 * Fehlversuche verlieren noch gleichzeitig eine gerade erreichte Sperre
 * umgehen.
 */
function authenticateWithBruteForce($ip, $password, $passwordHash, $bfConfig) {
    $attemptFile = $bfConfig['file'];
    $failure = array_merge(bfStorageFailureStatus(), [
        'authenticated' => false,
        'password_checked' => false
    ]);
    if (!bfEnsureParentDirectory($attemptFile)) return $failure;

    $fp = fopen($attemptFile, 'c+b');
    if (!$fp || !flock($fp, LOCK_EX)) {
        if ($fp) fclose($fp);
        return $failure;
    }

    try {
        $valid = true;
        $attempts = bfReadJsonFromHandle($fp, $valid);
        if (!$valid) return $failure;

        $now = time();
        $changed = bfPruneAttempts($attempts, $ip, $now, $bfConfig);
        $before = bfStatusForEntries($attempts[$ip] ?? [], $now, $bfConfig);
        if ($before['blocked']) {
            if ($changed && !bfWriteJsonToHandle($fp, $attempts)) return $failure;
            return array_merge($before, [
                'authenticated' => false,
                'password_checked' => false
            ]);
        }

        // Absichtlich innerhalb des Locks: Status, Verifikation und Mutation
        // bilden eine einzige serialisierte Authentifizierungsentscheidung.
        $authenticated = password_verify($password, $passwordHash);
        if ($authenticated) {
            unset($attempts[$ip]);
        } else {
            if (!isset($attempts[$ip]) || !is_array($attempts[$ip])) $attempts[$ip] = [];
            $attempts[$ip][] = [
                'time' => $now,
                'agent' => utf8SafeByteLimit($_SERVER['HTTP_USER_AGENT'] ?? 'Unknown', 128)
            ];
        }

        if (!bfWriteJsonToHandle($fp, $attempts)) return $failure;
        $after = bfStatusForEntries($attempts[$ip] ?? [], $now, $bfConfig);
        return array_merge($after, [
            'authenticated' => $authenticated,
            'password_checked' => true
        ]);
    } finally {
        flock($fp, LOCK_UN);
        fclose($fp);
    }
}

// ============================================================
// USER-AGENT PARSER - Gerät, Betriebssystem, Browser erkennen
// ============================================================

/**
 * parseUserAgent() - Parst den HTTP User-Agent zu lesbaren Gerätedaten.
 *
 * Extraktion von:
 *   - Betriebssystem (z.B. "Windows 10/11", "iOS 17.0", "Android 14")
 *   - Gerätename     (z.B. "iPhone", "Samsung SM-S918B", "PC", "Mac")
 *   - Browser        (z.B. "Chrome 120", "Safari 17", "Firefox 121")
 *
 * Kein externes Paket – kompakter Inline-Regex-Parser.
 * Reihenfolge der Prüfungen ist bewusst gewählt (spezifisch vor generisch),
 * da viele Browser-UAs mehrere Schlüsselwörter enthalten.
 *
 * @param  string $ua  Roher User-Agent String
 * @return array       ['os' => string, 'device' => string, 'browser' => string]
 */
function parseUserAgent($ua) {
    $os      = 'Unbekannt';
    $device  = 'Unbekannt';
    $browser = 'Unbekannt';

    // ── BETRIEBSSYSTEM & GERÄT (Reihenfolge: spezifisch → generisch) ──

    // iOS / iPhone (vor Android prüfen, da auch "Linux" im UA vorkommt)
    if (preg_match('/iPhone.*?CPU iPhone OS ([\d_]+)/i', $ua, $m)) {
        $os     = 'iOS ' . str_replace('_', '.', $m[1]);
        $device = 'iPhone';

    // iPadOS
    } elseif (preg_match('/iPad.*?CPU OS ([\d_]+)/i', $ua, $m)) {
        $os     = 'iPadOS ' . str_replace('_', '.', $m[1]);
        $device = 'iPad';

    // Android – Modell-Erkennung nach Betriebssystem-Version
    } elseif (preg_match('/Android ([\d.]+);?\s*([^)]*)/i', $ua, $m)) {
        $os  = 'Android ' . $m[1];
        $dev = trim($m[2] ?? '');

        // Samsung (SM-Prefix = Galaxy-Linie)
        if (preg_match('/SM-([A-Z0-9]+)/i', $dev, $dm)) {
            $device = 'Samsung ' . $dm[0];
        } elseif (stripos($dev, 'Samsung') !== false) {
            $device = 'Samsung';
        // Google Pixel
        } elseif (preg_match('/Pixel\s*([\dXLa-z]+)/i', $dev, $dm)) {
            $device = 'Google Pixel ' . $dm[1];
        // OnePlus
        } elseif (preg_match('/OnePlus\s*([\w]+)/i', $dev, $dm)) {
            $device = 'OnePlus ' . $dm[1];
        // Huawei / Honor
        } elseif (preg_match('/(HUAWEI|HONOR)\s*([\w-]+)/i', $dev, $dm)) {
            $device = ucfirst(strtolower($dm[1])) . ' ' . $dm[2];
        // Xiaomi / Redmi / POCO
        } elseif (preg_match('/(Xiaomi|Redmi|POCO)\s*([\w]+)/i', $dev, $dm)) {
            $device = $dm[1] . ' ' . $dm[2];
        // Sonstiger Bezeichner: max. 30 Zeichen übernehmen
        } elseif (!empty($dev)) {
            $device = utf8SafeByteLimit($dev, 30);
        } else {
            $device = 'Android-Gerät';
        }

    // Windows NT → Klartextversion
    } elseif (preg_match('/Windows NT ([\d.]+)/i', $ua, $m)) {
        $ntMap  = [
            '10.0' => '10/11', '6.3' => '8.1', '6.2' => '8',
            '6.1'  => '7',     '6.0' => 'Vista', '5.1' => 'XP'
        ];
        $os     = 'Windows ' . ($ntMap[$m[1]] ?? $m[1]);
        $device = 'PC';

    // macOS (Unterstriche und Punkte als Trennzeichen möglich)
    } elseif (preg_match('/Mac OS X ([\d_.]+)/i', $ua, $m)) {
        $ver    = str_replace('_', '.', $m[1]);
        $os     = 'macOS ' . $ver;
        $device = 'Mac';

    // ChromeOS (vor Linux prüfen – CrOS-UA enthält ebenfalls "Linux")
    } elseif (preg_match('/CrOS/i', $ua)) {
        $os     = 'ChromeOS';
        $device = 'Chromebook';

    // Linux (Desktop)
    } elseif (preg_match('/Linux/i', $ua)) {
        $os     = 'Linux';
        $device = 'Linux-PC';
    }

    // ── BROWSER (Reihenfolge: spezifisch vor generisch) ──

    // Microsoft Edge (Chromium-basiert, enthält "Chrome" im UA → zuerst prüfen)
    if (preg_match('/Edg(?:A|iOS)?\/(\d+)/i', $ua, $m)) {
        $browser = 'Edge ' . $m[1];

    // Samsung Internet (ebenfalls Chromium-basiert)
    } elseif (preg_match('/SamsungBrowser\/(\d+)/i', $ua, $m)) {
        $browser = 'Samsung Browser ' . $m[1];

    // Opera (OPR-Token, nicht der alte "Opera/" Token)
    } elseif (preg_match('/OPR\/(\d+)/i', $ua, $m)) {
        $browser = 'Opera ' . $m[1];

    // Firefox (kein Chromium)
    } elseif (preg_match('/Firefox\/(\d+)/i', $ua, $m)) {
        $browser = 'Firefox ' . $m[1];

    // Chrome / Chromium (nach allen anderen Chromium-Ablegern prüfen)
    } elseif (preg_match('/Chrome\/(\d+)/i', $ua, $m)) {
        $browser = 'Chrome ' . $m[1];

    // Safari (nur wenn kein Chrome-Token im UA – Apple-Geräte)
    } elseif (preg_match('/Version\/(\d+)[^\s]*\s+(?:Mobile\/\S+\s+)?Safari/i', $ua, $m)) {
        $browser = 'Safari ' . $m[1];

    // Fallback: ersten Token des UA-Strings verwenden
    } elseif (!empty($ua)) {
        $browser = utf8SafeByteLimit(explode('/', $ua)[0], 20);
    }

    return ['os' => $os, 'device' => $device, 'browser' => $browser];
}
