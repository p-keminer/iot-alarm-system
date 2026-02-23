    <!-- ============================================================
         HEADER - Logo, Navigation, Logout
         ============================================================ -->
    <header class="header">
        <div class="header-left">
            <!-- Logo mit Schild-Icon -->
            <div class="logo">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/>
                </svg>
                <span id="site-title"><?php echo htmlspecialchars($settings['site_title']); ?></span>
            </div>

            <!-- Tab-Navigation -->
            <nav class="nav-tabs">
                <button id="btn-dash" class="tab-btn active" onclick="switchView('dashboard')">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="3" width="7" height="7"/><rect x="14" y="3" width="7" height="7"/><rect x="14" y="14" width="7" height="7"/><rect x="3" y="14" width="7" height="7"/></svg>
                    Dashboard
                </button>
                <button id="btn-logs" class="tab-btn" onclick="switchView('logs')">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>
                    Logs
                </button>
                <button id="btn-diag" class="tab-btn" onclick="switchView('diagnose')">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/></svg>
                    Diagnose
                </button>
                <button id="btn-conf" class="tab-btn" onclick="switchView('config')">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M12 1v6m0 6v6m-6-6h6m6 0h-6M4.2 4.2l4.2 4.2m7.2 0l4.2-4.2M4.2 19.8l4.2-4.2m7.2 0l4.2 4.2"/></svg>
                    Settings
                </button>
                <!-- Audit Tab (nur sichtbar wenn site_title = 'admin') -->
                <?php if (strtolower($settings['site_title']) === 'admin'): ?>
                <button id="btn-userlogs" class="tab-btn" onclick="switchView('userlogs')">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/></svg>
                    Audit
                </button>
                <?php endif; ?>
            </nav>
        </div>

        <!-- Logout-Button -->
        <a href="?logout" class="btn-logout">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/><polyline points="16 17 21 12 16 7"/><line x1="21" y1="12" x2="9" y2="12"/></svg>
            Logout
        </a>
    </header>

    <!-- ============================================================
         HAUPTINHALT - Alle Views/Tabs
         ============================================================ -->
    <main class="main-content">
        <div class="container">

            <!-- ========================================================
                 DASHBOARD VIEW - Geräteübersicht & Steuerung
                 ======================================================== -->
            <div id="view-dashboard">
                <div class="page-header">
                    <h1 class="page-title">Network Status</h1>
                    <p class="page-subtitle">Real-time monitoring of connected devices</p>
                </div>
                <div class="grid">

                    <!-- === ESP SENDER KARTE === -->
                    <div id="card-sender" class="card sender">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="22" y1="2" x2="11" y2="13"/><polygon points="22 2 15 22 11 13 2 9 22 2"/></svg>
                                ESP Sender
                            </div>
                            <div id="dot-sender" class="status-indicator"></div>
                        </div>
                        <div class="status-message" id="msg-sender">Waiting for connection...</div>
                        <div class="card-content">
                            <div class="info-row">
                                <span class="info-label">IP Address</span>
                                <span class="info-value" id="ip-sender">---</span>
                            </div>
                        </div>
                        <div class="btn-group">
                            <button class="btn btn-danger" onclick="sendCommand('sender', 'REBOOT')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg>
                                Reboot
                            </button>
                            <button class="btn btn-danger" onclick="sendCommand('sender', 'RESET')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="1 4 1 10 7 10"/><path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10"/></svg>
                                Reset
                            </button>
                        </div>
                    </div>

                    <!-- === ESP RECEIVER KARTE === -->
                    <div id="card-receiver" class="card receiver">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="17 11 12 6 7 11"/><polyline points="17 18 12 13 7 18"/></svg>
                                ESP Receiver
                            </div>
                            <div id="dot-receiver" class="status-indicator"></div>
                        </div>
                        <div class="status-message" id="msg-receiver">Waiting for connection...</div>
                        <div class="card-content">
                            <div class="info-row">
                                <span class="info-label">IP Address</span>
                                <span class="info-value" id="ip-receiver">---</span>
                            </div>
                        </div>
                        <div class="btn-group">
                            <!-- Alarm Toggle Switch -->
                            <div class="toggle-wrapper">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9"/><path d="M13.73 21a2 2 0 0 1-3.46 0"/></svg>
                                <label class="switch">
                                    <input type="checkbox" id="alarm-toggle" onchange="toggleAlarm(this)">
                                    <span class="slider"></span>
                                </label>
                            </div>
                            <button class="btn btn-danger" onclick="sendCommand('receiver', 'REBOOT')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg>
                                Reboot
                            </button>
                            <button class="btn btn-danger" onclick="sendCommand('receiver', 'RESET')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="1 4 1 10 7 10"/><path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10"/></svg>
                                Reset
                            </button>
                        </div>
                    </div>

                    <!-- === PICAM KARTE (Raspberry Pi Kamera) === -->
                    <div id="card-camera" class="card camera">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2z"/><circle cx="12" cy="13" r="4"/></svg>
                                PiCam
                            </div>
                            <div id="dot-camera" class="status-indicator online"></div>
                        </div>
                        <div class="status-message" id="msg-camera">Running</div>
                        <div class="card-content">
                            <div class="info-row">
                                <span class="info-label">IP Address</span>
                                <span class="info-value" id="ip-camera">---</span>
                            </div>
                            <div class="info-row">
                                <span class="info-label">CPU Temp</span>
                                <span class="info-value" id="pi-cpu-temp">---</span>
                            </div>
                            <div class="info-row">
                                <span class="info-label">CPU Load</span>
                                <span class="info-value" id="pi-cpu-load">---</span>
                            </div>
                            <div class="info-row">
                                <span class="info-label">Aufnahme</span>
                                <span class="info-value" id="pi-rec-status">---</span>
                            </div>
                        </div>
                        <div class="btn-group">
                            <button class="btn btn-primary" onclick="openCameraStream()">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polygon points="5 3 19 12 5 21 5 3"/></svg>
                                Stream
                            </button>
                            <button class="btn btn-primary" onclick="switchView('recordings')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
                                Aufnahmen
                            </button>
                            <button class="btn btn-danger" onclick="rebootPi()">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg>
                                Reboot
                            </button>
                            <button class="btn btn-danger" onclick="shutdownPi()">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>
                                Shutdown
                            </button>
                        </div>
                    </div>

                    <!-- === ALARMSTEUERUNG KARTE (Arduino Serial) === -->
                    <div id="card-alarm" class="card">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/></svg>
                                Alarmsteuerung
                            </div>
                            <div id="alarm-serial-status" style="font-size:12px;color:var(--text-secondary);">Arduino</div>
                        </div>
                        <div class="status-message" id="alarm-serial-msg">Bereit</div>
                        <div class="card-content">
                            <div class="info-row">
                                <span class="info-label">Schnittstelle</span>
                                <span class="info-value" id="alarm-serial-port">Auto-Detect</span>
                            </div>
                        </div>
                        <div class="btn-group">
                            <button class="btn btn-success" onclick="sendSerialCmd('1')" style="flex:1;">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/></svg>
                                Aktivieren
                            </button>
                            <button class="btn btn-danger" onclick="sendSerialCmd('0')" style="flex:1;">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>
                                Deaktivieren
                            </button>
                        </div>
                    </div>
                </div>
            </div>

            <!-- ========================================================
                 LOGS VIEW - Live Kommunikations-Terminals
                 ======================================================== -->
            <div id="view-logs" class="view-section">
                <div class="page-header">
                    <h1 class="page-title">Live Communication Logs</h1>
                    <p class="page-subtitle">Real-time device communication streams</p>
                </div>
                <div class="grid">
                    <!-- Sender Terminal -->
                    <div class="card">
                        <div class="terminal-header">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/></svg>
                            ESP Sender Terminal
                        </div>
                        <div id="log-sender" class="terminal"></div>
                        <div class="terminal-actions">
                            <button class="btn btn-danger" onclick="clearLog('log-sender')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                                Clear
                            </button>
                        </div>
                    </div>

                    <!-- Receiver Terminal -->
                    <div class="card">
                        <div class="terminal-header">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/></svg>
                            ESP Receiver Terminal
                        </div>
                        <div id="log-receiver" class="terminal"></div>
                        <div class="terminal-actions">
                            <button class="btn btn-danger" onclick="clearLog('log-receiver')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                                Clear
                            </button>
                        </div>
                    </div>

                    <!-- PiCam Terminal -->
                    <div class="card">
                        <div class="terminal-header">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/></svg>
                            PiCam Terminal
                        </div>
                        <div id="log-camera" class="terminal"></div>
                        <div class="terminal-actions">
                            <button class="btn btn-danger" onclick="clearLog('log-camera')">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                                Clear
                            </button>
                        </div>
                    </div>
                </div>
            </div>

            <!-- ========================================================
                 DIAGNOSE VIEW - Telemetrie & System-Überwachung
                 ======================================================== -->
            <div id="view-diagnose" class="view-section">
                <div class="page-header">
                    <h1 class="page-title">System-Diagnose &amp; Telemetrie</h1>
                    <p class="page-subtitle">Detaillierte Systemüberwachung und Performance-Analyse</p>
                </div>

                <!-- Auto-Refresh Toggle -->
                <div class="alert alert-info" style="display: flex; align-items: center; gap: 12px;">
                    <label class="switch">
                        <input type="checkbox" id="diag-auto-refresh" checked>
                        <span class="slider"></span>
                    </label>
                    <span>Auto-Refresh (alle 5 Sek.)</span>
                </div>

                <!-- Statistik-Übersicht (4 Boxen) -->
                <div class="stats-grid">
                    <?php
                    $onlineCount = 0;
                    $totalUptime = 0;
                    foreach ($diagStatus as $data) {
                        if (isset($data['last_seen']) && time() - $data['last_seen'] < 30) $onlineCount++;
                        if (isset($data['uptime'])) $totalUptime += $data['uptime'];
                    }
                    ?>
                    <div class="stat-box">
                        <div class="stat-label">Online Geräte</div>
                        <div class="stat-value"><?php echo $onlineCount; ?> / <?php echo count($diagStatus); ?></div>
                    </div>
                    <div class="stat-box">
                        <div class="stat-label">Telemetrie-Einträge</div>
                        <div class="stat-value"><?php echo file_exists($csvFile) ? count(file($csvFile)) : 0; ?></div>
                    </div>
                    <div class="stat-box">
                        <div class="stat-label">System Logs</div>
                        <div class="stat-value"><?php echo count($systemLogs); ?></div>
                    </div>
                    <div class="stat-box" style="border-left-color: var(--accent-green);">
                        <div class="stat-label">Gesamt Uptime</div>
                        <div class="stat-value" style="color: var(--accent-green); font-size: 1.5rem;">
                            <?php echo gmdate("H:i:s", $totalUptime); ?>
                        </div>
                    </div>
                </div>

                <!-- Geräte-Status Tabelle -->
                <div class="card" style="margin-bottom: 24px;">
                    <h3 style="margin-bottom: 16px; display: flex; align-items: center; gap: 8px;">
                        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/></svg>
                        Geräte-Status &amp; Telemetrie
                    </h3>
                    <div style="overflow-x: auto;">
                        <table>
                            <thead>
                                <tr>
                                    <th>Gerät</th>
                                    <th>Status</th>
                                    <th>IP-Adresse</th>
                                    <th>Zuletzt gesehen</th>
                                    <th>Uptime</th>
                                    <th>Reset Grund</th>
                                    <th>Freier RAM</th>
                                    <th>Signal (RSSI)</th>
                                </tr>
                            </thead>
                            <tbody>
                                <?php if ($diagStatus): foreach ($diagStatus as $name => $data):
                                    $isOnline  = isset($data['last_seen']) && time() - $data['last_seen'] < 30;
                                    $rssi      = $data['rssi'] ?? 0;
                                    $rssiClass = $rssi > -60 ? 'badge-good' : ($rssi > -75 ? 'badge-warning' : 'badge-critical');
                                ?>
                                <tr>
                                    <td style="font-weight:600;"><?php echo htmlspecialchars(ucfirst($name)); ?></td>
                                    <td>
                                        <span class="status-badge <?php echo $isOnline ? 'badge-online' : 'badge-offline'; ?>">
                                            <?php echo $isOnline ? 'ONLINE' : 'OFFLINE'; ?>
                                        </span>
                                    </td>
                                    <td><?php echo htmlspecialchars($data['ip'] ?? 'N/A'); ?></td>
                                    <td style="font-size: 13px;">
                                        <?php
                                        if (isset($data['last_seen'])) {
                                            $diff = time() - $data['last_seen'];
                                            echo date("d.m.Y H:i:s", $data['last_seen']);
                                            echo " <span style='color: var(--text-tertiary);'>(" . $diff . "s)</span>";
                                        } else {
                                            echo '-';
                                        }
                                        ?>
                                    </td>
                                    <td><?php echo isset($data['uptime']) ? gmdate("H:i:s", $data['uptime']) : '-'; ?></td>
                                    <td>
                                        <span style="color: <?php echo in_array($data['reset_reason'] ?? '', ['External System', 'Power On']) ? 'var(--accent-green)' : 'var(--accent-red)'; ?>">
                                            <?php echo htmlspecialchars($data['reset_reason'] ?? 'unknown'); ?>
                                        </span>
                                    </td>
                                    <td><?php echo number_format($data['heap'] ?? 0); ?> B</td>
                                    <td>
                                        <span class="status-badge <?php echo $rssiClass; ?>">
                                            <?php echo $rssi; ?> dBm
                                        </span>
                                    </td>
                                </tr>
                                <?php endforeach; else: ?>
                                <tr><td colspan="8" style="text-align: center; color: var(--text-secondary);">Keine Geräte verbunden</td></tr>
                                <?php endif; ?>
                            </tbody>
                        </table>
                    </div>
                </div>

                <!-- Telemetrie-Charts (RSSI & Heap) -->
                <div class="grid-2" style="margin-top: 24px;">
                    <div class="card">
                        <h3 style="margin-bottom: 16px; display: flex; align-items: center; gap: 8px;">
                            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="2"/><path d="M16.24 7.76a6 6 0 0 1 0 8.49m-8.48-.01a6 6 0 0 1 0-8.49m11.31-2.82a10 10 0 0 1 0 14.14m-14.14 0a10 10 0 0 1 0-14.14"/></svg>
                            WLAN-Signal (RSSI)
                        </h3>
                        <div class="chart-container"><canvas id="rssiChart"></canvas></div>
                    </div>
                    <div class="card">
                        <h3 style="margin-bottom: 16px; display: flex; align-items: center; gap: 8px;">
                            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="4" y="4" width="16" height="16" rx="2" ry="2"/><rect x="9" y="9" width="6" height="6"/><line x1="9" y1="1" x2="9" y2="4"/><line x1="15" y1="1" x2="15" y2="4"/><line x1="9" y1="20" x2="9" y2="23"/><line x1="15" y1="20" x2="15" y2="23"/><line x1="20" y1="9" x2="23" y2="9"/><line x1="20" y1="14" x2="23" y2="14"/><line x1="1" y1="9" x2="4" y2="9"/><line x1="1" y1="14" x2="4" y2="14"/></svg>
                            Verfügbarer RAM (Heap)
                        </h3>
                        <div class="chart-container"><canvas id="heapChart"></canvas></div>
                    </div>
                </div>

                <!-- System-Logs Terminal -->
                <div class="card" style="margin-top: 24px;">
                    <h3 style="margin-bottom: 16px; display: flex; align-items: center; gap: 8px;">
                        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>
                        System-Logs (Letzte 50 Einträge)
                    </h3>
                    <div class="terminal" style="height: 400px;">
                        <?php if (!empty($systemLogs)): ?>
                            <?php foreach ($systemLogs as $log): ?>
                                <div class="log-line"><?php echo htmlspecialchars($log); ?></div>
                            <?php endforeach; ?>
                        <?php else: ?>
                            <div style="color: var(--text-secondary); text-align: center; padding: 40px;">
                                Keine Logs verfügbar
                            </div>
                        <?php endif; ?>
                    </div>
                </div>

                <!-- Wartung & Daten-Export -->
                <div class="card" style="margin-top: 24px;">
                    <h3 style="margin-bottom: 16px; display: flex; align-items: center; gap: 8px;">
                        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M12 1v6m0 6v6"/></svg>
                        Wartung &amp; Export
                    </h3>
                    <div class="btn-group">
                        <a href="api.php?action=export_telemetry" class="btn btn-primary">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
                            Telemetrie exportieren (CSV)
                        </a>
                        <button class="btn btn-danger" onclick="clearTelemetry()">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                            Telemetrie löschen
                        </button>
                        <button class="btn btn-danger" onclick="clearAllLogs()">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                            System-Logs löschen
                        </button>
                    </div>
                </div>
            </div>

            <!-- ========================================================
                 CONFIG VIEW - Dashboard & Node-Konfiguration
                 ======================================================== -->
            <div id="view-config" class="view-section">
                <div class="page-header">
                    <h1 class="page-title">System Configuration</h1>
                    <p class="page-subtitle">Manage dashboard and device settings</p>
                </div>
                <div class="grid">
                    <!-- Dashboard-Einstellungen -->
                    <div class="card">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="3" width="18" height="18" rx="2" ry="2"/><line x1="9" y1="3" x2="9" y2="21"/></svg>
                                Dashboard Settings
                            </div>
                        </div>

                        <div class="alert alert-info" id="timeout-status">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/></svg>
                            <div>
                                <div id="timeout-status-text" style="font-weight: 600;">Auto-Logout: Disabled</div>
                                <div id="timeout-info" style="font-size: 12px; opacity: 0.8; margin-top: 2px;">No automatic logout</div>
                            </div>
                        </div>

                        <div class="form-group">
                            <label class="form-label">Page Title</label>
                            <input type="text" id="cfg-title" class="form-input" value="<?php echo htmlspecialchars($settings['site_title']); ?>">
                        </div>
                        <div class="form-group">
                            <label class="form-label">Change Admin Password</label>
                            <input type="password" id="cfg-pw" class="form-input" placeholder="Enter new password">
                        </div>
                        <div class="form-group">
                            <label class="form-label">Alarm-PIN aendern</label>
                            <input type="password" id="cfg-alarm-pin" class="form-input" placeholder="Neuer Alarm-PIN (Standard: CHANGE_ME)">
                            <div class="form-hint">Separater PIN fuer Alarm Aktivieren/Deaktivieren</div>
                        </div>
                        <div class="form-group">
                            <label class="form-label">Refresh Rate (ms)</label>
                            <input type="number" id="cfg-refresh" class="form-input" value="<?php echo (int)$settings['refresh_rate']; ?>">
                        </div>
                        <div class="form-group">
                            <label class="form-label">Auto-Logout Timeout (Minutes)</label>
                            <input type="number" id="cfg-timeout-min" class="form-input" value="<?php echo (int)($settings['timeout_minutes'] ?? 5); ?>" min="1" max="60">
                            <div class="form-hint">Inactivity period before automatic logout</div>
                        </div>
                        <div class="form-group">
                            <div class="toggle-wrapper">
                                <span>Enable Auto-Logout</span>
                                <label class="switch">
                                    <input type="checkbox" id="cfg-timeout-active" <?php echo ($settings['timeout_active'] ?? false) ? 'checked' : ''; ?>>
                                    <span class="slider"></span>
                                </label>
                            </div>
                        </div>

                        <button class="btn btn-primary" onclick="saveConfig()" style="width: 100%;">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/><polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/></svg>
                            Save Settings
                        </button>

                        <div class="alert alert-danger" style="margin-top: 24px;">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>
                            <span style="font-weight: 600;">Danger Zone</span>
                        </div>
                        <button class="btn btn-danger" onclick="resetSystem()" style="width: 100%;">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/><line x1="10" y1="11" x2="10" y2="17"/><line x1="14" y1="11" x2="14" y2="17"/></svg>
                            System Reset
                        </button>
                    </div>

                    <!-- ESP Node Remote-Konfiguration -->
                    <div class="card">
                        <div class="card-header">
                            <div class="card-title">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="2"/><path d="M16.24 7.76a6 6 0 0 1 0 8.49m-8.48-.01a6 6 0 0 1 0-8.49m11.31-2.82a10 10 0 0 1 0 14.14m-14.14 0a10 10 0 0 1 0-14.14"/></svg>
                                Node Remote Config
                            </div>
                        </div>

                        <div class="alert alert-info">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/><line x1="12" y1="8" x2="12.01" y2="8"/></svg>
                            <span>Only online devices can be configured</span>
                        </div>

                        <div class="form-group">
                            <label class="form-label">Target Device</label>
                            <select id="conf-target" class="form-select">
                                <option value="" disabled selected>Loading status...</option>
                            </select>
                        </div>
                        <div class="form-group">
                            <label class="form-label">API Server IP</label>
                            <input type="text" id="conf-apiip" class="form-input" placeholder="e.g. 192.168.1.50">
                        </div>
                        <div class="form-group">
                            <label class="form-label">Primary WLAN SSID</label>
                            <input type="text" id="conf-mssid" class="form-input" placeholder="Network name">
                        </div>
                        <div class="form-group">
                            <label class="form-label">Primary WLAN Password</label>
                            <input type="password" id="conf-mpass" class="form-input" placeholder="Network password">
                        </div>
                        <div class="form-group">
                            <label class="form-label">Backup WLAN SSID</label>
                            <input type="text" id="conf-bssid" class="form-input" placeholder="Backup network name">
                        </div>
                        <div class="form-group">
                            <label class="form-label">Backup WLAN Password</label>
                            <input type="password" id="conf-bpass" class="form-input" placeholder="Backup password">
                        </div>
                        <div class="form-group">
                            <label class="form-label">Telnet Password</label>
                            <input type="password" id="conf-telnet" class="form-input" placeholder="Telnet access password">
                        </div>

                        <button class="btn btn-primary" onclick="sendNodeConfig()" style="width: 100%;">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="12" y1="5" x2="12" y2="19"/><polyline points="19 12 12 19 5 12"/></svg>
                            Send to ESP
                        </button>
                    </div>
                </div>
            </div>

            <!-- ========================================================
                 AUDIT LOG VIEW (nur für Admin sichtbar)
                 ======================================================== -->
            <?php if (strtolower($settings['site_title']) === 'admin'): ?>
            <div id="view-userlogs" class="view-section">
                <div class="page-header">
                    <h1 class="page-title">Security Audit Log</h1>
                    <p class="page-subtitle">Administrator activity tracking and monitoring</p>
                </div>

                <div class="card" style="margin-bottom: 24px;">
                    <div class="alert alert-danger">
                        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>
                        <div>
                            <div style="font-weight: 600;">Security Protocol Active</div>
                            <div style="font-size: 12px; opacity: 0.8; margin-top: 2px;">All administrative actions are logged and monitored</div>
                        </div>
                    </div>
                    <div class="btn-group">
                        <button class="btn btn-primary" onclick="exportUserLogs()">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
                            Export Logs
                        </button>
                        <button class="btn btn-danger" onclick="clearUserLogs()">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                            Clear Logs
                        </button>
                    </div>
                </div>

                <div class="card">
                    <div id="userlog-container" class="log-container">
                        <div style="text-align: center; padding: 40px; color: var(--text-secondary);">
                            <svg style="width: 48px; height: 48px; margin-bottom: 16px; opacity: 0.5;" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="12" y1="2" x2="12" y2="6"/><line x1="12" y1="18" x2="12" y2="22"/><line x1="4.93" y1="4.93" x2="7.76" y2="7.76"/><line x1="16.24" y1="16.24" x2="19.07" y2="19.07"/><line x1="2" y1="12" x2="6" y2="12"/><line x1="18" y1="12" x2="22" y2="12"/><line x1="4.93" y1="19.07" x2="7.76" y2="16.24"/><line x1="16.24" y1="7.76" x2="19.07" y2="4.93"/></svg>
                            <div>Loading audit logs...</div>
                        </div>
                    </div>
                </div>
            </div>

            <!-- ========================================================
                 RECORDINGS VIEW - Alarm-Kameraaufnahmen
                 ======================================================== -->
            <div id="view-recordings" class="view-section">
                <div class="page-header">
                    <h1 class="page-title">Alarm-Aufnahmen</h1>
                    <p class="page-subtitle">Automatische Kameraaufnahmen bei Alarm</p>
                </div>

                <div class="card" style="margin-bottom: 24px;">
                    <div class="alert alert-info" id="rec-monitor-status">
                        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/></svg>
                        <div>
                            <div style="font-weight: 600;" id="rec-monitor-text">Alarm-Monitor Status</div>
                            <div style="font-size: 12px; opacity: 0.8; margin-top: 2px;" id="rec-monitor-detail">Lade...</div>
                        </div>
                    </div>
                    <div class="btn-group">
                        <button class="btn btn-primary" onclick="loadRecordings()">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg>
                            Aktualisieren
                        </button>
                        <button class="btn btn-danger" onclick="deleteAllRecordings()">
                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                            Alle loeschen
                        </button>
                    </div>
                </div>

                <div class="card">
                    <div id="recordings-container" style="min-height: 100px;">
                        <div style="text-align: center; padding: 40px; color: var(--text-secondary);">
                            Lade Aufnahmen...
                        </div>
                    </div>
                </div>
            </div>

            <?php endif; ?>

        </div>
    </main>
