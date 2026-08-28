import importlib.util
import json
import os
import pathlib
import socket
import subprocess
import tempfile
import time
import unittest
from unittest import mock


MODULE_PATH = pathlib.Path(__file__).parents[2] / "web" / "alarm_monitor.py"
SPEC = importlib.util.spec_from_file_location("alarm_monitor", MODULE_PATH)
alarm_monitor = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(alarm_monitor)


class FakeConnection:
    def __init__(self, reads=None):
        self.writes = []
        self.flush_count = 0
        self.closed = False
        self.reads = list(reads or [])

    def write(self, payload):
        self.writes.append(payload)
        return len(payload)

    def flush(self):
        self.flush_count += 1

    def close(self):
        self.closed = True

    def read_until(self, _separator, _size):
        return self.reads.pop(0) if self.reads else b""


class FakeProcess:
    def __init__(self, return_code=None):
        self.return_code = return_code
        self.pid = 1234
        self.signals = []

    def poll(self):
        return self.return_code

    def send_signal(self, value):
        self.signals.append(value)

    def wait(self, timeout=None):
        self.return_code = 0
        return 0

    def kill(self):
        self.return_code = -9


class StubbornProcess(FakeProcess):
    def wait(self, timeout=None):
        raise subprocess.TimeoutExpired("ffmpeg", timeout)


class SlowClient:
    def __init__(self):
        self.response = b""

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def settimeout(self, _value):
        pass

    def recv(self, _size):
        time.sleep(0.02)
        return b"x"

    def sendall(self, payload):
        self.response += payload


class OneClientServer:
    def __init__(self, client):
        self.client = client
        self.used = False

    def accept(self):
        if self.used:
            raise BlockingIOError()
        self.used = True
        return self.client, None

    def close(self):
        pass


class AlarmMonitorTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.environ = {
            "ALARM_DATA_DIR": self.temporary.name,
            "ALARM_RECORD_DIR": os.path.join(self.temporary.name, "recordings"),
            "ALARM_FFMPEG_BIN": os.path.abspath("ffmpeg-test-bin"),
            "ALARM_IPC_SOCKET": os.path.join(self.temporary.name, "control.sock"),
            "ALARM_HANDSHAKE_TIMEOUT_SECONDS": "1",
            "ALARM_COMMAND_CONFIRM_SECONDS": "0.5",
        }
        self.config = alarm_monitor.MonitorConfig(self.environ)
        self.monitor = alarm_monitor.AlarmMonitor(self.config)

    def tearDown(self):
        self.monitor.close_ipc()
        self.temporary.cleanup()

    def request(self, action, **extra):
        now = int(time.time())
        return {
            "version": 1,
            "id": os.urandom(16).hex(),
            "action": action,
            "issued_at": now,
            "expires_at": now + 5,
            **extra,
        }

    def test_heartbeat_is_acknowledged_exactly(self):
        connection = FakeConnection()
        self.monitor.handle_serial_line(connection, "HB")
        self.assertEqual(connection.writes, [b"HB_ACK\n"])
        self.assertEqual(connection.flush_count, 1)

    def test_alarm_commands_and_status_require_exact_lines(self):
        connection = FakeConnection()
        self.monitor.reconcile_recording = mock.Mock()
        self.monitor.publish_runtime_status = mock.Mock()

        self.monitor.handle_serial_line(connection, "STATUS:ALARM_ON")
        self.monitor.handle_serial_line(connection, "NOT_ALARM_OFF")
        self.assertFalse(self.monitor.alarm_active)

        self.monitor.handle_serial_line(connection, "ALARM_ON")
        self.assertTrue(self.monitor.alarm_active)
        self.monitor.handle_serial_line(connection, "ALARM_OFF")
        self.assertFalse(self.monitor.alarm_active)

        valid = "STATUS:SCHARF=1,AUSGELOEST=1,TUER1=ZU,TUER2=OFFEN,VERBINDUNG=OK"
        self.monitor.handle_serial_line(connection, valid)
        self.assertTrue(self.monitor.alarm_active)
        impossible = "STATUS:SCHARF=0,AUSGELOEST=1,TUER1=ZU,TUER2=ZU,VERBINDUNG=OK"
        self.assertFalse(self.monitor._apply_status_line(impossible))

    def test_serial_decoder_rejects_nul_non_ascii_and_overlong_lines(self):
        self.assertIsNone(self.monitor._decode_serial_line(b"ALARM_ON\x00junk\n"))
        self.assertIsNone(self.monitor._decode_serial_line(b"ALARM_ON\xff\n"))
        self.assertIsNone(self.monitor._decode_serial_line(b"A" * 257 + b"\n"))
        self.assertEqual(self.monitor._decode_serial_line(b"ALARM_ON\r\n"), "ALARM_ON")

        connection = FakeConnection([b"A" * 257, b"ALARM_ON\n", b"ALARM_OFF\n"])
        self.assertIsNone(self.monitor._read_line(connection))
        self.assertIsNone(self.monitor._read_line(connection))
        self.assertEqual(self.monitor._read_line(connection), "ALARM_OFF")

    def test_connect_rejects_failed_handshake_and_tries_next_device(self):
        self.config.serial_ports = ("/dev/ttyUSB0", "/dev/ttyACM0")
        first = FakeConnection()
        second = FakeConnection()
        with mock.patch.object(alarm_monitor.os.path, "exists", return_value=True), \
             mock.patch.object(
                 self.monitor, "_open_serial_port", side_effect=[first, second]
             ) as open_port, \
             mock.patch.object(self.monitor, "_handshake", side_effect=[False, True]):
            result = self.monitor.connect_serial()

        self.assertIs(result, second)
        self.assertEqual(open_port.call_count, 2)
        self.assertTrue(first.closed)
        self.assertTrue(self.monitor.serial_connected)
        self.assertEqual(self.monitor.serial_port, "/dev/ttyACM0")

    def test_handshake_accepts_only_status_snapshot_and_answers_heartbeat(self):
        connection = FakeConnection(
            [
                b"noise\n",
                b"HB\n",
                b"STATUS:SCHARF=1,AUSGELOEST=1,TUER1=ZU,TUER2=ZU,VERBINDUNG=OK\n",
            ]
        )
        self.monitor.reconcile_recording = mock.Mock()
        self.monitor.publish_runtime_status = mock.Mock()
        self.assertTrue(self.monitor._handshake(connection))
        self.assertIn(b"STATUS\n", connection.writes)
        self.assertIn(b"HB_ACK\n", connection.writes)
        self.assertTrue(self.monitor.alarm_active)

    def test_status_file_is_valid_json_after_atomic_replace(self):
        self.monitor.serial_connected = True
        self.monitor.serial_port = "/dev/ttyACM0"
        self.monitor.update_status("idle")
        with open(self.config.status_file, encoding="utf-8") as handle:
            status = json.load(handle)
        self.assertEqual(status["state"], "idle")
        self.assertTrue(status["serial_connected"])
        self.assertEqual(status["serial_port"], "/dev/ttyACM0")

    def test_configuration_rejects_unsafe_paths_and_limits(self):
        bad = dict(self.environ, ALARM_SERIAL_PORTS="/tmp/not-a-device;touch-pwned")
        with self.assertRaises(alarm_monitor.ConfigurationError):
            alarm_monitor.MonitorConfig(bad)
        bad = dict(self.environ, ALARM_IPC_SOCKET="relative.sock")
        with self.assertRaises(alarm_monitor.ConfigurationError):
            alarm_monitor.MonitorConfig(bad)
        bad = dict(self.environ, ALARM_RECONNECT_MIN_SECONDS="30", ALARM_RECONNECT_MAX_SECONDS="2")
        with self.assertRaises(alarm_monitor.ConfigurationError):
            alarm_monitor.MonitorConfig(bad)

    def test_ipc_request_rejects_expiry_replay_unknown_fields_and_bad_filename(self):
        expired = self.request("status")
        expired["issued_at"] -= 30
        expired["expires_at"] -= 30
        with self.assertRaisesRegex(ValueError, "expired"):
            self.monitor.execute_ipc_request(expired)

        valid = self.request("status")
        self.monitor.execute_ipc_request(valid)
        with self.assertRaisesRegex(ValueError, "replayed"):
            self.monitor.execute_ipc_request(valid)

        unknown = self.request("status", extra="no")
        with self.assertRaisesRegex(ValueError, "unknown"):
            self.monitor.execute_ipc_request(unknown)

        traversal = self.request("delete_recording", filename="../alarm_20260101_000000.avi")
        with self.assertRaisesRegex(ValueError, "invalid filename"):
            self.monitor.execute_ipc_request(traversal)

    def test_ipc_arm_disarm_requires_validated_serial_connection(self):
        connection = FakeConnection(
            [
                b"STATUS:SCHARF=1,AUSGELOEST=0,TUER1=ZU,TUER2=ZU,VERBINDUNG=OK\n",
                b"STATUS:SCHARF=0,AUSGELOEST=0,TUER1=ZU,TUER2=ZU,VERBINDUNG=OK\n",
            ]
        )
        with self.assertRaisesRegex(RuntimeError, "nicht verbunden"):
            self.monitor.execute_ipc_request(self.request("arm"), connection)
        self.monitor.serial_connected = True
        self.monitor.execute_ipc_request(self.request("arm"), connection)
        self.monitor.execute_ipc_request(self.request("disarm"), connection)
        self.assertEqual(
            connection.writes,
            [b"SCHARF\n", b"STATUS\n", b"UNSCHARF\n", b"STATUS\n"],
        )

        impossible = FakeConnection(
            [b"STATUS:SCHARF=0,AUSGELOEST=1,TUER1=ZU,TUER2=ZU,VERBINDUNG=OK\n"]
        )
        with self.assertRaisesRegex(RuntimeError, "Bestaetigung"):
            self.monitor.execute_ipc_request(self.request("disarm"), impossible)

    def test_unix_socket_returns_bound_response(self):
        if not hasattr(socket, "AF_UNIX"):
            self.skipTest("Unix sockets unavailable")
        self.monitor.setup_ipc()
        request = self.request("status")
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            client.connect(self.config.ipc_socket)
            client.sendall(json.dumps(request).encode("utf-8") + b"\n")
            self.monitor.handle_ipc(None)
            response = json.loads(client.recv(8192).decode("utf-8"))
        finally:
            client.close()
        self.assertTrue(response["ok"])
        self.assertEqual(response["id"], request["id"])

    def test_slow_ipc_client_cannot_starve_main_loop(self):
        client = SlowClient()
        self.monitor._ipc_server = OneClientServer(client)
        started = time.monotonic()
        self.monitor.handle_ipc(None)
        elapsed = time.monotonic() - started
        self.assertLess(elapsed, 0.30)
        response = json.loads(client.response.decode("ascii"))
        self.assertFalse(response["ok"])

    def test_active_recording_cannot_be_deleted_and_delete_all_skips_it(self):
        os.makedirs(self.config.record_dir)
        active = os.path.join(self.config.record_dir, "alarm_20260101_000000.avi")
        other = os.path.join(self.config.record_dir, "manual_20260101_000001.avi")
        pathlib.Path(active).write_bytes(b"active")
        pathlib.Path(other).write_bytes(b"other")
        self.monitor.current_file = active
        with self.assertRaisesRegex(RuntimeError, "Aktive"):
            self.monitor.execute_ipc_request(
                self.request("delete_recording", filename=os.path.basename(active))
            )
        result = self.monitor.execute_ipc_request(self.request("delete_all_recordings"))
        self.assertEqual(result["deleted"], 1)
        self.assertTrue(os.path.exists(active))
        self.assertFalse(os.path.exists(other))

    def test_recording_has_time_size_and_no_overwrite_limits(self):
        process = FakeProcess()
        with mock.patch.object(
            alarm_monitor.shutil,
            "disk_usage",
            return_value=mock.Mock(free=5 * 1024 * 1024 * 1024),
        ), mock.patch.object(alarm_monitor.subprocess, "Popen", return_value=process) as popen:
            self.assertTrue(self.monitor.start_recording("alarm"))
        command = popen.call_args.args[0]
        self.assertIn("-n", command)

        self.assertIn("-t", command)
        self.assertIn("-fs", command)
        self.assertNotIn("-y", command)
        size_limit = int(command[command.index("-fs") + 1])
        expected_maximum = (
            5 * 1024 * 1024 * 1024
            - (self.config.min_free_mib + self.config.disk_safety_mib)
            * 1024
            * 1024
        )
        self.assertLessEqual(size_limit, expected_maximum)

    def test_runtime_log_can_only_be_cleared_through_ipc(self):
        pathlib.Path(self.config.log_file).write_text("current\n", encoding="utf-8")
        pathlib.Path(self.config.log_file + ".1").write_text("backup\n", encoding="utf-8")
        result = self.monitor.execute_ipc_request(self.request("clear_runtime_log"))
        self.assertEqual(result["cleared"], 2)
        self.assertEqual(pathlib.Path(self.config.log_file).read_text(encoding="utf-8"), "")
        self.assertFalse(os.path.exists(self.config.log_file + ".1"))

    def test_early_ffmpeg_exit_zero_or_nonzero_retries_but_cutoff_does_not(self):
        self.monitor.alarm_active = True
        self.monitor.recording_process = FakeProcess(return_code=1)
        self.monitor.current_file = "alarm_20260101_000000.avi"
        self.monitor.recording_reason = "alarm"
        self.monitor.recording_started = time.monotonic()
        self.monitor.reap_recording()
        self.assertEqual(self.monitor.recording_retry_count, 1)
        self.assertFalse(self.monitor.recording_cutoff_reached)

        self.monitor.recording_process = FakeProcess(return_code=0)
        self.monitor.current_file = "alarm_20260101_000001.avi"
        self.monitor.recording_reason = "alarm"
        self.monitor.recording_started = time.monotonic()
        self.monitor.reap_recording()
        self.assertEqual(self.monitor.recording_retry_count, 2)
        self.assertFalse(self.monitor.recording_cutoff_reached)

        self.monitor.recording_process = FakeProcess(return_code=0)
        self.monitor.current_file = "alarm_20260101_000002.avi"
        self.monitor.recording_reason = "alarm"
        self.monitor.recording_started = (
            time.monotonic() - self.config.max_recording_seconds
        )
        self.monitor.reap_recording()
        self.assertTrue(self.monitor.recording_cutoff_reached)

    def test_stubborn_ffmpeg_does_not_abort_cleanup(self):
        self.monitor.recording_process = StubbornProcess()
        self.monitor.current_file = "alarm_20260101_000000.avi"
        self.monitor.recording_reason = "alarm"
        self.monitor.stop_recording()
        self.assertIsNone(self.monitor.recording_process)

    def test_log_rotation_bounds_file_growth(self):
        self.config.log_max_kib = 1
        for _ in range(12):
            self.monitor.log("x" * 400)
        self.assertLessEqual(os.path.getsize(self.config.log_file), 1024)
        self.assertTrue(os.path.exists(self.config.log_file + ".1"))


if __name__ == "__main__":
    unittest.main()
