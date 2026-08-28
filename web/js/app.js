// ============================================================
// js/app.js - Kritische Dashboard-Funktionen
// ============================================================
// Enthält: CSRF-Setup, API-Helper, Dashboard-Updates,
// Navigation, Gerätesteuerung, Konfiguration, Logs,
// Aufnahmen-Verwaltung, lokale Alarmsteuerung.
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
function redactForLog(value, key) {
    if (/(pass(word)?|pin|token|secret|csrf|credential|authorization)/i.test(key || '')) {
        return '[REDACTED]';
    }
    if (typeof value === 'string' && /^[\[{]/.test(value.trim())) {
        try { return redactForLog(JSON.parse(value), key); } catch (e) { return '[STRUCTURED DATA]'; }
    }
    if (Array.isArray(value)) {
        return value.map(function(item) { return redactForLog(item, ''); });
    }
    if (value && typeof value === 'object') {
        var clean = {};
        Object.keys(value).forEach(function(childKey) {
            clean[childKey] = redactForLog(value[childKey], childKey);
        });
        return clean;
    }
    return value;
}

function apiCall(action, params) {
    if (!params) params = {};
    params.csrf_token = CSRF_TOKEN;
    // Rekursiv filtern: settings/config enthalten verschachtelte Credentials
    // als JSON-String und dürfen nicht über die Browserkonsole abfließen.
    var logParams = redactForLog(params, '');
    console.log('[API]', action, logParams);
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
var bootstrapAlarmPinPending = false;

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
// PIN MODAL - Sicheres Passwort-Eingabe-Dialog
// ============================================================

/** PIN-Transport ist ausschliesslich in einem sicheren HTTPS-Kontext erlaubt. */
var _securePinTransportAvailable = !!window.isSecureContext && window.location.protocol === 'https:';

/**
 * preparePin() sendet den PIN nur innerhalb der TLS-geschuetzten Sitzung.
 * Der Server speichert und prueft ausschliesslich einen bcrypt-Hash.
 */
function preparePin(pin) {
    if (!_securePinTransportAvailable) {
        return Promise.reject('Alarm-PINs können nur über HTTPS gesendet werden.');
    }
    return Promise.resolve({ alarm_pin: pin });
}

// Interner Callback-Slot für das aktive Modal
// Signatur: callback(pin, resolveFn, rejectFn)
var _pinModalCallback = null;

/**
 * showPinModal() - Zeigt das zentrale PIN-Eingabe-Modal
 *
 * @param {string}   title    - Modalüberschrift
 * @param {string}   desc     - Kurzbeschreibung der angeforderten Aktion
 * @param {Function} callback - Wird mit (pin, resolve, reject) aufgerufen.
 *                              resolve() schließt das Modal bei Erfolg.
 *                              reject(message) zeigt den Fehler im Modal.
 */
function showPinModal(title, desc, callback) {
    _pinModalCallback = callback;

    document.getElementById('pin-modal-title').textContent    = title;
    document.getElementById('pin-modal-desc').textContent     = desc || '';
    document.getElementById('pin-modal-input').value          = '';
    document.getElementById('pin-modal-input').type           = 'password';
    document.getElementById('pin-modal-error').style.display  = 'none';
    document.getElementById('pin-modal-error').textContent    = '';
    document.getElementById('pin-modal-confirm').disabled     = false;
    document.getElementById('pin-modal-confirm').textContent  = 'Bestätigen';

    // Augen-Icon: Startzustand = Passwort verborgen (Auge auf)
    document.getElementById('pin-eye-icon').style.display     = 'block';
    document.getElementById('pin-eye-off-icon').style.display = 'none';

    // HTTPS-Warnung einblenden, wenn PIN-Transport blockiert ist.
    var warnEl = document.getElementById('pin-modal-warning');
    if (warnEl) warnEl.style.display = _securePinTransportAvailable ? 'none' : 'block';

    document.getElementById('pin-modal-overlay').classList.add('active');

    // Fokus mit kleiner Verzögerung setzen (Modal muss sichtbar sein)
    setTimeout(function() { document.getElementById('pin-modal-input').focus(); }, 50);
}

/** hidePinModal() - Schließt das Modal und bereinigt den Zustand */
function hidePinModal() {
    document.getElementById('pin-modal-overlay').classList.remove('active');
    document.getElementById('pin-modal-input').value = '';
    _pinModalCallback = null;
}

/**
 * togglePinVisibility() - Wechselt zwischen Sternchen und Klartext im PIN-Feld
 * Wird vom Auge-Icon-Button ausgelöst (data-click="togglePinVisibility").
 */
function togglePinVisibility() {
    var input  = document.getElementById('pin-modal-input');
    var eyeOn  = document.getElementById('pin-eye-icon');
    var eyeOff = document.getElementById('pin-eye-off-icon');

    if (input.type === 'password') {
        // → Klartext anzeigen
        input.type           = 'text';
        eyeOn.style.display  = 'none';
        eyeOff.style.display = 'block';
    } else {
        // → Passwort verbergen
        input.type           = 'password';
        eyeOn.style.display  = 'block';
        eyeOff.style.display = 'none';
    }
    input.focus();
}

/** pinModalCancel() - Modal schließen ohne Aktion */
function pinModalCancel() {
    hidePinModal();
}

/**
 * pinModalConfirm() - PIN-Eingabe bestätigen
 * Sperrt den Confirm-Button während der Verifikation läuft.
 * Der Callback entscheidet über resolve() oder reject(message).
 */
function pinModalConfirm() {
    var pin = document.getElementById('pin-modal-input').value;
    if (!pin) {
        document.getElementById('pin-modal-error').textContent  = 'PIN darf nicht leer sein.';
        document.getElementById('pin-modal-error').style.display = 'block';
        document.getElementById('pin-modal-input').focus();
        return;
    }
    if (typeof _pinModalCallback !== 'function') { hidePinModal(); return; }

    // Confirm-Button sperren während die Verifikation läuft
    var confirmBtn = document.getElementById('pin-modal-confirm');
    confirmBtn.disabled    = true;
    confirmBtn.textContent = '...';

    _pinModalCallback(
        pin,
        // resolve() – Erfolg: Modal schließen
        function() { hidePinModal(); },
        // reject(msg) – Fehler: Fehlermeldung anzeigen, Modal offen lassen
        function(errMsg) {
            document.getElementById('pin-modal-error').textContent  = errMsg || 'Falscher PIN!';
            document.getElementById('pin-modal-error').style.display = 'block';
            document.getElementById('pin-modal-input').value         = '';
            document.getElementById('pin-modal-input').focus();
            confirmBtn.disabled    = false;
            confirmBtn.textContent = 'Bestätigen';
        }
    );
}

// ============================================================
// GERÄTESTEUERUNG - ESP Befehle senden
// ============================================================

/**
 * sendCommand() - Befehl an ESP-Node senden
 *
 * REBOOT und jede Receiver-Alarmänderung erfordern PIN-Bestätigung. Der
 * Factory-Reset bleibt ausschließlich am physischen Gerät verfügbar.
 *
 * @param {string} target - 'sender' oder 'receiver'
 * @param {string} cmd    - Befehl: 'REBOOT', 'ALARM_ON' oder 'ALARM_OFF'
 */
function sendCommand(target, cmd) {
    var needsPin = (cmd === 'REBOOT') ||
                   (target === 'receiver' && (cmd === 'ALARM_ON' || cmd === 'ALARM_OFF'));

    if (needsPin) {
        showPinModal(
            'Gerätesteuerung – PIN erforderlich',
            cmd + ' auf ' + target + ' ausführen',
            function(pin, resolve, reject) {
                preparePin(pin).then(function(pinParams) {
                    apiCall('send_command', Object.assign({target: target, cmd: cmd}, pinParams))
                        .then(function(r) { return r.json(); })
                        .then(function(data) {
                            if (data.status === 'ok') {
                                resolve();
                                alert(data.message || 'Befehl gesendet.');
                            } else {
                                reject(data.error || 'Fehler beim Senden.');
                            }
                        })['catch'](function() { reject('Verbindungsfehler'); });
                })['catch'](function(err) { reject(String(err || 'PIN-Transport fehlgeschlagen')); });
            }
        );
    } else {
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
}

/**
 * toggleAlarm() - Alarm über ESP Receiver ein/ausschalten
 * @param {HTMLElement} checkbox - Alarm Toggle Checkbox
 */
function toggleAlarm(checkbox) {
    var cmd = checkbox.checked ? 'ALARM_ON' : 'ALARM_OFF';
    var requestedState = checkbox.checked;
    checkbox.checked = !requestedState;
    showPinModal(
        'Alarmsteuerung – PIN erforderlich',
        cmd === 'ALARM_ON' ? 'Empfängeralarm aktivieren' : 'Empfängeralarm deaktivieren',
        function(pin, resolve, reject) {
            preparePin(pin).then(function(pinParams) {
                return apiCall('send_command', Object.assign({target: 'receiver', cmd: cmd}, pinParams));
            }).then(function(r) { return r.json(); })
              .then(function(data) {
                  if (data.status !== 'ok') { reject(data.error || 'Alarmbefehl abgelehnt.'); return; }
                  checkbox.checked = requestedState;
                  resolve();
              })['catch'](function(err) { reject(String(err || 'Verbindungsfehler')); });
        }
    );
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
            // Tabelle per DOM-API aufbauen (kein onclick-Inline-Handler → CSP-kompatibel)
            var table = document.createElement('table');
            table.style.cssText = 'width:100%;border-collapse:collapse;';

            var thead = document.createElement('tr');
            thead.style.borderBottom = '1px solid var(--border-color)';
            ['Datei','Datum','Groesse',''].forEach(function(t) {
                var th = document.createElement('th');
                th.style.cssText = 'text-align:left;padding:8px 12px;color:var(--text-secondary);font-size:12px;';
                th.textContent = t;
                thead.appendChild(th);
            });
            table.appendChild(thead);

            recs.forEach(function(rec) {
                var tr = document.createElement('tr');
                tr.style.borderBottom = '1px solid rgba(51,65,85,0.3)';

                var tdName = document.createElement('td');
                tdName.style.cssText = 'padding:10px 12px;font-size:13px;';
                tdName.textContent = rec.name;

                var tdDate = document.createElement('td');
                tdDate.style.cssText = 'padding:10px 12px;font-size:13px;color:var(--text-secondary);';
                tdDate.textContent = rec.date;

                var tdSize = document.createElement('td');
                tdSize.style.cssText = 'padding:10px 12px;font-size:13px;text-align:right;';
                tdSize.textContent = rec.size_mb + ' MB';

                var tdBtns = document.createElement('td');
                tdBtns.style.cssText = 'padding:10px 12px;text-align:right;white-space:nowrap;';

                var dlLink = document.createElement('a');
                dlLink.href = 'api.php?action=download_recording&file=' + encodeURIComponent(rec.name);
                dlLink.className = 'btn btn-primary';
                dlLink.style.cssText = 'padding:4px 10px;font-size:12px;display:inline-block;margin-right:4px;';
                dlLink.textContent = 'Download';

                // data-click statt onclick → wird vom zentralen Dispatcher verarbeitet
                var delBtn = document.createElement('button');
                delBtn.className = 'btn btn-danger';
                delBtn.style.cssText = 'padding:4px 10px;font-size:12px;';
                delBtn.textContent = 'Loeschen';
                delBtn.dataset.click = 'deleteRecording';
                delBtn.dataset.filename = rec.name;

                tdBtns.appendChild(dlLink);
                tdBtns.appendChild(delBtn);

                tr.appendChild(tdName);
                tr.appendChild(tdDate);
                tr.appendChild(tdSize);
                tr.appendChild(tdBtns);
                table.appendChild(tr);
            });

            container.innerHTML = '';
            container.appendChild(table);
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
    if (!confirm('Alle inaktiven Aufnahmen wirklich loeschen?')) return;
    apiCall('delete_all_recordings')
        .then(function(r) { return r.json(); })
        .then(function(data) { alert(data.message || data.error); loadRecordings(); })
        ['catch'](function() { alert('Fehler'); });
}

// ============================================================
// ALARMSTEUERUNG - Exklusiv über den lokalen Alarm-Monitor
// ============================================================

/**
 * setAlarmState() - Alarmzustand über den lokalen Steuerdienst setzen
 *
 * PIN wird über showPinModal() abgefragt (type="password" → Sternchen,
 * Toggle-Icon zum Einblenden). preparePin() erzwingt sicheren HTTPS-Transport.
 *
 * @param {string} cmd - '1' = Alarm aktivieren, '0' = Alarm deaktivieren
 */
function setAlarmState(cmd) {
    var label = (cmd === '1') ? 'AKTIVIEREN (Scharfschalten)' : 'DEAKTIVIEREN (Unscharfschalten)';

    showPinModal(
        'Alarm ' + label,
        'PIN eingeben um fortzufahren',
        function(pin, resolve, reject) {
            var msgEl  = document.getElementById('alarm-control-msg');
            var portEl = document.getElementById('alarm-control-interface');
            if (msgEl) msgEl.textContent = 'Prüfe PIN...';

            preparePin(pin).then(function(pinParams) {
                apiCall('set_alarm_state', Object.assign({cmd: cmd}, pinParams))
                    .then(function(r) { return r.json(); })
                    .then(function(data) {
                        if (data.status === 'ok') {
                            resolve();
                            if (msgEl)  msgEl.textContent  = (cmd === '1') ? 'SCHARF' : 'UNSCHARF';
                            if (portEl) portEl.textContent = 'Alarm-Monitor';
                            alert(data.message || 'Alarm ' + label);
                        } else {
                            if (msgEl) msgEl.textContent = 'Fehler';
                            reject(data.error || 'Alarmzustand konnte nicht gesetzt werden');
                        }
                    })['catch'](function(err) {
                        if (msgEl) msgEl.textContent = 'Fehler!';
                        reject('Steuerbefehl fehlgeschlagen: ' + err);
                    });
            })['catch'](function(err) { reject(String(err || 'PIN-Transport fehlgeschlagen')); });
        }
    );
}

// ============================================================
// MANUELLE KAMERAAUFNAHME
// ============================================================

/**
 * setRecDot() - Aufnahme-Statusindikator in der Alarmsteuerung-Karte aktualisieren.
 *
 * Wird von updateDashboard() (Poll-getriggert) UND direkt nach erfolgreichem
 * API-Call aufgerufen, damit der Dot sofort reagiert ohne auf den nächsten Poll
 * warten zu müssen.
 *
 * @param {boolean} isRecording - true = rot + blinkt, false = grau
 */
function setRecDot(isRecording) {
    var dot  = document.getElementById('alarm-rec-dot');
    var text = document.getElementById('alarm-rec-text');
    if (!dot || !text) return;

    if (isRecording) {
        dot.classList.add('rec-dot-active');   // CSS: rot + pulse-Animation
        text.textContent    = 'LÄUFT';
        text.style.color      = '#ef4444';
        text.style.fontWeight = '600';
    } else {
        dot.classList.remove('rec-dot-active'); // CSS: grau, keine Animation
        text.textContent    = 'Inaktiv';
        text.style.color      = '';
        text.style.fontWeight = '';
    }
}

/**
 * startManualRecord() - Manuelle Aufnahme starten (PIN-geschützt).
 *
 * Sendet alarm_pin + csrf_token an api.php?action=start_manual_recording.
 * Folgt dem gleichen PIN-/CSRF-Muster wie setAlarmState().
 */
function startManualRecord() {
    showPinModal(
        'Manuelle Aufnahme starten',
        'PIN eingeben um Aufnahme zu starten',
        function(pin, resolve, reject) {
            preparePin(pin).then(function(pinParams) {
                apiCall('start_manual_recording', pinParams)
                    .then(function(r) { return r.json(); })
                    .then(function(data) {
                        if (data.status === 'ok') {
                            resolve();
                            setRecDot(data.recording === true);
                        } else {
                            reject(data.error || 'Aufnahme konnte nicht gestartet werden');
                        }
                    })['catch'](function(e) { reject('Fehler: ' + e); });
            })['catch'](function(err) { reject(String(err || 'PIN-Transport fehlgeschlagen')); });
        }
    );
}

/**
 * stopManualRecord() - Laufende manuelle Aufnahme stoppen (PIN-geschützt).
 *
 * Der lokale Alarm-Monitor beendet die manuelle Anforderung und besitzt den Recorder exklusiv.
 */
function stopManualRecord() {
    showPinModal(
        'Aufnahme stoppen',
        'PIN eingeben um Aufnahme zu stoppen',
        function(pin, resolve, reject) {
            preparePin(pin).then(function(pinParams) {
                apiCall('stop_manual_recording', pinParams)
                    .then(function(r) { return r.json(); })
                    .then(function(data) {
                        if (data.status === 'ok') {
                            resolve();
                            setRecDot(data.recording === true);
                        } else {
                            reject(data.error || 'Stoppen fehlgeschlagen');
                        }
                    })['catch'](function(e) { reject('Fehler: ' + e); });
            })['catch'](function(err) { reject(String(err || 'PIN-Transport fehlgeschlagen')); });
        }
    );
}

// ============================================================
// NAVIGATION - Tab-Umschaltung
// ============================================================

/**
 * switchView() - Zwischen Dashboard/Logs/Diagnose/Config/etc. wechseln
 *
 * Der Admin-Bereich ('userlogs') ist zusätzlich PIN-geschützt:
 * Der PIN wird gegen die API verifiziert bevor der View erscheint.
 * Alle anderen Views wechseln direkt ohne PIN-Abfrage.
 *
 * @param {string} tabName - Name des Views (z.B. 'dashboard', 'logs')
 */
function switchView(tabName) {
    // Admin-Bereich: zusätzlicher PIN-Schutz (serverseitige Verifikation)
    if (tabName === 'userlogs') {
        showPinModal(
            'Admin-Bereich',
            'PIN eingeben um Audit-Logs zu öffnen',
            function(pin, resolve, reject) {
                preparePin(pin).then(function(pinParams) {
                    apiCall('verify_pin', pinParams)
                        .then(function(r) { return r.json(); })
                        .then(function(data) {
                            if (data.status === 'ok') {
                                resolve();
                                _doSwitchView('userlogs');
                            } else {
                                reject(data.error || 'Falscher PIN!');
                            }
                        })['catch'](function() { reject('Verbindungsfehler'); });
                })['catch'](function(err) { reject(String(err || 'PIN-Transport fehlgeschlagen')); });
            }
        );
        return; // Tatsächlicher View-Wechsel passiert nach PIN-Verifikation
    }

    _doSwitchView(tabName);
}

/**
 * _doSwitchView() - Führt den eigentlichen View-Wechsel durch.
 * Wird von switchView() direkt oder nach PIN-Verifikation aufgerufen.
 * @param {string} tabName
 */
function _doSwitchView(tabName) {
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

/** saveConfig() - Dashboard-Einstellungen an Server senden.
 *  Wenn der Titel auf "admin" gesetzt wird, ist vorher eine PIN-Verifikation nötig. */
function saveConfig() {
    var newTitle      = document.getElementById('cfg-title').value.trim();
    // Alle Credential-Felder vorab auslesen – beide Branches nutzen dieselben Werte
    var newPw         = document.getElementById('cfg-pw').value;
    var currentPw     = document.getElementById('cfg-current-pw').value;
    var newPin        = document.getElementById('cfg-alarm-pin').value;
    var currentPinRaw = document.getElementById('cfg-current-alarm-pin').value;

    function _doSave(extra) {
        // Client-seitige Pflichtfeld-Prüfung: Änderung ohne Bestätigung verhindern
        if (newPw && !currentPw) {
            alert('Bitte das aktuelle Passwort zur Bestätigung eingeben.');
            return;
        }
        if (newPin && !currentPinRaw && !bootstrapAlarmPinPending) {
            alert('Bitte den aktuellen Alarm-PIN zur Bestätigung eingeben.');
            return;
        }

        var settings = {
            site_title:      newTitle,
            password:        newPw,
            alarm_pin:       newPin,
            refresh_rate:    document.getElementById('cfg-refresh').value,
            timeout_active:  document.getElementById('cfg-timeout-active').checked,
            timeout_minutes: parseInt(document.getElementById('cfg-timeout-min').value)
        };
        // Optionale PIN-Felder für Admin-Titel-Verifikation auf dem Server
        if (extra) {
            settings.admin_title_pin = extra.alarm_pin;
        }
        // Aktuelles Passwort im Klartext mitsenden – Server nutzt password_verify()
        if (newPw) settings.current_password = currentPw;

        // Aktuellen PIN nur ueber die HTTPS-geschuetzte Sitzung uebertragen.
        var pin_promise = (newPin && currentPinRaw && !bootstrapAlarmPinPending)
            ? preparePin(currentPinRaw)
            : Promise.resolve(null);

        pin_promise.then(function(prepared) {
            if (prepared) {
                settings.current_alarm_pin = prepared.alarm_pin;
            }
            apiCall('save_settings', {settings: JSON.stringify(settings)})
                .then(function(r) { return r.json(); })
                .then(function(data) { alert(data.message || data.error); location.reload(); })
                ['catch'](function() { alert('Request failed'); });
        })['catch'](function() { alert('PIN-Vorbereitung fehlgeschlagen.'); });
    }

    // Admin-Titel setzt erweiterte Rechte → PIN verlangen
    if (newTitle.toLowerCase() === 'admin') {
        showPinModal(
            'Admin-Titel setzen',
            'Das Setzen des Titels auf "admin" aktiviert den Admin-Modus. Bitte PIN bestätigen.',
            function(pin, resolve, reject) {
                preparePin(pin).then(function(prepared) {
                    // Settings-Objekt direkt hier aufbauen (inkl. PIN-Felder)
                    var settings = {
                        site_title:              newTitle,
                        password:                newPw,
                        alarm_pin:               newPin,
                        refresh_rate:            document.getElementById('cfg-refresh').value,
                        timeout_active:          document.getElementById('cfg-timeout-active').checked,
                        timeout_minutes:         parseInt(document.getElementById('cfg-timeout-min').value),
                        admin_title_pin:         prepared.alarm_pin
                    };
                    // Aktuelles Passwort ebenfalls weitergeben (falls gleichzeitig geändert)
                    if (newPw) settings.current_password = currentPw;
                    // Der im Modal eingegebene PIN ist der aktuelle PIN des Users.
                    // Er dient gleichzeitig als current_alarm_pin, wenn der PIN geändert wird.
                    if (newPin) {
                        settings.current_alarm_pin = prepared.alarm_pin;
                    }
                    apiCall('save_settings', {settings: JSON.stringify(settings)})
                        .then(function(r) { return r.json(); })
                        .then(function(data) {
                            if (data.error) {
                                // Server hat PIN abgelehnt → Fehler im Modal anzeigen
                                reject(data.error);
                            } else {
                                // Erfolg → Modal schließen, Seite neu laden
                                resolve();
                                location.reload();
                            }
                        })
                        ['catch'](function() {
                            reject('Verbindungsfehler. Bitte erneut versuchen.');
                        });
                })['catch'](reject);
            }
        );
    } else {
        _doSave(null);
    }
}

/** sendNodeConfig() - ESP-Knoten-Konfiguration senden */
function sendNodeConfig() {
    var target = document.getElementById('conf-target').value;
    if (!target) { alert('No device selected!'); return; }

    var config = {};
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

/** resetSystem() - Alle Daten löschen (doppelte Bestätigung + PIN) */
function resetSystem() {
    if (!confirm('Delete all data?')) return;
    if (!confirm('CONFIRM: Erase all status, logs and commands?')) return;
    showPinModal(
        'System Reset – PIN erforderlich',
        'Alle Status-, Log- und Befehlsdaten werden unwiderruflich gelöscht',
        function(pin, resolve, reject) {
            preparePin(pin).then(function(pinParams) {
                apiCall('system_reset', pinParams)
                    .then(function(r) { return r.json(); })
                    .then(function(data) {
                        if (data.status === 'ok') {
                            resolve();
                            alert(data.message);
                            location.reload();
                        } else {
                            reject(data.error || 'Fehler');
                        }
                    })['catch'](function() { reject('Verbindungsfehler'); });
            })['catch'](function(err) { reject(String(err || 'PIN-Transport fehlgeschlagen')); });
        }
    );
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
    return fetch('api.php?get=all', {credentials: 'same-origin'})
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

            // Verbindungsstatus kommt ausschließlich aus alarm_monitor.json.
            var alarmCard = document.getElementById('card-alarm');
            var alarmStatus = document.getElementById('alarm-control-status');
            var alarmInterface = document.getElementById('alarm-control-interface');
            if (alarmCard && data.alarm_monitor) {
                var unoConnected = data.alarm_monitor.serial_connected === true;
                alarmCard.classList.toggle('online', unoConnected);
                if (alarmStatus) alarmStatus.textContent = unoConnected ? 'Uno verbunden' : 'Uno getrennt';
                if (alarmInterface) {
                    alarmInterface.textContent = unoConnected
                        ? (data.alarm_monitor.serial_port || 'Alarm-Monitor')
                        : 'Lokaler Steuerdienst';
                }
            }

            // alarm_monitor.json ist die einzige Statusquelle für Aufnahmen.
            var isRecording = data.alarm_monitor && data.alarm_monitor.state === 'recording';
            setRecDot(isRecording);

            // Konfiguration synchronisieren
            if (data.config) {
                timeoutActive  = data.config.timeout_active  || false;
                timeoutMinutes = data.config.timeout_minutes || 5;
                bootstrapAlarmPinPending = data.config.bootstrap_pin_pending === true;
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
    // Erst nach Abschluss (auch bei Fehlern) neu planen. Langsame Antworten
    // erzeugen dadurch keine ueberlappenden Polling-Kaskaden.
    Promise.resolve(updateDashboard())
        ['catch'](function(err) { console.error('Dashboard loop error:', err); })
        ['finally'](function() { updateTimer = setTimeout(startLoop, refreshRate); });
}

// ============================================================
// AUDIT LOG FUNKTIONEN
// ============================================================

/**
 * safeEl() - Erstellt ein DOM-Element mit sicherem textContent (kein XSS möglich)
 * @param {string} tag   - HTML-Tag-Name
 * @param {string} text  - Textinhalt (wird als textContent gesetzt, nie als HTML geparst)
 * @param {string} cls   - Optionale CSS-Klasse
 * @param {string} style - Optionaler Inline-Style
 */
function safeEl(tag, text, cls, style) {
    var el = document.createElement(tag);
    if (text  !== undefined && text  !== null) el.textContent = text;
    if (cls)   el.className = cls;
    if (style) el.style.cssText = style;
    return el;
}

/** loadUserLogsSimple() - Audit-Logs per AJAX laden und rendern (XSS-sicher via DOM-API) */
function loadUserLogsSimple() {
    var container = document.getElementById('userlog-container');
    if (!container) return;
    fetch('api.php?action=get_user_logs', {credentials: 'same-origin'})
        .then(function(r) { return r.text(); })
        .then(function(text) {
            var logs;
            try { logs = JSON.parse(text); } catch(e) {
                container.innerHTML = '';
                container.appendChild(safeEl('div', 'Error parsing log data',
                    '', 'text-align:center;padding:40px;color:var(--accent-red);'));
                return;
            }
            if (!logs || logs.length === 0) {
                container.innerHTML = '';
                container.appendChild(safeEl('div', 'No activity logged',
                    '', 'text-align:center;padding:40px;color:var(--text-secondary);'));
                return;
            }

            // Alle Einträge als sichere DOM-Knoten aufbauen (KEIN innerHTML mit Nutzerdaten)
            var fragment = document.createDocumentFragment();
            logs.slice(0, 20).forEach(function(log) {
                var entry = document.createElement('div');
                entry.className = 'log-entry';

                // --- Header-Zeile: Aktion + Zeitstempel ---
                var header = document.createElement('div');
                header.className = 'log-entry-header';

                var actionDiv = document.createElement('div');
                actionDiv.className = 'log-action';
                actionDiv.style.cssText = 'display:flex;align-items:center;gap:8px;';
                actionDiv.appendChild(safeEl('span', log.action || ''));

                header.appendChild(actionDiv);
                header.appendChild(safeEl('span', log.date || '', 'log-time'));

                // --- Detail-Zeile: IP | Gerät (OS) | Browser | Details ---
                var detailsDiv = document.createElement('div');
                detailsDiv.className = 'log-details';

                // Zeile 1: IP + Gerätename + OS (alle als Klartext, XSS-sicher via createTextNode)
                var deviceText = (log.device_name || 'Unbekannt');
                detailsDiv.appendChild(document.createTextNode(
                    'IP: ' + (log.ip || '') + ' | Gerät: ' + deviceText
                ));

                // Zeile 2: Browser (nur anzeigen wenn vorhanden)
                if (log.browser) {
                    detailsDiv.appendChild(document.createElement('br'));
                    detailsDiv.appendChild(document.createTextNode(
                        'Browser: ' + log.browser
                    ));
                }

                if (log.details) {
                    detailsDiv.appendChild(document.createElement('br'));
                    var detailSpan = safeEl('span', '\u2192 ' + log.details,
                        '', 'color:var(--accent-yellow);');
                    detailsDiv.appendChild(detailSpan);
                }

                entry.appendChild(header);
                entry.appendChild(detailsDiv);
                fragment.appendChild(entry);
            });

            container.innerHTML = '';
            container.appendChild(fragment);
        })
        ['catch'](function() {
            container.innerHTML = '';
            container.appendChild(safeEl('div', 'Error loading logs',
                '', 'text-align:center;padding:40px;color:var(--accent-red);'));
        });
}

/** exportUserLogs() - Audit-Logs als Datei herunterladen */
function exportUserLogs() { window.location.href = 'api.php?action=export_user_logs'; }

/** clearUserLogs() - Alle Audit-Logs löschen */
function clearUserLogs() {
    if (!confirm('Delete all audit logs?')) return;
    showPinModal(
        'Audit-Log löschen – PIN erforderlich',
        'Alle bisherigen Audit-Einträge werden entfernt.',
        function(pin, resolve, reject) {
            preparePin(pin).then(function(pinParams) {
                return apiCall('clear_user_logs', pinParams);
            }).then(function(r) { return r.json(); })
              .then(function(data) {
                  if (data.status !== 'ok') { reject(data.error || 'Löschen fehlgeschlagen.'); return; }
                  resolve();
                  loadUserLogsSimple();
              })['catch'](function(err) { reject(String(err || 'Request failed')); });
        }
    );
}

// ============================================================
// INITIALISIERUNG
// ============================================================
updateTimeoutStatus();

// Gespeicherten Tab wiederherstellen (nach Seiten-Reload)
try {
    var savedTab = sessionStorage.getItem('activeTab');
    // 'userlogs' nicht automatisch wiederherstellen – erfordert erneuten PIN
    if (savedTab && savedTab !== 'dashboard' && savedTab !== 'userlogs') _doSwitchView(savedTab);
} catch(e) {}

// Dashboard-Update Loop starten
startLoop();

// Lucide Icons initialisieren
try { if (typeof lucide !== 'undefined') lucide.createIcons(); } catch(e) {}

// Enter-Taste bestätigt das Modal, Escape schließt es
document.addEventListener('keydown', function(e) {
    var overlay = document.getElementById('pin-modal-overlay');
    if (!overlay || !overlay.classList.contains('active')) return;
    if (e.key === 'Enter')  { e.preventDefault(); pinModalConfirm(); }
    if (e.key === 'Escape') { e.preventDefault(); pinModalCancel(); }
});

// ============================================================
// ZENTRALER EVENT-DISPATCHER (CSP-kompatibel)
// Ersetzt alle onclick="..." / onchange="..." Inline-Handler im HTML.
// Buttons/Checkboxen tragen data-click="..." / data-change="..."
// Attribute – der Dispatcher leitet sie an die jeweilige Funktion weiter.
// ============================================================
document.addEventListener('click', function(e) {
    var el = e.target.closest('[data-click]');
    if (!el || el.disabled) return;
    var d = el.dataset;
    switch (d.click) {
        // Navigation
        case 'switchView':          switchView(d.arg); break;
        // Gerätesteuerung
        case 'sendCommand':         sendCommand(d.target, d.cmd); break;
        case 'openCameraStream':    openCameraStream(); break;
        case 'setAlarmState':       setAlarmState(d.cmd); break;
        // Manuelle Kameraaufnahme
        case 'startManualRecord':   startManualRecord(); break;
        case 'stopManualRecord':    stopManualRecord();  break;
        // Logs
        case 'clearLog':            clearLog(d.arg); break;
        case 'clearTelemetry':      clearTelemetry(); break;
        case 'clearAllLogs':        clearAllLogs(); break;
        // Konfiguration
        case 'saveConfig':          saveConfig(); break;
        case 'resetSystem':         resetSystem(); break;
        case 'sendNodeConfig':      sendNodeConfig(); break;
        // Aufnahmen
        case 'loadRecordings':      loadRecordings(); break;
        case 'deleteAllRecordings': deleteAllRecordings(); break;
        case 'deleteRecording':     deleteRecording(d.filename); break;
        // Audit
        case 'exportUserLogs':      exportUserLogs(); break;
        case 'clearUserLogs':       clearUserLogs(); break;
        // PIN Modal
        case 'togglePinVisibility': togglePinVisibility(); break;
        case 'pinModalCancel':      pinModalCancel(); break;
        case 'pinModalConfirm':     pinModalConfirm(); break;
    }
});

document.addEventListener('change', function(e) {
    var el = e.target.closest('[data-change]');
    if (!el) return;
    if (el.dataset.change === 'toggleAlarm') toggleAlarm(el);
});

console.log(
    '[INIT] All critical functions loaded OK. CSRF token:',
    CSRF_TOKEN ? 'present (' + CSRF_TOKEN.length + ' chars)' : 'MISSING!',
    '| PIN transport:', _securePinTransportAvailable ? 'HTTPS' : 'BLOCKED (HTTPS required)'
);
