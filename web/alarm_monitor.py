#!/usr/bin/env python3
"""Own the alarm serial link, recorder and local dashboard control socket.

Only this daemon may open the Uno serial device or start/stop recordings.  The
PHP dashboard talks to a small Unix-domain socket with short-lived, strictly
validated JSON requests.  This keeps ``www-data`` out of ``dialout`` and
removes shell/process control from the web application.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
from datetime import datetime

import serial


DEFAULT_SERIAL_PORTS = (
    "/dev/ttyACM0",
    "/dev/ttyACM1",
    "/dev/ttyUSB0",
    "/dev/ttyUSB1",
)
_SERIAL_PORT_RE = re.compile(
    r"^/dev/(?:tty[A-Za-z0-9._:+-]+|serial/(?:by-id|by-path)/[A-Za-z0-9._:+-]+)$"
)
_STATUS_RE = re.compile(
    r"^STATUS:SCHARF=([01]),AUSGELOEST=([01]),"
    r"TUER1=(OFFEN|ZU),TUER2=(OFFEN|ZU),VERBINDUNG=(OK|VERLOREN)$"
)
_REQUEST_ID_RE = re.compile(r"^[a-f0-9]{32}$")
_RECORDING_RE = re.compile(
    r"^(?:alarm|manual)_\d{8}_\d{6}(?:_\d{1,3})?\.(?:avi|mp4|mkv)$"
)
_IPC_ACTIONS = {
    "arm",
    "disarm",
    "manual_record_start",
    "manual_record_stop",
    "delete_recording",
    "delete_all_recordings",
    "clear_runtime_log",
    "status",
}


class ConfigurationError(ValueError):
    """Raised when an environment value is unsafe or outside its limits."""


def _env_int(environ, name, default, minimum, maximum):
    raw = environ.get(name)
    if raw is None or raw == "":
        return default
    try:
        value = int(raw, 10)
    except ValueError as exc:
        raise ConfigurationError(f"{name} muss eine ganze Zahl sein") from exc
    if not minimum <= value <= maximum:
        raise ConfigurationError(
            f"{name} muss zwischen {minimum} und {maximum} liegen"
        )
    return value


def _env_float(environ, name, default, minimum, maximum):
    raw = environ.get(name)
    if raw is None or raw == "":
        return default
    try:
        value = float(raw)
    except ValueError as exc:
        raise ConfigurationError(f"{name} muss eine Zahl sein") from exc
    if not minimum <= value <= maximum:
        raise ConfigurationError(
            f"{name} muss zwischen {minimum} und {maximum} liegen"
        )
    return value


def _absolute_path(environ, name, default):
    raw = environ.get(name, default)
    if not raw or "\x00" in raw or not os.path.isabs(raw):
        raise ConfigurationError(f"{name} muss ein absoluter Pfad sein")
    value = os.path.normpath(raw)
    if value == os.path.sep:
        raise ConfigurationError(f"{name} darf nicht das Wurzelverzeichnis sein")
    return value


def _serial_ports(environ):
    raw = environ.get("ALARM_SERIAL_PORTS", "")
    ports = tuple(part.strip() for part in raw.split(",") if part.strip())
    if not ports:
        ports = DEFAULT_SERIAL_PORTS
    if len(ports) > 16:
        raise ConfigurationError("ALARM_SERIAL_PORTS enthaelt zu viele Eintraege")
    if any(not _SERIAL_PORT_RE.fullmatch(port) for port in ports):
        raise ConfigurationError(
            "ALARM_SERIAL_PORTS akzeptiert nur tty- oder /dev/serial/by-*-Pfade"
        )
    return ports


class MonitorConfig:
    """Validated runtime configuration."""

    def __init__(self, environ=None):
        environ = os.environ if environ is None else environ
        self.serial_ports = _serial_ports(environ)
        self.baud_rate = _env_int(environ, "ALARM_BAUD_RATE", 9600, 1200, 115200)
        self.serial_timeout = _env_float(
            environ, "ALARM_SERIAL_TIMEOUT_SECONDS", 0.20, 0.05, 2.0
        )
        self.serial_write_timeout = _env_float(
            environ, "ALARM_SERIAL_WRITE_TIMEOUT_SECONDS", 1.0, 0.1, 10.0
        )
        self.handshake_timeout = _env_float(
            environ, "ALARM_HANDSHAKE_TIMEOUT_SECONDS", 8.0, 1.0, 30.0
        )
        self.status_interval = _env_float(
            environ, "ALARM_STATUS_INTERVAL_SECONDS", 10.0, 2.0, 300.0
        )
        self.status_response_timeout = _env_float(
            environ, "ALARM_STATUS_RESPONSE_TIMEOUT_SECONDS", 2.0, 0.5, 10.0
        )
        self.command_confirm_timeout = _env_float(
            environ, "ALARM_COMMAND_CONFIRM_SECONDS", 2.0, 0.5, 10.0
        )
        self.reconnect_min = _env_float(
            environ, "ALARM_RECONNECT_MIN_SECONDS", 2.0, 0.2, 60.0
        )
        self.reconnect_max = _env_float(
            environ, "ALARM_RECONNECT_MAX_SECONDS", 30.0, 0.2, 300.0
        )
        if self.reconnect_max < self.reconnect_min:
            raise ConfigurationError(
                "ALARM_RECONNECT_MAX_SECONDS darf nicht kleiner als das Minimum sein"
            )

        self.stream_port = _env_int(environ, "ALARM_STREAM_PORT", 8082, 1, 65535)
        self.max_recording_seconds = _env_int(
            environ, "ALARM_MAX_RECORDING_SECONDS", 3600, 30, 86400
        )
        self.max_recording_mib = _env_int(
            environ, "ALARM_MAX_RECORDING_MIB", 2048, 16, 1048576
        )
        self.min_free_mib = _env_int(
            environ, "ALARM_MIN_FREE_MIB", 2048, 128, 1048576
        )
        self.disk_safety_mib = _env_int(
            environ, "ALARM_DISK_SAFETY_MIB", 128, 16, 102400
        )
        self.recording_retry_limit = _env_int(
            environ, "ALARM_RECORDING_RETRY_LIMIT", 3, 0, 20
        )
        self.log_max_kib = _env_int(
            environ, "ALARM_LOG_MAX_KIB", 1024, 64, 102400
        )
        self.log_backups = _env_int(environ, "ALARM_LOG_BACKUPS", 2, 1, 10)
        self.ipc_ttl_seconds = _env_int(
            environ, "ALARM_IPC_TTL_SECONDS", 10, 2, 60
        )

        self.data_dir = _absolute_path(environ, "ALARM_DATA_DIR", "/var/www/html/data")
        self.record_dir = _absolute_path(
            environ, "ALARM_RECORD_DIR", os.path.join(self.data_dir, "recordings")
        )
        self.ffmpeg_bin = _absolute_path(
            environ, "ALARM_FFMPEG_BIN", "/usr/bin/ffmpeg"
        )
        self.ipc_socket = _absolute_path(
            environ,
            "ALARM_IPC_SOCKET",
            "/run/iot-alarm-monitor/control.sock",
        )
        self.status_file = os.path.join(self.data_dir, "alarm_monitor.json")
        self.log_file = os.path.join(self.data_dir, "log.txt")


def _atomic_json_write(path, value):
    """Replace a JSON file atomically so readers never observe truncation."""

    directory = os.path.dirname(path)
    os.makedirs(directory, mode=0o750, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=f".{os.path.basename(path)}.", dir=directory, text=True
    )
    try:
        os.chmod(temporary, 0o640)
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            descriptor = -1
            json.dump(value, handle, ensure_ascii=False, separators=(",", ":"))
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


class AlarmMonitor:
    """Single owner for serial I/O, recording and recording deletion."""

    def __init__(self, config):
        self.config = config
        self.recording_process = None
        self.current_file = None
        self.recording_reason = None
        self.recording_started = None
        self.recording_limit_bytes = None
        self.recording_retry_count = 0
        self.next_recording_retry = 0.0
        self.recording_cutoff_reached = False
        self.alarm_active = False
        self.manual_requested = False
        self.running = True
        self.serial_connected = False
        self.serial_port = None
        self._last_connection_error = None
        self._last_status_request = 0.0
        self._pending_status_since = None
        self._serial_fault_reason = None
        self._serial_discard_until_eol = False
        self._ipc_server = None
        self._seen_request_ids = {}

    # ------------------------------------------------------------------ logging
    def _rotate_log_if_needed(self, incoming_bytes):
        try:
            current = os.path.getsize(self.config.log_file)
        except FileNotFoundError:
            return
        limit = self.config.log_max_kib * 1024
        if current + incoming_bytes <= limit:
            return
        for index in range(self.config.log_backups, 0, -1):
            source = self.config.log_file if index == 1 else f"{self.config.log_file}.{index - 1}"
            target = f"{self.config.log_file}.{index}"
            if not os.path.exists(source):
                continue
            if index == self.config.log_backups and os.path.exists(target):
                os.unlink(target)
            os.replace(source, target)

    def log(self, message):
        safe = "".join(ch if ch.isprintable() else "?" for ch in str(message))[:500]
        timestamp = datetime.now().strftime("[%d.%m.%Y %H:%M:%S]")
        line = f"{timestamp} camera: [AlarmMonitor] {safe}\n"
        print(line.rstrip(), flush=True)
        try:
            os.makedirs(self.config.data_dir, mode=0o750, exist_ok=True)
            encoded = line.encode("utf-8", errors="replace")
            self._rotate_log_if_needed(len(encoded))
            flags = os.O_APPEND | os.O_CREAT | os.O_WRONLY
            if hasattr(os, "O_NOFOLLOW"):
                flags |= os.O_NOFOLLOW
            descriptor = os.open(self.config.log_file, flags, 0o640)
            try:
                os.write(descriptor, encoded)
            finally:
                os.close(descriptor)
        except OSError as exc:
            print(f"Log write error: {exc}", file=sys.stderr, flush=True)

    # ------------------------------------------------------------------- status
    def status_payload(self, state=None, error=None):
        if state is None:
            if self.recording_process is not None:
                state = "recording"
            elif self.serial_connected:
                state = "idle"
            else:
                state = "error"
        payload = {
            "state": state,
            "current_file": os.path.basename(self.current_file) if self.current_file else None,
            "recording_reason": self.recording_reason,
            "alarm_active": self.alarm_active,
            "manual_requested": self.manual_requested,
            "timestamp": datetime.now().isoformat(),
            "pid": os.getpid(),
            "serial_connected": self.serial_connected,
            "serial_port": self.serial_port,
            "baud_rate": self.config.baud_rate,
        }
        if error:
            payload["error"] = error
        return payload

    def publish_runtime_status(self, error=None, state=None):
        try:
            _atomic_json_write(self.config.status_file, self.status_payload(state, error))
        except OSError as exc:
            print(f"Status write error: {exc}", file=sys.stderr, flush=True)

    def update_status(self, state, filepath=None, error=None):
        """Compatibility wrapper used by older local tooling and tests."""
        if filepath is not None:
            self.current_file = filepath
        self.publish_runtime_status(error=error, state=state)

    # ---------------------------------------------------------------- recording
    def _recording_disk_path(self):
        path = self.config.record_dir
        while not os.path.exists(path):
            parent = os.path.dirname(path)
            if parent == path:
                return os.path.sep
            path = parent
        return path

    def _new_recording_path(self, prefix):
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        base = os.path.join(self.config.record_dir, f"{prefix}_{stamp}.avi")
        if not os.path.exists(base):
            return base
        for suffix in range(1, 1000):
            candidate = os.path.join(
                self.config.record_dir, f"{prefix}_{stamp}_{suffix}.avi"
            )
            if not os.path.exists(candidate):
                return candidate
        raise OSError("Kein eindeutiger Aufnahmedateiname verfuegbar")

    def _clear_recording_process(self):
        self.recording_process = None
        self.current_file = None
        self.recording_reason = None
        self.recording_started = None
        self.recording_limit_bytes = None

    def _schedule_recording_retry(self, message):
        self.recording_retry_count += 1
        if self.recording_retry_count <= self.config.recording_retry_limit:
            delay = min(5 * (2 ** (self.recording_retry_count - 1)), 60)
            self.next_recording_retry = time.monotonic() + delay
            self.log(
                f"{message}: Retry {self.recording_retry_count}/"
                f"{self.config.recording_retry_limit} in {delay}s"
            )
        else:
            self.recording_cutoff_reached = True
            self.log(f"{message}: Retry-Limit erreicht")

    def reap_recording(self):
        if self.recording_process is None:
            return
        return_code = self.recording_process.poll()
        if return_code is None:
            return

        filename = os.path.basename(self.current_file or "unknown")
        duration = time.monotonic() - (self.recording_started or time.monotonic())
        reason = self.recording_reason
        filepath = self.current_file
        limit_bytes = self.recording_limit_bytes
        self._clear_recording_process()
        file_size = 0
        if filepath:
            try:
                file_size = os.path.getsize(filepath)
            except OSError:
                pass
        size_tolerance = max(1024 * 1024, int((limit_bytes or 0) * 0.01))
        size_limit_reached = bool(
            limit_bytes and file_size >= max(0, limit_bytes - size_tolerance)
        )
        normal_cutoff = (
            duration >= self.config.max_recording_seconds - 2 or size_limit_reached
        )
        if normal_cutoff:
            self.recording_cutoff_reached = True
            if reason == "manual" and not self.alarm_active:
                self.manual_requested = False
            self.log(f"Aufnahme regulaer beendet: {filename}")
        else:
            self._schedule_recording_retry(f"ffmpeg Status {return_code}")
        self.publish_runtime_status(
            error=None if normal_cutoff else f"ffmpeg exit {return_code}"
        )

    def start_recording(self, reason):
        self.reap_recording()
        if self.recording_process is not None or self.recording_cutoff_reached:
            return False
        try:
            free = shutil.disk_usage(self._recording_disk_path()).free
        except OSError as exc:
            self._schedule_recording_retry(f"Disk-Check fehlgeschlagen: {exc}")
            self.publish_runtime_status(error="Disk check failed")
            return False

        reserve = self.config.min_free_mib * 1024 * 1024
        safety = self.config.disk_safety_mib * 1024 * 1024
        budget = min(
            self.config.max_recording_mib * 1024 * 1024,
            free - reserve - safety,
        )
        if budget < 16 * 1024 * 1024:
            self._schedule_recording_retry(
                "Aufnahme verweigert: Speicherreserve nicht gewaehrleistet"
            )
            self.publish_runtime_status(error="Disk reserve reached")
            return False

        os.makedirs(self.config.record_dir, mode=0o750, exist_ok=True)
        try:
            filepath = self._new_recording_path("manual" if reason == "manual" else "alarm")
        except OSError as exc:
            self._schedule_recording_retry(str(exc))
            return False
        stream_url = f"http://127.0.0.1:{self.config.stream_port}/?action=stream"
        try:
            process = subprocess.Popen(
                [
                    self.config.ffmpeg_bin,
                    "-nostdin",
                    "-n",
                    "-i",
                    stream_url,
                    "-c:v",
                    "copy",
                    "-an",
                    "-t",
                    str(self.config.max_recording_seconds),
                    "-fs",
                    str(budget),
                    "-f",
                    "avi",
                    filepath,
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except (FileNotFoundError, OSError) as exc:
            self.log(f"Aufnahme konnte nicht gestartet werden: {exc}")
            self._schedule_recording_retry("ffmpeg-Start fehlgeschlagen")
            self.publish_runtime_status(error="Recording start failed")
            return False

        self.recording_process = process
        self.current_file = filepath
        self.recording_reason = reason
        self.recording_started = time.monotonic()
        self.recording_limit_bytes = budget
        self.log(f"Aufnahme gestartet: {os.path.basename(filepath)}")
        self.publish_runtime_status()
        return True

    def stop_recording(self):
        self.reap_recording()
        if self.recording_process is None:
            return
        process = self.recording_process
        filepath = self.current_file
        try:
            process.send_signal(signal.SIGINT)
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            try:
                process.kill()
                process.wait(timeout=3)
            except (OSError, subprocess.TimeoutExpired):
                self.log("ffmpeg reagiert auch nach SIGKILL nicht; Cleanup wird fortgesetzt")
        except OSError:
            try:
                process.kill()
                process.wait(timeout=3)
            except (OSError, subprocess.TimeoutExpired):
                pass
        size_kib = 0
        if filepath and os.path.exists(filepath):
            size_kib = os.path.getsize(filepath) // 1024
        filename = os.path.basename(filepath or "unknown")
        self._clear_recording_process()
        self.log(f"Aufnahme gestoppt: {filename} ({size_kib} KiB)")
        self.publish_runtime_status()

    def _recording_should_run(self):
        return self.alarm_active or self.manual_requested

    def reconcile_recording(self):
        self.reap_recording()
        should_run = self._recording_should_run()
        if not should_run:
            self.recording_cutoff_reached = False
            self.recording_retry_count = 0
            self.next_recording_retry = 0.0
            if self.recording_process is not None:
                self.stop_recording()
            return
        if (
            self.recording_process is None
            and not self.recording_cutoff_reached
            and time.monotonic() >= self.next_recording_retry
        ):
            reason = "alarm" if self.alarm_active else "manual"
            self.start_recording(reason)

        if self.recording_process is not None:
            try:
                free = shutil.disk_usage(self._recording_disk_path()).free
            except OSError:
                free = 0
            stop_threshold = (
                self.config.min_free_mib + self.config.disk_safety_mib
            ) * 1024 * 1024
            if free < stop_threshold:
                self.log("Aufnahme beendet: konfigurierte Speicherreserve erreicht")
                self.recording_cutoff_reached = True
                self.stop_recording()

    def set_alarm_active(self, active):
        active = bool(active)
        if active and not self.alarm_active:
            self.recording_cutoff_reached = False
            self.recording_retry_count = 0
            self.next_recording_retry = 0.0
        self.alarm_active = active
        self.reconcile_recording()
        self.publish_runtime_status()

    # ------------------------------------------------------------------- serial
    def _open_serial_port(self, port):
        connection = serial.Serial(
            port=None,
            baudrate=self.config.baud_rate,
            timeout=self.config.serial_timeout,
            write_timeout=self.config.serial_write_timeout,
            dsrdtr=False,
            rtscts=False,
            exclusive=True,
        )
        connection.dtr = False
        connection.port = port
        connection.open()
        return connection

    @staticmethod
    def _write_line(connection, line):
        payload = line.encode("ascii") + b"\n"
        written = connection.write(payload)
        connection.flush()
        if written != len(payload):
            raise serial.SerialTimeoutException("Serielle Ausgabe unvollstaendig")

    @staticmethod
    def _decode_serial_line(raw):
        if not raw or len(raw) > 256 or not raw.endswith((b"\n", b"\r")):
            return None
        try:
            line = raw.decode("ascii", errors="strict").strip("\r\n")
        except UnicodeDecodeError:
            return None
        if not line or any(ord(ch) < 0x20 or ord(ch) > 0x7E for ch in line):
            return None
        return line

    def _read_line(self, connection):
        if hasattr(connection, "read_until"):
            raw = connection.read_until(b"\n", 257)
        else:
            raw = connection.readline()
        if self._serial_discard_until_eol:
            if raw.endswith((b"\n", b"\r")):
                self._serial_discard_until_eol = False
            return None
        if raw and not raw.endswith((b"\n", b"\r")):
            # A timeout or overlong line must not make its remaining suffix a
            # fresh command on the next iteration.
            self._serial_discard_until_eol = True
            return None
        return self._decode_serial_line(raw)

    def _apply_status_line(self, line):
        match = _STATUS_RE.fullmatch(line)
        if not match or (match.group(1) == "0" and match.group(2) == "1"):
            return False
        self._pending_status_since = None
        # AUSGELOEST is authoritative. SCHARF remains useful for diagnostics;
        # only an active alarm drives recording.
        self.set_alarm_active(match.group(2) == "1")
        return True

    def _handshake(self, connection):
        deadline = time.monotonic() + self.config.handshake_timeout
        next_request = 0.0
        while self.running and time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_request:
                self._write_line(connection, "STATUS")
                next_request = now + 1.0
            line = self._read_line(connection)
            if line == "HB":
                self._write_line(connection, "HB_ACK")
            elif line and self._apply_status_line(line):
                return True
        return False

    def connect_serial(self):
        errors = []
        for port in self.config.serial_ports:
            if not os.path.exists(port):
                continue
            connection = None
            try:
                connection = self._open_serial_port(port)
                self._serial_discard_until_eol = False
                if not self._handshake(connection):
                    raise serial.SerialException("kein gueltiger STATUS-Handshake")
            except (serial.SerialException, OSError) as exc:
                errors.append(f"{port}: {exc}")
                if connection is not None:
                    try:
                        connection.close()
                    except (serial.SerialException, OSError):
                        pass
                continue

            self.serial_connected = True
            self.serial_port = port
            self._last_connection_error = None
            self._last_status_request = time.monotonic()
            self._pending_status_since = None
            self._serial_fault_reason = None
            self.log(f"Uno validiert: {port} @ {self.config.baud_rate} Baud")
            self.publish_runtime_status()
            return connection

        error = "; ".join(errors) if errors else "Kein serieller Port vorhanden"
        if error != self._last_connection_error:
            self.log(error)
            self._last_connection_error = error
            self.publish_runtime_status(error="Kein validierter Uno gefunden")
        self.serial_connected = False
        self.serial_port = None
        return None

    def disconnect_serial(self, connection, reason):
        try:
            connection.close()
        except (serial.SerialException, OSError):
            pass
        self.serial_connected = False
        self.serial_port = None
        self.log(f"Serielle Verbindung getrennt: {reason}")
        # The last known alarm target remains active until a STATUS snapshot
        # after reconnect proves otherwise.
        self.publish_runtime_status(error="Serielle Verbindung getrennt")

    def handle_serial_line(self, connection, line):
        if line == "HB":
            self._write_line(connection, "HB_ACK")
            return
        if line == "ALARM_ON":
            self.log("Alarm ausgeloest")
            self.set_alarm_active(True)
            return
        if line == "ALARM_OFF":
            self.log("Alarm beendet")
            self.set_alarm_active(False)
            return
        if self._apply_status_line(line):
            self.log(f"Serial RX: {line}")
            return
        self.log(f"Serial RX: {line}")

    def _command_and_confirm(self, connection, action):
        target_armed = action == "arm"
        self._write_line(connection, "SCHARF" if target_armed else "UNSCHARF")
        deadline = time.monotonic() + self.config.command_confirm_timeout
        next_status = 0.0
        while self.running and time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_status:
                self._write_line(connection, "STATUS")
                self._last_status_request = now
                self._pending_status_since = now
                next_status = now + 0.5
            line = self._read_line(connection)
            if line == "HB":
                self._write_line(connection, "HB_ACK")
                continue
            if line:
                match = _STATUS_RE.fullmatch(line)
                if match and match.group(1) == "0" and match.group(2) == "1":
                    match = None
                self.handle_serial_line(connection, line)
                confirmed = bool(
                    match
                    and (
                        (target_armed and match.group(1) == "1")
                        or (
                            not target_armed
                            and match.group(1) == "0"
                            and match.group(2) == "0"
                        )
                    )
                )
                if confirmed:
                    return {"confirmed": True, "armed": target_armed}
        self._serial_fault_reason = "STATUS-Bestaetigung nach Steuerbefehl fehlt"
        raise RuntimeError(self._serial_fault_reason)

    # ---------------------------------------------------------------------- IPC
    def setup_ipc(self):
        directory = os.path.dirname(self.config.ipc_socket)
        os.makedirs(directory, mode=0o750, exist_ok=True)
        try:
            existing = os.lstat(self.config.ipc_socket)
        except FileNotFoundError:
            existing = None
        if existing is not None:
            if not stat.S_ISSOCK(existing.st_mode):
                raise ConfigurationError("ALARM_IPC_SOCKET existiert und ist kein Socket")
            os.unlink(self.config.ipc_socket)
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            server.bind(self.config.ipc_socket)
            os.chmod(self.config.ipc_socket, 0o660)
            server.listen(8)
            server.setblocking(False)
        except Exception:
            server.close()
            raise
        self._ipc_server = server

    def close_ipc(self):
        if self._ipc_server is not None:
            self._ipc_server.close()
            self._ipc_server = None
        try:
            info = os.lstat(self.config.ipc_socket)
            if stat.S_ISSOCK(info.st_mode):
                os.unlink(self.config.ipc_socket)
        except FileNotFoundError:
            pass

    def _validate_ipc_request(self, request):
        if not isinstance(request, dict):
            raise ValueError("request must be an object")
        allowed = {"version", "id", "action", "issued_at", "expires_at", "filename"}
        if set(request) - allowed:
            raise ValueError("unknown request field")
        request_id = request.get("id")
        action = request.get("action")
        issued = request.get("issued_at")
        expires = request.get("expires_at")
        if request.get("version") != 1 or not isinstance(request_id, str) or not _REQUEST_ID_RE.fullmatch(request_id):
            raise ValueError("invalid request identity")
        if action not in _IPC_ACTIONS:
            raise ValueError("invalid action")
        if not isinstance(issued, int) or not isinstance(expires, int):
            raise ValueError("invalid request time")
        now = int(time.time())
        if issued > now + 2 or expires < now or expires - issued > self.config.ipc_ttl_seconds:
            raise ValueError("expired request")
        if request_id in self._seen_request_ids:
            raise ValueError("replayed request")
        filename = request.get("filename")
        if action == "delete_recording":
            if not isinstance(filename, str) or not _RECORDING_RE.fullmatch(filename):
                raise ValueError("invalid filename")
        elif filename is not None:
            raise ValueError("filename not allowed for action")
        self._seen_request_ids[request_id] = expires
        return action, filename

    def _recording_path_for_delete(self, filename):
        if not _RECORDING_RE.fullmatch(filename):
            raise ValueError("invalid filename")
        path = os.path.join(self.config.record_dir, filename)
        try:
            info = os.lstat(path)
        except FileNotFoundError as exc:
            raise FileNotFoundError("Aufnahme nicht gefunden") from exc
        if not stat.S_ISREG(info.st_mode):
            raise ValueError("recording is not a regular file")
        return path

    def _delete_recording(self, filename):
        path = self._recording_path_for_delete(filename)
        if self.current_file and os.path.abspath(path) == os.path.abspath(self.current_file):
            raise RuntimeError("Aktive Aufnahme kann nicht geloescht werden")
        os.unlink(path)
        self.log(f"Aufnahme geloescht: {filename}")
        return {"deleted": 1}

    def _delete_all_recordings(self):
        count = 0
        if not os.path.isdir(self.config.record_dir):
            return {"deleted": 0}
        active = os.path.abspath(self.current_file) if self.current_file else None
        with os.scandir(self.config.record_dir) as entries:
            for entry in entries:
                if not _RECORDING_RE.fullmatch(entry.name) or not entry.is_file(follow_symlinks=False):
                    continue
                if active and os.path.abspath(entry.path) == active:
                    continue
                os.unlink(entry.path)
                count += 1
        self.log(f"{count} inaktive Aufnahmen geloescht")
        return {"deleted": count}

    def _clear_runtime_log(self):
        cleared = 0
        for index in range(0, self.config.log_backups + 1):
            path = self.config.log_file if index == 0 else f"{self.config.log_file}.{index}"
            try:
                info = os.lstat(path)
            except FileNotFoundError:
                continue
            if not stat.S_ISREG(info.st_mode):
                raise RuntimeError("Runtime-Log ist keine regulaere Datei")
            if index == 0:
                flags = os.O_WRONLY | os.O_TRUNC
                if hasattr(os, "O_NOFOLLOW"):
                    flags |= os.O_NOFOLLOW
                descriptor = os.open(path, flags)
                os.close(descriptor)
            else:
                os.unlink(path)
            cleared += 1
        return {"cleared": cleared}

    def execute_ipc_request(self, request, connection=None):
        action, filename = self._validate_ipc_request(request)
        if action in {"arm", "disarm"}:
            if connection is None or not self.serial_connected:
                raise RuntimeError("Uno ist nicht verbunden")
            return self._command_and_confirm(connection, action)
        if action == "manual_record_start":
            if not self.manual_requested:
                self.recording_cutoff_reached = False
                self.recording_retry_count = 0
                self.next_recording_retry = 0.0
            self.manual_requested = True
            self.reconcile_recording()
            return {"recording": self.recording_process is not None}
        if action == "manual_record_stop":
            self.manual_requested = False
            self.reconcile_recording()
            return {"recording": self.recording_process is not None}
        if action == "delete_recording":
            return self._delete_recording(filename)
        if action == "delete_all_recordings":
            return self._delete_all_recordings()
        if action == "clear_runtime_log":
            return self._clear_runtime_log()
        return self.status_payload()

    def handle_ipc(self, serial_connection):
        if self._ipc_server is None:
            return
        now = int(time.time())
        self._seen_request_ids = {
            request_id: expiry
            for request_id, expiry in self._seen_request_ids.items()
            if expiry >= now
        }
        # One local client must never starve heartbeat/status processing by
        # trickling bytes just below the socket timeout. The entire IPC batch,
        # not each recv(), has a small absolute deadline.
        batch_deadline = time.monotonic() + 0.15
        for _ in range(4):
            if time.monotonic() >= batch_deadline:
                return
            try:
                client, _ = self._ipc_server.accept()
            except BlockingIOError:
                return
            with client:
                client_deadline = min(batch_deadline, time.monotonic() + 0.10)
                response = None
                try:
                    payload = b""
                    while b"\n" not in payload and len(payload) <= 4096:
                        remaining = client_deadline - time.monotonic()
                        if remaining <= 0:
                            raise ValueError("request deadline exceeded")
                        client.settimeout(min(0.05, remaining))
                        chunk = client.recv(1024)
                        if not chunk:
                            break
                        payload += chunk
                    if len(payload) > 4096 or b"\n" not in payload:
                        raise ValueError("invalid request framing")
                    first, trailing = payload.split(b"\n", 1)
                    if trailing.strip():
                        raise ValueError("multiple requests are not allowed")
                    request = json.loads(first.decode("utf-8", errors="strict"))
                    result = self.execute_ipc_request(request, serial_connection)
                    response = {"ok": True, "id": request.get("id"), "result": result}
                except (ValueError, UnicodeError, json.JSONDecodeError) as exc:
                    response = {"ok": False, "error": str(exc)}
                except (RuntimeError, FileNotFoundError, OSError, serial.SerialException) as exc:
                    response = {"ok": False, "error": str(exc)}
                try:
                    client.sendall(
                        json.dumps(response, ensure_ascii=True, separators=(",", ":")).encode("ascii")
                        + b"\n"
                    )
                except OSError:
                    pass

    # --------------------------------------------------------------------- loop
    def request_stop(self, _signum=None, _frame=None):
        if self.running:
            self.log("Shutdown-Signal empfangen")
        self.running = False

    def _interruptible_sleep(self, seconds, serial_connection=None):
        deadline = time.monotonic() + seconds
        while self.running and time.monotonic() < deadline:
            self.handle_ipc(serial_connection)
            self.reconcile_recording()
            time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))

    def run(self):
        signal.signal(signal.SIGTERM, self.request_stop)
        signal.signal(signal.SIGINT, self.request_stop)
        self.setup_ipc()
        self.log("Daemon gestartet")
        connection = None
        reconnect_delay = self.config.reconnect_min
        try:
            while self.running:
                self.handle_ipc(connection)
                self.reconcile_recording()
                if connection is None:
                    connection = self.connect_serial()
                    if connection is None:
                        self._interruptible_sleep(reconnect_delay)
                        reconnect_delay = min(reconnect_delay * 2, self.config.reconnect_max)
                        continue
                    reconnect_delay = self.config.reconnect_min

                try:
                    if self._serial_fault_reason:
                        reason = self._serial_fault_reason
                        self._serial_fault_reason = None
                        raise serial.SerialException(reason)
                    now = time.monotonic()
                    if (
                        self._pending_status_since is not None
                        and now - self._pending_status_since
                        > self.config.status_response_timeout
                    ):
                        self._pending_status_since = None
                        raise serial.SerialTimeoutException("periodischer STATUS-Timeout")
                    if (
                        self._pending_status_since is None
                        and now - self._last_status_request >= self.config.status_interval
                    ):
                        self._write_line(connection, "STATUS")
                        self._last_status_request = now
                        self._pending_status_since = now
                    line = self._read_line(connection)
                    if line:
                        self.handle_serial_line(connection, line)
                except (serial.SerialException, OSError) as exc:
                    self.disconnect_serial(connection, str(exc))
                    connection = None
                    self._interruptible_sleep(reconnect_delay)
                    reconnect_delay = min(reconnect_delay * 2, self.config.reconnect_max)
        finally:
            if connection is not None:
                try:
                    connection.close()
                except (serial.SerialException, OSError):
                    pass
            self.serial_connected = False
            self.serial_port = None
            self.alarm_active = False
            self.manual_requested = False
            self.stop_recording()
            self.publish_runtime_status(state="stopped")
            self.close_ipc()
            self.log("Daemon beendet")


def main():
    try:
        config = MonitorConfig()
        AlarmMonitor(config).run()
    except (ConfigurationError, OSError) as exc:
        print(f"Konfigurationsfehler: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
