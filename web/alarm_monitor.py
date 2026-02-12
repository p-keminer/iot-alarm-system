#!/usr/bin/env python3  
"""  
Alarm Monitor Daemon  
====================  
Liest den seriellen Port (Uno R3) und steuert die Kameraaufnahme.  
  
- ALARM_ON  -> Startet ffmpeg-Aufnahme vom mjpg-streamer  
- ALARM_OFF -> Stoppt Aufnahme  
  
Aufnahmen werden in RECORD_DIR gespeichert.  
Status wird in STATUS_FILE geschrieben (fuer Dashboard).  
"""  
  
import serial  
import subprocess  
import os  
import sys  
import time  
import json  
import signal  
import glob  
from datetime import datetime  
  
import shutil  
import fcntl  

  
# === KONFIGURATION ===  
SERIAL_PORTS = ['/dev/ttyUSB0', '/dev/ttyUSB1', '/dev/ttyACM0', '/dev/ttyACM1']  
BAUD_RATE = 9600  
STREAM_PORT = 8082  
RECORD_DIR = '/var/www/html/data/recordings'  
STATUS_FILE = '/var/www/html/data/alarm_monitor.json'  
LOG_FILE = '/var/www/html/data/log.txt'  
MAX_RECORDING_SECONDS = 3600  # Max 1h pro Aufnahme (Sicherheit)  
  
# === GLOBALS ===  
recording_process = None  
current_file = None  
running = True  
  
  
def log(msg):  
    """Schreibt ins System-Log (gleicher Log wie Dashboard)."""  
    timestamp = datetime.now().strftime("[%d.%m.%Y %H:%M:%S]")  
    line = f"{timestamp} camera: [AlarmMonitor] {msg}\n"  
    print(line.strip())  
    try:  
        with open(LOG_FILE, 'a') as f:  
            f.write(line)  
    except Exception:  
        pass  
  
  
def update_status(state, filepath=None, extra=None):  
    """Schreibt Status-JSON fuer Dashboard (mit File-Locking)."""  
    status = {  
        'state': state,  
        'current_file': os.path.basename(filepath) if filepath else None,  
        'timestamp': datetime.now().isoformat(),  
        'pid': os.getpid()  
    }  
    if extra:  
        status.update(extra)  
  
    try:  
        os.makedirs(os.path.dirname(STATUS_FILE), exist_ok=True)  
  
         
        with open(STATUS_FILE, 'w') as f:  
            fcntl.flock(f, fcntl.LOCK_EX)   # exklusiver Write-Lock  
            json.dump(status, f)  
            f.flush()  
            os.fsync(f.fileno())  
            fcntl.flock(f, fcntl.LOCK_UN)  
        
  
    except Exception as e:  
        print(f"Status write error: {e}")  
  
  
def find_serial_port():  
    """Findet den ersten verfuegbaren seriellen Port."""  
    for port in SERIAL_PORTS:  
        if os.path.exists(port):  
            return port  
    return None  
  
  
def start_recording():  
    """Startet ffmpeg-Aufnahme vom mjpg-streamer."""  
    global recording_process, current_file  
  
    #NEU (SCHRITT 1: DISK-FULL-NOTBREMSE)  
    try:  
        total, used, free = shutil.disk_usage("/")  
        free_gb = free // (2**30)  
        if free_gb < 2:  
            log("CRITICAL: Festplatte fast voll! Aufnahme verweigert.")  
            update_status('error', extra={'error': 'Disk full (<2GB free)'})  
            return  
    except Exception as e:  
        log(f"WARN: Disk-Check fehlgeschlagen: {e}")  
        update_status('error', extra={'error': 'Disk check failed'})  
        return  
    # <
  
    if recording_process is not None:  
        log("Aufnahme laeuft bereits")  
        return  
  
    os.makedirs(RECORD_DIR, exist_ok=True)  
  
    filename = datetime.now().strftime('alarm_%Y%m%d_%H%M%S.avi')  
    filepath = os.path.join(RECORD_DIR, filename)  
    current_file = filepath  
  
    stream_url = f"http://localhost:{STREAM_PORT}/?action=stream"  
  
    try:  
        recording_process = subprocess.Popen([  
            'ffmpeg',  
            '-y',  
            '-i', stream_url,  
            '-c:v', 'copy',  
            '-an',  
            '-t', str(MAX_RECORDING_SECONDS),  
            '-f', 'avi',  
            filepath  
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)  
  
        log(f"Aufnahme gestartet: {filename} (PID {recording_process.pid})")  
        update_status('recording', filepath)  
  
    except FileNotFoundError:  
        log("FEHLER: ffmpeg nicht gefunden!")  
        update_status('error', extra={'error': 'ffmpeg nicht installiert'})  
    except Exception as e:  
        log(f"FEHLER beim Starten der Aufnahme: {e}")  
        update_status('error', extra={'error': str(e)})  
  
  
def stop_recording():  
    """Stoppt die laufende ffmpeg-Aufnahme."""  
    global recording_process, current_file  
  
    if recording_process is None:  
        return  
  
    pid = recording_process.pid  
    filename = os.path.basename(current_file) if current_file else 'unknown'  
  
    try:  
        recording_process.send_signal(signal.SIGINT)  
        recording_process.wait(timeout=5)  
    except subprocess.TimeoutExpired:  
        recording_process.kill()  
        recording_process.wait(timeout=3)  
    except Exception as e:  
        log(f"Fehler beim Stoppen: {e}")  
        try:  
            recording_process.kill()  
        except Exception:  
            pass  
  
    size_kb = 0  
    if current_file and os.path.exists(current_file):  
        size_kb = os.path.getsize(current_file) // 1024  
  
    recording_process = None  
    log(f"Aufnahme gestoppt: {filename} ({size_kb} KB)")  
    update_status('idle')  
    current_file = None  
  
  
def signal_handler(signum, frame):  
    global running  
    log("Shutdown-Signal empfangen")  
    stop_recording()  
    update_status('stopped')  
    running = False  
    sys.exit(0)  
  
  
def main():  
    global running  
  
    signal.signal(signal.SIGTERM, signal_handler)  
    signal.signal(signal.SIGINT, signal_handler)  
  
    log("Daemon gestartet")  
  
    port = find_serial_port()  
    if not port:  
        log("FEHLER: Kein serieller Port gefunden!")  
        update_status('error', extra={'error': 'Kein serieller Port'})  
        while running:  
            time.sleep(10)  
            port = find_serial_port()  
            if port:  
                break  
        if not port:  
            return  
  
    log(f"Serieller Port: {port}")  
    update_status('idle')  
  
    try:  
        os.system(f"stty -F {port} 9600 cs8 -cstopb -parenb -echo -hupcl raw 2>/dev/null")  
    except Exception:  
        pass  
  
    try:  
        ser = serial.Serial(  
            port=port,  
            baudrate=BAUD_RATE,  
            timeout=1,  
            dsrdtr=False,  
            rtscts=False,  
            exclusive=False  
        )  
        ser.dtr = False  
    except Exception as e:  
        log(f"FEHLER: Port {port} kann nicht geoeffnet werden: {e}")  
        update_status('error', extra={'error': str(e)})  
        return  
  
    log(f"Lausche auf {port} @ {BAUD_RATE} Baud...")  
  
    reconnect_delay = 0  
    while running:  
        try:  
            raw = ser.readline()  
            if not raw:  
                continue  
  
            line = raw.decode('utf-8', errors='ignore').strip()  
            if not line:  
                continue  
  
            if len(line) > 1:  
                log(f"Serial RX: {line}")  
  
            if 'ALARM_ON' in line:  
                log("ALARM ausgeloest!")  
                start_recording()  
            elif 'ALARM_OFF' in line:  
                log("Alarm beendet")  
                stop_recording()  
  
            reconnect_delay = 0  
  
        except serial.SerialException as e:  
            log(f"Serial-Fehler: {e}")  
            stop_recording()  
            time.sleep(max(reconnect_delay, 5))  
            reconnect_delay = min(reconnect_delay + 5, 60)  
  
            try:  
                ser.close()  
            except Exception:  
                pass  
  
            port = find_serial_port()  
            if port:  
                try:  
                    ser = serial.Serial(  
                        port=port,  
                        baudrate=BAUD_RATE,  
                        timeout=1,  
                        dsrdtr=False,  
                        rtscts=False,  
                        exclusive=False  
                    )  
                    ser.dtr = False  
                    log(f"Reconnected: {port}")  
                    update_status('idle')  
                except Exception:  
                    pass  
  
        except Exception as e:  
            log(f"Unerwarteter Fehler: {e}")  
            time.sleep(1)  
  
    stop_recording()  
    update_status('stopped')  
    log("Daemon beendet")  
  
  
if __name__ == '__main__':  
    main()  
