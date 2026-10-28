"""Real WSL helper sockets and anonymous pipes; no Docker or network required."""
import sys
sys.dont_write_bytecode = True
import json
import os
from pathlib import Path
import select
import shlex
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "integration/ai/scripts"
HELPER = Path(os.environ.get("POPPY_TEST_WSL_HELPER", str(ROOT / "out/docker/wsl/poppy-wsl-helper")))


@unittest.skipUnless(HELPER.is_file(), "Build the WSL helper first")
class RelayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="poppy-relay-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.server, self.socket = self.start_server()
        self.environment = dict(os.environ, AICONTAINER_EVENT_SOCKET=str(self.socket))

    def start_server(self):
        process = subprocess.Popen([str(HELPER), "--events"], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        self.addCleanup(self.stop, process)
        name = Path(self.frame(process)["socket"])
        return process, name

    def frame(self, process=None):
        process = process or self.server
        def read(size):
            result = b""
            deadline = time.monotonic() + 3
            while len(result) < size:
                self.assertTrue(select.select([process.stdout], [], [], max(0, deadline - time.monotonic()))[0],
                                "Notification frame timed out")
                value = os.read(process.stdout.fileno(), size - len(result))
                self.assertTrue(value, "Notification stream ended")
                result += value
            return result
        size = struct.unpack(">I", read(4))[0]
        self.assertLessEqual(size, 65537)
        frame = read(size)
        self.assertEqual(frame[0], 1)
        return json.loads(frame[1:])

    def stop(self, process):
        if process.poll() is not None:
            for stream in (process.stdin, process.stdout, process.stderr):
                if stream:
                    stream.close()
            return
        if process.stdin:
            process.stdin.close(); process.stdin = None
        try:
            _, diagnostics = process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill(); process.communicate()
            self.fail("Relay did not stop after stdio EOF")
        self.assertEqual(process.returncode, 0, diagnostics.decode(errors="replace"))

    def notify(self, value):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(3); connection.connect(str(self.socket)); connection.sendall(value)
            try:
                return connection.recv(16)
            except ConnectionResetError:
                return b""

    def valid(self):
        return b'{"type":"status","body":"{\\"status\\":\\"running\\"}"}\n'

    def test_installed_hooks_need_no_poppy_environment(self):
        for tool in ("codex", "claude"):
            target = self.root / tool
            subprocess.run([sys.executable, str(SCRIPTS / f"install-{tool}-hooks.py"), f"--{tool}-dir", str(target)],
                           check=True, capture_output=True)
            filename = "hooks.json" if tool == "codex" else "settings.json"
            hooks = json.loads((target / filename).read_text())["hooks"]
            transcript = target / "transcript.jsonl"
            transcript.write_text(json.dumps({"type": "assistant", "message": {"role": "assistant", "content": "処理中です。"}}), encoding="utf-8")
            for name, data, kind, body in (
                ("UserPromptSubmit", {}, "status", {"status": "running"}),
                ("PostToolUse", {}, "status", {"status": "running", "onlyIfActive": True}),
                ("PreToolUse", {"transcript_path": str(transcript)}, "activity", {"activityText": "処理中です。"}),
                ("Stop", {"last_assistant_message": "完了🙂\nしました。"}, "status",
                 {"status": "completed", "activityText": "完了🙂 しました。"}),
            ):
                command = hooks[name][0]["hooks"][0]["command"]
                result = subprocess.run(shlex.split(command), input=json.dumps(data), text=True, capture_output=True,
                                        env=self.environment, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, ""); self.assertEqual(result.stderr, "")
                event = self.frame()
                self.assertEqual(event["type"], kind); self.assertEqual(json.loads(event["body"]), body)
                self.assertNotIn("sessionId", event)

    def test_stdio_eof_removes_socket_and_private_directory(self):
        self.assertTrue(self.socket.exists())
        self.stop(self.server)
        self.assertFalse(self.socket.exists())
        self.assertFalse(self.socket.parent.exists())

    def test_invalid_and_oversized_events_leave_relay_usable(self):
        oversized = json.dumps({"type": "status", "body": json.dumps({"status": "running", "pad": "x" * 8192})}).encode() + b"\n"
        for value in (b'not-json\n', b'\xff\n', b'x' * 16385, oversized,
                      b'{"type":"status","body":"{}"}\n', b'{"type":"other","body":"{}"}\n'):
            self.assertEqual(self.notify(value), b"")
        self.assertEqual(self.notify(self.valid()), b"OK\n")
        self.assertEqual(json.loads(self.frame()["body"]), {"status": "running"})

    def fill(self, value, limit):
        count = 0
        while count < limit:
            answer = self.notify(value)
            if answer == b"BUSY\n":
                return count
            self.assertEqual(answer, b"OK\n")
            count += 1
        self.fail("Unbounded notification queue")

    def test_count_budget_and_backpressure_recovery(self):
        count = self.fill(self.valid(), 2300)
        self.assertGreaterEqual(count, 1024)
        result = subprocess.run([sys.executable, str(SCRIPTS / "poppy_status.py"), "running", "1"],
                                input="{}", text=True, capture_output=True, env=self.environment, timeout=3)
        self.assertEqual(result.returncode, 0)
        self.assertIn("通知リレー", result.stderr)
        for _ in range(count):
            self.assertEqual(self.frame()["type"], "status")
        self.assertEqual(self.notify(self.valid()), b"OK\n")
        self.frame()

    def test_byte_budget(self):
        body = {"status": "running", **{f"pad{i}": "x" * 500 for i in range(15)}}
        value = json.dumps({"type": "status", "body": json.dumps(body)}).encode() + b"\n"
        count = self.fill(value, 200)
        self.assertGreater(count, 100)
        self.assertLess(count, 150)

    def test_fragmented_sender_and_slow_client(self):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as slow:
            slow.connect(str(self.socket))
            self.assertEqual(self.socket.parent.stat().st_mode & 0o777, 0o700)
            self.assertEqual(self.socket.stat().st_mode & 0o777, 0o666)
            self.assertEqual(self.notify(self.valid()), b"OK\n")
            self.frame()
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(3); connection.connect(str(self.socket))
            value = self.valid()
            connection.sendall(value[:12])
            self.assertFalse(select.select([connection], [], [], 0.05)[0])
            connection.sendall(value[12:])
            self.assertEqual(connection.recv(16), b"OK\n")
            self.frame()

    def test_other_terminal_survives_closing_one(self):
        other, other_socket = self.start_server()
        self.assertNotEqual(other_socket, self.socket)
        self.stop(self.server)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(3); connection.connect(str(other_socket)); connection.sendall(self.valid())
            self.assertEqual(connection.recv(16), b"OK\n")
        self.assertEqual(json.loads(self.frame(other)["body"]), {"status": "running"})

    def test_output_disconnect_also_cleans_socket(self):
        self.server.stdout.close()
        self.assertEqual(self.server.wait(timeout=3), 0)
        self.assertFalse(self.socket.exists())
        self.server.stdin.close(); self.server.stderr.close()

    def test_socket_failure_is_nonfatal(self):
        environment = dict(self.environment, AICONTAINER_EVENT_SOCKET=str(self.root / "missing"))
        result = subprocess.run([sys.executable, str(SCRIPTS / "poppy_status.py"), "running", "1"],
                                input="{}", text=True, capture_output=True, env=environment, timeout=3)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("通知リレー", result.stderr)



if __name__ == "__main__":
    unittest.main()
