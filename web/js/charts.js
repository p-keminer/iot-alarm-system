// ============================================================
// js/charts.js - Telemetrie-Charts (Chart.js)
// ============================================================
// Isoliert vom Rest des Dashboards damit Chart-Fehler
// KEINE kritischen Funktionen (Buttons, Navigation) brechen.
//
// Erwartet die globale Variable `chartData` die von index.php
// als inline <script>-Block gesetzt wird.
// ============================================================

// Chart-Instanzen (global für Destroy/Recreate)
var rssiChart = null, heapChart = null;

/**
 * initDiagnoseCharts() - Telemetrie-Charts initialisieren
 *
 * Erstellt zwei Chart.js Line-Charts:
 * 1. RSSI Chart: WLAN-Signalstärke über Zeit (alle 3 Geräte)
 * 2. Heap Chart: Verfügbarer RAM über Zeit (alle 3 Geräte)
 *
 * Verwendet eine gemeinsame Zeitachse (Union aller Timestamps).
 * Fehlende Datenpunkte werden als null dargestellt (Lücken).
 */
function initDiagnoseCharts() {
    try {
        if (typeof Chart === 'undefined') {
            console.warn('[CHARTS] Chart.js not loaded');
            return;
        }

        // === Gemeinsame Zeitachse erstellen ===
        var allTimes = {};
        ['sender', 'receiver', 'camera'].forEach(function(src) {
            chartData[src].time.forEach(function(t) { allTimes[t] = true; });
        });
        var sortedTimes = Object.keys(allTimes).map(Number).sort(function(a, b) { return a - b; });

        // Keine Daten → Platzhalter-Nachricht anzeigen
        if (sortedTimes.length === 0) {
            console.log('[CHARTS] Keine Telemetrie-Daten vorhanden');
            var rssiEl = document.getElementById('rssiChart');
            var heapEl = document.getElementById('heapChart');
            if (rssiEl) rssiEl.parentElement.innerHTML = '<div style="color:var(--text-secondary);text-align:center;padding:40px;">Keine Telemetrie-Daten vorhanden. Warte auf ESP-Heartbeats...</div>';
            if (heapEl) heapEl.parentElement.innerHTML = '<div style="color:var(--text-secondary);text-align:center;padding:40px;">Keine Telemetrie-Daten vorhanden.</div>';
            return;
        }

        // Zeitstempel zu lesbaren Labels konvertieren
        var labels = sortedTimes.map(function(t) { return new Date(t * 1000).toLocaleTimeString('de-DE'); });

        /**
         * mapToTimeline() - Gerätedaten auf gemeinsame Zeitachse mappen
         * @param {Object} srcData - Daten eines Geräts {time:[], rssi:[], heap:[]}
         * @param {string} field   - Feldname ('rssi' oder 'heap')
         * @returns {Array}        - Werte aligned mit sortedTimes (null wo Daten fehlen)
         */
        function mapToTimeline(srcData, field) {
            var lookup = {};
            srcData.time.forEach(function(t, i) { lookup[t] = srcData[field][i]; });
            return sortedTimes.map(function(t) { return (t in lookup) ? lookup[t] : null; });
        }

        // Bestehende Charts zerstören vor Neuerstellen
        if (rssiChart) { rssiChart.destroy(); rssiChart = null; }
        if (heapChart) { heapChart.destroy(); heapChart = null; }

        /**
         * makeDatasets() - Chart.js Datasets für alle 3 Geräte erstellen
         * @param {string} field - 'rssi' oder 'heap'
         * @returns {Array}      - Array von Chart.js Dataset-Objekten
         */
        function makeDatasets(field) {
            return [
                { label: 'Sender',   data: mapToTimeline(chartData.sender,   field), borderColor: '#3b82f6', tension: 0.3, fill: false, spanGaps: true },
                { label: 'Receiver', data: mapToTimeline(chartData.receiver, field), borderColor: '#f59e0b', tension: 0.3, fill: false, spanGaps: true },
                { label: 'PiCam',    data: mapToTimeline(chartData.camera,   field), borderColor: '#8b5cf6', tension: 0.3, fill: false, spanGaps: true }
            ];
        }

        // === RSSI Chart ===
        var rssiOpts = {
            responsive: true, maintainAspectRatio: false, animation: false,
            plugins: { legend: { labels: { color: '#f1f5f9', usePointStyle: true, pointStyle: 'line' } } },
            scales: {
                x: { ticks: { color: '#94a3b8', maxTicksLimit: 12, maxRotation: 45 }, grid: { color: 'rgba(51,65,85,0.3)' } },
                y: {
                    ticks: { color: '#94a3b8' },
                    grid:  { color: 'rgba(51,65,85,0.3)' },
                    title: { display: true, text: 'dBm', color: '#94a3b8' },
                    suggestedMin: -80,
                    suggestedMax: -30
                }
            },
            elements: { point: { radius: 0, hitRadius: 6 }, line: { borderWidth: 2 } }
        };

        rssiChart = new Chart(document.getElementById('rssiChart'), {
            type: 'line', data: { labels: labels, datasets: makeDatasets('rssi') }, options: rssiOpts
        });

        // === Heap Chart ===
        var heapOpts = {
            responsive: true, maintainAspectRatio: false, animation: false,
            plugins: { legend: { labels: { color: '#f1f5f9', usePointStyle: true, pointStyle: 'line' } } },
            scales: {
                x: { ticks: { color: '#94a3b8', maxTicksLimit: 12, maxRotation: 45 }, grid: { color: 'rgba(51,65,85,0.3)' } },
                y: {
                    ticks: {
                        color: '#94a3b8',
                        callback: function(v) { return (v / 1024).toFixed(1) + ' KB'; }
                    },
                    grid:  { color: 'rgba(51,65,85,0.3)' },
                    title: { display: true, text: 'Bytes', color: '#94a3b8' }
                }
            },
            elements: { point: { radius: 0, hitRadius: 6 }, line: { borderWidth: 2 } }
        };

        heapChart = new Chart(document.getElementById('heapChart'), {
            type: 'line', data: { labels: labels, datasets: makeDatasets('heap') }, options: heapOpts
        });

        console.log('[CHARTS] OK:', sortedTimes.length, 'points');
    } catch(err) {
        console.error('[CHARTS] Error:', err);
    }

    // === Auto-Refresh für Diagnose-Seite ===
    var toggle      = document.getElementById('diag-auto-refresh');
    var diagInterval = null;
    try {
        var saved = sessionStorage.getItem('diagAutoRefresh');
        if (saved !== null) toggle.checked = (saved === 'true');
    } catch(e) {}

    function startDiagRefresh() {
        if (diagInterval) clearInterval(diagInterval);
        if (toggle.checked) diagInterval = setInterval(function() { location.reload(); }, 5000);
    }
    toggle.addEventListener('change', function() {
        try { sessionStorage.setItem('diagAutoRefresh', this.checked); } catch(e) {}
        startDiagRefresh();
    });
    startDiagRefresh();
}

console.log('[INIT] Chart module loaded');

// Auto-Init: Falls Diagnose-Tab beim Seitenladen bereits aktiv ist
try {
    var diagView = document.getElementById('view-diagnose');
    if (diagView && diagView.style.display === 'block') {
        console.log('[CHARTS] Diagnose tab active on load - initializing charts');
        initDiagnoseCharts();
    }
} catch(e) { console.error('[CHARTS] Auto-init error:', e); }
