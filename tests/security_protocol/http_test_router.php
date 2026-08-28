<?php
declare(strict_types=1);

// Nur fuer den isolierten PHP-Builtin-Server der Routing-Regressionstests.
// Er simuliert den produktiven TLS-Terminator, ohne Testschalter in api.php.
$_SERVER['HTTPS'] = 'on';
$_SERVER['SERVER_PORT'] = '443';
$path = parse_url($_SERVER['REQUEST_URI'] ?? '/', PHP_URL_PATH);

if ($path === '/__test_session') {
    ini_set('session.cookie_httponly', '1');
    ini_set('session.cookie_samesite', 'Strict');
    ini_set('session.cookie_secure', '1');
    session_start();
    $_SESSION['loggedin'] = true;
    $_SESSION['last_activity'] = time();
    $_SESSION['csrf_token'] = bin2hex(random_bytes(32));
    header('Content-Type: application/json');
    echo json_encode(['csrf_token' => $_SESSION['csrf_token']], JSON_THROW_ON_ERROR);
    return true;
}

if ($path === '/api.php') {
    require dirname(__DIR__, 2) . '/web/api.php';
    return true;
}

http_response_code(404);
echo "not found\n";
return true;
