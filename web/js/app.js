// ============================================================
// js/app.js - Kritische Dashboard-Funktionen
// ============================================================
// Enthält: CSRF-Setup, API-Helper, Dashboard-Updates,
// Navigation, Gerätesteuerung, Konfiguration, Logs,
// Aufnahmen-Verwaltung, Arduino Serial.
//
// PHP-Konfigurationsvariablen werden von index.php als
// inline <script>-Block vor diesem File gesetzt:
//   - CAMERA_PORT, refreshRate, timeoutActive, timeoutMinutes
// ============================================================

// ============================================================
// SECURITY: CSRF-Token & Secure API Helper
// ============================================================
var csrfMeta  = document.querySelector('meta[name="csrf-token"]');
var CSRF_TOKEN = csrfMeta ? csrfMeta.content : '';

/**
 * apiCall() - Zentraler API-Wrapper mit CSRF-Schutz
 *
 * Sendet POST-Requests an api.php mit automatischem CSRF-Token.
 * Bei HTTP 403 (Session abgelaufen) → automatischer Redirect zum Login.
 *
 * @param {string} action  - API-Aktion (z.B. 'send_command', 'save_settings')
 * @param {Object} params  - Zusätzliche Parameter als Key-Value Paare
 * @returns {Promise}      - Fetch-Response Promise
 */
function apiCall(action, params) {
    if (!params) params = {};
    params.csrf_token = CSRF_TOKEN;
    console.log('[API]', action, params);
    return fetch('api.php?action=' + action, {
        method: 'POST',
        headers: {
            'Content-Type': 'application/x-www-form-urlencoded',
            'X-CSRF-TOKEN': CSRF_TOKEN
        },
        body: new URLSearchParams(params),
        credentials: 'same-origin'
    }).then(function(response) {
        console.log('[API Response]', action, response.status);
        if (response.status === 403) {
            window.location.href = 'index.php?timeout=1';
            return new Promise(function() {});
        }
        return response;
    })['catch'](function(err) {
        console.error('[API Error]', action, err);
        throw err;
    });
}

// ============================================================
// GLOBALE VARIABLEN
// ============================================================
var updateTimer = null;  // Timer-Handle für Dashboard-Loop

// ============================================================
// ACTIVITY TRACKING (für Session-Timeout)
// ============================================================
var lastPing = 0;
function resetActivityTimer() {
    if (!timeoutActive) return;
    var now = Date.now();
    if (now - lastPing < 30000) return;  // Throttle: max alle 30s
    lastPing = now;
    fetch('api.php?action=ping_activity', {method: 'POST', credentials: 'same-origin'});
}

['mousedown', 'keydown', 'scroll', 'touchstart'].forEach(function(event) {
    document.addEventListener(event, resetActivityTimer, {passive: true});
});

// ============================================================
// GERÄTESTEUERUNG - ESP Befehle senden
// ============================================================

/**
 * sendCommand() - Befehl an ESP-Node senden
 * @param {string} target - 'sender' oder 'receiver'
 * @param {string} cmd    - Befehl: 'REBOOT', 'RESET', 'ALARM_ON', 'ALARM_OFF'
 */
function sendCommand(target, cmd) {
    if (!confirm('Execute ' + cmd + ' on ' + target + '?')) return;
    console.log('[CMD] Sending', cmd, 'to', target);
    apiCall('send_command', {target: target, cmd: cmd})
        .then(function(r) { return r.json(); })
        .then(function(data) {
            console.log('[CMD] Response data:', data);
            alert(data.message || data.error || 'Unknown response');
        })
        ['catch'](function(err) {
            console.error('[CMD] Error:', err);
            alert('Request failed: ' + err);
        });
}

/**
 * toggleAlarm() - Alarm über ESP Receiver ein/ausschalten
 * @param {HTMLElement} checkbox - Alarm Toggle Checkbox
 */
function toggleAlarm(checkbox) {
    var cmd = checkbox.checked ? 'ALARM_ON' : 'ALARM_OFF';
    console.log('[ALARM] Sending', cmd);
    apiCall('send_command', {target: 'receiver', cmd: cmd})
        .then(function(r) { return r.json(); })
        .then(function(data) { console.log('[ALARM] Response:', data); })
        ['catch'](function(err) { console.error('[ALARM] Error:', err); });
}

// ============================================================
// LOG-VERWALTUNG
// ============================================================

/**
 * clearLog() - Terminal-Log eines Geräts löschen
 * @param {string} id - DOM-ID des Terminal-Elements
 */
function clearLog(id) {
    var target = '';
    if (id === 'log-sender')   target = 'sender';
    if (id === 'log-receiver') target = 'receiver';
    if (id === 'log-camera')   target = 'camera';
    if (!target) return;
    if (!confirm(target + ' Logs wirklich loeschen?')) return;
    apiCall('clear_logs', {target: target})
        .then(function(r) { return r.json(); })
        .then(function() {
            document.getElementById(id).innerHTML = '<div style="opacity:0.5;text-align:center;padding:20px;">Log cleared</div>';
        })
        ['catch'](function() { alert('Fehler beim Loeschen'); });
}

/** clearTelemetry() - Alle Telemetrie-CSV-Daten löschen */
function clearTelemetry() {
    if (!confirm('Telemetrie-Daten wirklich loeschen? Charts werden zurueckgesetzt.')) return;
    apiCall('clear_telemetry')
        .then(function(r) { return r.json(); })
        .then(function(data) { alert(data.message || data.error); location.reload(); })
        ['catch'](function() { alert('Fehler beim Loeschen'); });
}

/** clearAllLogs() - Alle System-Logs löschen */
function clearAllLogs() {
    if (!confirm('ALLE System-Logs wirklich loeschen?')) return;
    apiCall('clear_all_logs')
        .then(function(r) { return r.json(); })
        .then(function(data) { alert(data.message || data.error); location.reload(); })
        ['catch'](function() { alert('Fehler beim Loeschen'); });
}

// ============================================================
// KAMERA & RASPBERRY PI STEUERUNG
// ============================================================

/** openCameraStream() - Kamera-Stream in neuem Tab öffnen */
function openCameraStream() {
    window.open('stream.php', '_blank');
}

/** rebootPi() - Raspberry Pi neustarten (doppelte Bestätigung) */
function rebootPi() {
    if (!confirm('Raspberry Pi wirklich neustarten?')) return;
    if (!confirm('ACHTUNG: Dashboard wird kurzzeitig nicht erreichbar!')) return;
    apiCall('pi_reboot')
        .then(function(r) { return r.json(); })
        .then(function(data) { alert(data.message || data.error); })
        ['catch'](function() { alert('Request failed'); });
}

/** shutdownPi() - Pi komplett herunterfahren (erfordert Alarm-PIN) */
function shutdownPi() {
    var pin = prompt('Alarm-PIN eingeben um Pi HERUNTERZUFAHREN:');
    if (pin === null || pin === '') return;
    if (!confirm('ACHTUNG: Pi wird KOMPLETT heruntergefahren! Nur durch physischen Neustart wieder erreichbar!')) return;
    apiCall('pi_shutdown', {alarm_pin: pin})
        .then(function(r) { return r.json(); })
        .then(function(data) { alert(data.message || data.error); })
        ['catch'](function() { alert('Request failed'); });
}

// ============================================================
// AUFNAHMEN-VERWALTUNG
// ============================================================

/** loadRecordings() - Alarm-Aufnahmen laden und anzeigen */
function loadRecordings() {
    var container = document.getElementById('recordings-container');
    if (!container) return;
    container.innerHTML = '<div style="text-align:center;padding:20px;color:var(--text-secondary);">Lade...</div>';

    fetch('api.php?action=get_recordings', {credentials: 'same-origin'})
        .then(function(r) {
            if (r.status === 403) { window.location.href = 'index.php?timeout=1'; return null; }
            return r.json();
        })
        .then(function(data) {
            if (!data) return;
            var recs = data.recordings || [];
            if (recs.length === 0) {
                container.innerHTML = '<div style="text-align:center;padding:40px;color:var(--text-secondary);">Keine Aufnahmen vorhanden</div>';
                return;
            }
            var html = '<table style="width:100%;border-collapse:collapse;">';
            html += '<tr style="border-bottom:1px solid var(--border);"><th style="text-align:left;padding:8px 12px;color:var(--text-secondary);font-size:12px;">Datei</th><th style="text-align:left;padding:8px 12px;color:var(--text-secondary);font-size:12px;">Datum</th><th style="text-align:right;padding:8px 12px;color:var(--text-secondary);font-size:12px;">Groesse</th><th style="padding:8px 12px;"></th></tr>';
            recs.forEach(function(rec) {
                html += '<tr style="border-bottom:1px solid rgba(51,65,85,0.3);">';
                html += '<td style="padding:10px 12px;font-size:13px;">' + rec.name + '</td>';
                html += '<td style="padding:10px 12px;font-size:13px;color:var(--text-secondary);">' + rec.date + '</td>';
                html += '<td style="padding:10px 12px;font-size:13px;text-align:right;">' + rec.size_mb + ' MB</td>';
                html += '<td style="padding:10px 12px;text-align:right;white-space:nowrap;">';
                html += '<a href="api.php?action=download_recording&file=' + encodeURIComponent(rec.name) + '" class="btn btn-primary" style="padding:4px 10px;font-size:12px;display:inline-block;margin-right:4px;">Download</a>';
                html += '<button onclick="deleteRecording(\'' + rec.name + '\')" class="btn btn-danger" style="padding:4px 10px;font-size:12px;">Loeschen</button>';
                html += '</td></tr>';
            });
            html += '</table>';
            container.innerHTML = html;
        })
        ['catch'](function(err) {
            container.innerHTML = '<div style="text-align:center;padding:20px;color:#ef4444;">Fehler: ' + err + '</div>';
        });

    // Alarm-Monitor Status parallel laden
    fetch('api.php?action=get_alarm_status', {credentials: 'same-origin'})
        .then(function(r) { return r.json(); })
        .then(function(data) {
            var textEl   = document.getElementById('rec-monitor-text');
            var detailEl = document.getElementById('rec-monitor-detail');
            if (!textEl) return;
            var stateMap = {
                'idle':        'Alarm-Monitor aktiv - Bereit',
                'recording':   'AUFNAHME LAEUFT',
                'stopped':     'Alarm-Monitor gestoppt',
                'error':       'Fehler',
                'not_running': 'Alarm-Monitor nicht gestartet'
            };
            textEl.textContent = stateMap[data.state] || data.state;
            if (data.current_file) {
                detailEl.textContent = 'Datei: ' + data.current_file;
            } else if (data.error) {
                detailEl.textContent = 'Fehler: ' + data.error;
            } else {
                detailEl.textContent = data.timestamp
                    ? ('Letztes Update: ' + new Date(data.timestamp).toLocaleTimeString('de-DE'))
                    : 'Warte auf Alarm-Signale...';
            }
        })
        ['catch'](function() {});
}

/**
 * deleteRecording() - Einzelne Aufnahme löschen
 * @param {string} filename - Name der zu löschenden Datei
 */
function deleteRecording(filename) {
    if (!confirm('Aufnahme "' + filename + '" wirklich loeschen?')) return;
    apiCall('delete_recording', {file: filename})
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.status === 'ok') loadRecordings();
            else alert(data.error || 'Fehler');
        })
        ['catch'](function() { alert('Fehler beim Loeschen'); });
}

/** deleteAllRecordings() - Alle Aufnahmen löschen */
function deleteAllRecordings() {
    if (!confirm('ALLE Aufnahmen wirklich loeschen?')) return;
    apiCall('delete_all_recordings')
        .then(function(r) { return r.json(); })
        .then(function(data) { alert(data.message || data.error); loadRecordings(); })
        ['catch'](function() { alert('Fehler'); });
}

// ============================================================
// ARDUINO SERIAL - Alarmanlage über serielle Schnittstelle
// ============================================================

/**
 * sendSerialCmd() - Befehl über serielle Schnittstelle an Arduino senden
 * @param {string} cmd - '1' = Alarm aktivieren, '0' = Alarm deaktivieren
 */
function sendSerialCmd(cmd) {
    var label = (cmd === '1') ? 'AKTIVIEREN (Scharfschalten)' : 'DEAKTIVIEREN (Unscharfschalten)';
    var pin = prompt('Alarm-PIN eingeben um ' + label + ':');
    if (pin === null || pin === '') return;
    console.log('[ALARM-SERIAL] Sending', cmd);
    var msgEl  = document.getElementById('alarm-serial-msg');
    var portEl = document.getElementById('alarm-serial-port');
    if (msgEl) msgEl.textContent = 'Pruefe PIN...';
    apiCall('serial_send', {cmd: cmd, alarm_pin: pin})
        .then(function(r) { return r.json(); })
        .then(function(data) {
            console.log('[ALARM-SERIAL] Response:', data);
            if (data.status === 'ok') {
                if (msgEl)  msgEl.textContent  = (cmd === '1') ? 'SCHARF' : 'UNSCHARF';
                if (portEl && data.port) portEl.textContent = data.port;
            } else {
                if (msgEl) msgEl.textContent = 'Fehler';
            }
            alert(data.message || data.error);
        })
        ['catch'](function(err) {
            console.error('[ALARM-SERIAL] Error:', err);
            if (msgEl) msgEl.textContent = 'Fehler!';
            alert('Serieller Befehl fehlgeschlagen: ' + err);
        });
}

// ============================================================
// NAVIGATION - Tab-Umschaltung
// ============================================================

/**
 * switchView() - Zwischen Dashboard/Logs/Diagnose/Config/etc. wechseln
 * @param {string} tabName - Name des Views (z.B. 'dashboard', 'logs')
 */
function switchView(tabName) {
    var views  = ['dashboard', 'logs', 'diagnose', 'config', 'userlogs', 'recordings'];
    var btnMap = { 'dashboard': 'dash', 'config': 'conf', 'logs': 'logs', 'userlogs': 'userlogs', 'diagnose': 'diag' };

    views.forEach(function(v) {
        var viewEl = document.getElementById('view-' + v);
        if (viewEl) viewEl.style.display = 'none';
        var btn = document.getElementById('btn-' + btnMap[v]);
        if (btn) btn.classList.remove('active');
    });

    var targetView = document.getElementById('view-' + tabName);
    if (targetView) targetView.style.display = 'block';
    var activeBtn = document.getElementById('btn-' + btnMap[tabName]);
    if (activeBtn) activeBtn.classList.add('active');

    try { sessionStorage.setItem('activeTab', tabName); } catch(e) {}

    if (tabName === 'userlogs')  loadUserLogsSimple();
    if (tabName === 'diagnose' && typeof initDiagnoseCharts === 'function') initDiagnoseCharts();
    if (tabName === 'recordings') loadRecordings();

    resetActivityTimer();
}

// ============================================================
// KONFIGURATION SPEICHERN
// ============================================================

/** saveConfig() - Dashboard-Einstellungen an Server senden */
function saveConfig() {
    var settings = {
        site_title:      document.getElementById('cfg-title').value,
        password:        document.getElementById('cfg-pw').value,
        alarm_pin:       document.getElementById('cfg-alarm-pin').value,
        refresh_rate:    document.getElementById('cfg-refresh').value,
        timeout_active:  document.getElementById('cfg-timeout-active').checked,
        timeout_minutes: parseInt(document.getElementById('cfg-timeout-min').value)
    };
    apiCall('save_settings', {settings: JSON.stringify(settings)})
        .then(function(r) { return r.json(); })
        .then(function(data) { alert(data.message || data.error); location.reload(); })
        ['catch'](function() { alert('Request failed'); });
}

/** sendNodeConfig() - Remote-Konfiguration an ESP-Node senden */
function sendNodeConfig() {
    var target = document.getElementById('conf-target').value;
    if (!target) { alert('No device selected!'); return; }

    var config = {};
    var api    = document.getElementById('conf-apiip').value;  if (api)    config.apiip = api;
    var mssid  = document.getElementById('conf-mssid').value;  if (mssid)  config.mssid = mssid;
    var mpass  = document.getElementById('conf-mpass').value;  if (mpass)  config.mpass = mpass;
    var bssid  = document.getElementById('conf-bssid').value;  if (bssid)  config.bssid = bssid;
    var bpass  = document.getElementById('conf-bpass').value;  if (bpass)  config.bpass = bpass;
    var telnet = document.getElementById('conf-telnet').value; if (telnet) config.tpass = telnet;

    if (Object.keys(config).length === 0) { alert('No fields filled!'); return; }
    if (!confirm('Send configuration to ' + target + '? Device will restart.')) return;

    apiCall('save_node_config', {target: target, config: JSON.stringify(config)})
        .then(function(r) { return r.json(); })
        .then(function(data) { alert(data.message || data.error); })
        ['catch'](function() { alert('Request failed'); });
}

/** resetSystem() - Alle Daten löschen (doppelte Bestätigung) */
function resetSystem() {
    if (!confirm('Delete all data?')) return;
    if (!confirm('CONFIRM: Erase all status, logs and commands?')) return;
    apiCall('system_reset')
        .then(function(r) { return r.json(); })
        .then(function(data) { alert(data.message || data.error); location.reload(); })
        ['catch'](function() { alert('Request failed'); });
}

// ============================================================
// TIMEOUT STATUS ANZEIGE
// ============================================================

/** updateTimeoutStatus() - Zeigt aktuellen Auto-Logout Status in Config */
function updateTimeoutStatus() {
    var indicator  = document.getElementById('timeout-status');
    var statusText = document.getElementById('timeout-status-text');
    var infoText   = document.getElementById('timeout-info');
    if (!indicator || !statusText || !infoText) return;

    if (timeoutActive) {
        indicator.className    = 'alert alert-warning';
        statusText.textContent = 'Auto-Logout: Enabled';
        infoText.textContent   = 'Automatic logout after ' + timeoutMinutes + ' minutes of inactivity';
    } else {
        indicator.className    = 'alert alert-info';
        statusText.textContent = 'Auto-Logout: Disabled';
        infoText.textContent   = 'No automatic logout';
    }
}

// ============================================================
// DASHBOARD UPDATE LOOP
// ============================================================

/**
 * updateDashboard() - Holt alle aktuellen Daten von der API
 * und aktualisiert Dashboard-Elemente.
 */
function updateDashboard() {
    fetch('api.php?get=all', {credentials: 'same-origin'})
        .then(function(response) {
            if (response.status === 403) { window.location.href = 'index.php?timeout=1'; return null; }
            return response.json();
        })
        .then(function(data) {
            if (!data || !data.status) return;

            updateNode('sender',   data.status.sender   || null);
            updateNode('receiver', data.status.receiver || null);

            // PiCam: Kombination aus Camera-ESP und Pi-Daten
            var camData = data.status.camera ? JSON.parse(JSON.stringify(data.status.camera)) : {};
            var piData  = data.status.pi || {};
            if (!camData.ip || camData.ip === '0.0.0.0') camData.ip = piData.ip || '---';
            if (!camData.online && piData.online) {
                camData.online = true;
                camData.status = 'Stream bereit';
            }
            updateNode('camera', camData);

            // Pi Hardware-Metriken
            var tempEl = document.getElementById('pi-cpu-temp');
            var loadEl = document.getElementById('pi-cpu-load');
            if (piData.cpu_temp !== undefined && tempEl) {
                var temp  = parseFloat(piData.cpu_temp);
                var color = temp > 70 ? '#ef4444' : (temp > 55 ? '#f59e0b' : '#10b981');
                tempEl.innerHTML = '<span style="color:' + color + '">' + temp.toFixed(1) + ' °C</span>';
            }
            if (piData.cpu_load !== undefined && loadEl) {
                loadEl.textContent = piData.cpu_load;
            }

            // Aufnahme-Status
            var recEl = document.getElementById('pi-rec-status');
            if (recEl && data.alarm_monitor) {
                var am = data.alarm_monitor;
                if (am.state === 'recording') {
                    recEl.innerHTML = '<span style="color:#ef4444;font-weight:600;">● REC</span>';
                } else if (am.state === 'idle') {
                    recEl.innerHTML = '<span style="color:#10b981;">Bereit</span>';
                } else if (am.state === 'not_running') {
                    recEl.innerHTML = '<span style="color:#94a3b8;">Inaktiv</span>';
                } else {
                    recEl.textContent = am.state || '---';
                }
            }

            // Konfiguration synchronisieren
            if (data.config) {
                timeoutActive  = data.config.timeout_active  || false;
                timeoutMinutes = data.config.timeout_minutes || 5;
                updateTimeoutStatus();
            }

            // Node Config Dropdown aktualisieren
            var targetSelect = document.getElementById('conf-target');
            if (targetSelect) {
                var currentSelection = targetSelect.value;
                var sOnline = data.status.sender   && data.status.sender.online;
                var rOnline = data.status.receiver && data.status.receiver.online;
                var newOpts  = '<option value="sender">ESP Sender'   + (sOnline ? '' : ' (OFFLINE)') + '</option>';
                newOpts     += '<option value="receiver">ESP Receiver' + (rOnline ? '' : ' (OFFLINE)') + '</option>';
                if (targetSelect.innerHTML !== newOpts) {
                    targetSelect.innerHTML = newOpts;
                    if (currentSelection) targetSelect.value = currentSelection;
                }
            }

            // Alarm Toggle synchronisieren
            var alarmSwitch = document.getElementById('alarm-toggle');
            if (alarmSwitch && data.status.receiver && data.status.receiver.alarm !== undefined && document.activeElement !== alarmSwitch) {
                alarmSwitch.checked = (data.status.receiver.alarm == true || data.status.receiver.alarm == '1');
            }

            // Live-Logs aktualisieren
            var logSender   = document.getElementById('log-sender');
            var logReceiver = document.getElementById('log-receiver');
            var logCamera   = document.getElementById('log-camera');
            if (logSender)   logSender.innerHTML   = '';
            if (logReceiver) logReceiver.innerHTML = '';
            if (logCamera)   logCamera.innerHTML   = '';

            if (data.logs) {
                data.logs.forEach(function(line) {
                    var tgt = 'log-sender';
                    if (line.indexOf('receiver:') !== -1) tgt = 'log-receiver';
                    if (line.indexOf('camera:')   !== -1) tgt = 'log-camera';
                    var div = document.createElement('div');
                    div.className   = 'log-line';
                    div.textContent = line;
                    var container = document.getElementById(tgt);
                    if (container) container.appendChild(div);
                });
                ['log-sender', 'log-receiver', 'log-camera'].forEach(function(id) {
                    var el = document.getElementById(id);
                    if (el) el.scrollTop = el.scrollHeight;
                });
            }

            // Audit-Logs aktualisieren wenn Tab aktiv
            var ulView = document.getElementById('view-userlogs');
            if (ulView && ulView.style.display !== 'none') loadUserLogsSimple();
        })
        ['catch'](function(err) { console.error('Dashboard Error:', err); });
}

/**
 * updateNode() - Einzelne Gerätekarte aktualisieren
 * @param {string} name - Gerätename ('sender', 'receiver', 'camera')
 * @param {Object} data - Statusdaten {ip, online, status, ...}
 */
function updateNode(name, data) {
    var card     = document.getElementById('card-' + name);
    var ipField  = document.getElementById('ip-' + name);
    var msgField = document.getElementById('msg-' + name);
    var dot      = document.getElementById('dot-' + name);
    if (!card) return;

    if (!data) {
        card.classList.remove('online');
        if (dot) dot.classList.remove('online');
        return;
    }
    if (ipField) ipField.innerText = data.ip || '---';
    if (data.online) {
        if (msgField) msgField.innerText = data.status || 'Online';
        card.classList.add('online');
        if (dot) dot.classList.add('online');
    } else {
        card.classList.remove('online');
        if (msgField) msgField.innerText = 'Device offline';
        if (dot) dot.classList.remove('online');
    }
}

/**
 * startLoop() - Rekursiver Dashboard-Update Loop
 */
function startLoop() {
    updateDashboard();
    updateTimer = setTimeout(startLoop, refreshRate);
}

// ============================================================
// AUDIT LOG FUNKTIONEN
// ============================================================

/** loadUserLogsSimple() - Audit-Logs per AJAX laden und rendern */
function loadUserLogsSimple() {
    var container = document.getElementById('userlog-container');
    if (!container) return;
    fetch('api.php?action=get_user_logs', {credentials: 'same-origin'})
        .then(function(r) { return r.text(); })
        .then(function(text) {
            var logs;
            try { logs = JSON.parse(text); } catch(e) {
                container.innerHTML = '<div style="text-align:center;padding:40px;color:var(--accent-red);">Error parsing log data</div>';
                return;
            }
            if (!logs || logs.length === 0) {
                container.innerHTML = '<div style="text-align:center;padding:40px;color:var(--text-secondary);">No activity logged</div>';
                return;
            }
            var html = '';
            logs.slice(0, 20).forEach(function(log) {
                html += '<div class="log-entry">' +
                    '<div class="log-entry-header">' +
                        '<div class="log-action" style="display:flex;align-items:center;gap:8px;">' +
                            '<span>' + (log.action || '') + '</span>' +
                        '</div>' +
                        '<span class="log-time">' + (log.date || '') + '</span>' +
                    '</div>' +
                    '<div class="log-details">' +
                        'IP: ' + (log.ip || '') + ' | Device: ' + (log.device_name || '') +
                        (log.details ? '<br><span style="color:var(--accent-yellow);">&rarr; ' + log.details + '</span>' : '') +
                    '</div></div>';
            });
            container.innerHTML = html;
        })
        ['catch'](function() {
            container.innerHTML = '<div style="text-align:center;padding:40px;color:var(--accent-red);">Error loading logs</div>';
        });
}

/** exportUserLogs() - Audit-Logs als Datei herunterladen */
function exportUserLogs() { window.location.href = 'api.php?action=export_user_logs'; }

/** clearUserLogs() - Alle Audit-Logs löschen */
function clearUserLogs() {
    if (!confirm('Delete all audit logs?')) return;
    apiCall('clear_user_logs')
        .then(function(r) { return r.json(); })
        .then(function(data) { alert(data.message || data.error); loadUserLogsSimple(); })
        ['catch'](function() { alert('Request failed'); });
}

// ============================================================
// INITIALISIERUNG
// ============================================================
updateTimeoutStatus();

// Gespeicherten Tab wiederherstellen (nach Seiten-Reload)
try {
    var savedTab = sessionStorage.getItem('activeTab');
    if (savedTab && savedTab !== 'dashboard') switchView(savedTab);
} catch(e) {}

// Dashboard-Update Loop starten
startLoop();

// Lucide Icons initialisieren
try { if (typeof lucide !== 'undefined') lucide.createIcons(); } catch(e) {}

console.log('[INIT] All critical functions loaded OK. CSRF token:', CSRF_TOKEN ? 'present (' + CSRF_TOKEN.length + ' chars)' : 'MISSING!');
