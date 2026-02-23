<?php
// ============================================================
// views/login.php - Login-Seite
// ============================================================
// Wird angezeigt wenn der Benutzer NICHT eingeloggt ist.
// Verwendet $loginFailed und $bruteForceStatus aus auth.php.
// ============================================================

// Timeout-Warnmeldung (nach Session-Ablauf)
$timeoutMsg = isset($_GET['timeout'])
    ? '<div class="alert-warning"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><path d="M12 6v6l4 2"/></svg><span>Session expired due to inactivity</span></div>'
    : '';

// Fehlermeldung bei falschem Passwort
$failedMsg = '';
if (isset($loginFailed) && $loginFailed) {
    if ($bruteForceStatus && $bruteForceStatus['blocked']) {
        // === GESPERRT: Lockout-Meldung mit Countdown ===
        $lockoutMin  = ceil($bruteForceStatus['lockout_remaining'] / 60);
        $lockoutSec  = $bruteForceStatus['lockout_remaining'];
        $tierText    = '';
        if ($bruteForceStatus['tier'] >= 3) {
            $tierText = ' (Stufe 3 - Maximale Sperre)';
        } elseif ($bruteForceStatus['tier'] >= 2) {
            $tierText = ' (Stufe 2)';
        }
        $failedMsg = '<div class="alert-warning" style="background:#fee2e2;border-color:#ef4444;color:#991b1b;">'
            . '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">'
            . '<path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/></svg>'
            . '<div><strong>Account temporarily locked' . htmlspecialchars($tierText) . '</strong><br>'
            . '<span style="font-size:13px;">Too many failed attempts (' . $bruteForceStatus['total_attempts'] . '). '
            . 'Try again in <span id="lockout-timer" data-seconds="' . $lockoutSec . '">' . $lockoutMin . ' min</span>.</span></div></div>';
    } else {
        // === Normaler Fehlversuch mit Restversuche-Anzeige ===
        $remainingText = '';
        if ($bruteForceStatus && $bruteForceStatus['remaining_attempts'] <= 3) {
            $remainingText = '<br><span style="font-size:12px;">⚠ ' . $bruteForceStatus['remaining_attempts'] . ' attempt(s) remaining before lockout</span>';
        }
        $failedMsg = '<div class="alert-warning" style="background:#fee2e2;border-color:#ef4444;color:#991b1b;">'
            . '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">'
            . '<circle cx="12" cy="12" r="10"/><path d="M15 9l-6 6M9 9l6 6"/></svg>'
            . '<div>Invalid password' . $remainingText . '</div></div>';
    }
}

// Formular deaktivieren wenn Lockout aktiv
$formDisabled = ($bruteForceStatus && $bruteForceStatus['blocked']) ? 'disabled' : '';
$btnExtraStyle = ($bruteForceStatus && $bruteForceStatus['blocked']) ? 'opacity:0.5;cursor:not-allowed;' : '';
?>
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Login</title>
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { font-family: "Inter", sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; display: flex; align-items: center; justify-content: center; padding: 20px; }
        .login-container { background: rgba(255,255,255,0.95); backdrop-filter: blur(10px); padding: 40px; border-radius: 16px; box-shadow: 0 20px 60px rgba(0,0,0,0.3); width: 100%; max-width: 400px; }
        .login-header { text-align: center; margin-bottom: 32px; }
        .login-header svg { width: 48px; height: 48px; margin-bottom: 16px; color: #667eea; }
        .login-header h1 { font-size: 24px; font-weight: 700; color: #1a1a1a; margin-bottom: 8px; }
        .login-header p { color: #666; font-size: 14px; }
        .alert-warning { display: flex; align-items: center; gap: 12px; padding: 12px 16px; background: #fff3cd; border: 1px solid #ffc107; border-radius: 8px; color: #856404; font-size: 14px; margin-bottom: 20px; }
        .alert-warning svg { flex-shrink: 0; }
        .form-group { margin-bottom: 20px; }
        .form-label { display: block; font-weight: 500; font-size: 14px; color: #333; margin-bottom: 8px; }
        .form-input { width: 100%; padding: 12px 16px; border: 2px solid #e0e0e0; border-radius: 8px; font-size: 15px; font-family: inherit; transition: border-color 0.2s; }
        .form-input:focus { outline: none; border-color: #667eea; }
        .form-input:disabled { background: #f3f4f6; cursor: not-allowed; }
        .btn-primary { width: 100%; padding: 14px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; border: none; border-radius: 8px; font-size: 15px; font-weight: 600; cursor: pointer; transition: transform 0.2s, box-shadow 0.2s; }
        .btn-primary:hover:not(:disabled) { transform: translateY(-2px); box-shadow: 0 10px 20px rgba(102,126,234,0.3); }
        .btn-primary:active:not(:disabled) { transform: translateY(0); }
        .btn-primary:disabled { opacity: 0.5; cursor: not-allowed; }
        @keyframes pulse-red { 0%, 100% { opacity: 1; } 50% { opacity: 0.6; } }
        .lockout-active { animation: pulse-red 2s infinite; }
    </style>
</head>
<body>
    <div class="login-container">
        <div class="login-header">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                <rect x="3" y="11" width="18" height="11" rx="2" ry="2"/>
                <path d="M7 11V7a5 5 0 0 1 10 0v4"/>
            </svg>
            <h1>Secure Login</h1>
            <p>Access Control Center</p>
        </div>

        <?php echo $timeoutMsg; ?>
        <?php echo $failedMsg; ?>

        <form method="post" action="keks.php" id="login-form">
            <div class="form-group">
                <label class="form-label">Password</label>
                <input type="password" name="password" class="form-input" placeholder="Enter password" required autofocus <?php echo $formDisabled; ?>>
            </div>
            <button type="submit" class="btn-primary" id="login-btn" <?php echo $formDisabled; ?> style="<?php echo $btnExtraStyle; ?>">
                Sign In
            </button>
        </form>
    </div>

    <!-- Lockout Countdown Timer -->
    <script>
    (function() {
        var timerEl = document.getElementById('lockout-timer');
        if (!timerEl) return;
        var seconds = parseInt(timerEl.getAttribute('data-seconds')) || 0;
        if (seconds <= 0) return;
        var form  = document.getElementById('login-form');
        var btn   = document.getElementById('login-btn');
        var input = form ? form.querySelector('input[name=password]') : null;
        var interval = setInterval(function() {
            seconds--;
            if (seconds <= 0) {
                clearInterval(interval);
                timerEl.textContent = 'now';
                if (input) { input.disabled = false; input.focus(); }
                if (btn)   { btn.disabled = false; btn.style.opacity = '1'; btn.style.cursor = 'pointer'; }
                return;
            }
            var m = Math.floor(seconds / 60);
            var s = seconds % 60;
            timerEl.textContent = m + ':' + (s < 10 ? '0' : '') + s;
        }, 1000);
    })();
    </script>
</body>
</html>
