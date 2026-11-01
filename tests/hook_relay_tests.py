"""Real WSL helper sockets and anonymous pipes; no Docker or network required."""
import sys
sys.dont_write_bytecode = True
import json
import hashlib
import fcntl
import os
from pathlib import Path
import select
import re
import shlex
import socket
import signal
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

    def start_server(self, bootstrap_home=None, extra_environment=None):
        command = [str(HELPER), "--events"]
        environment = None
        binary = b""
        if bootstrap_home is not None:
            binary = HELPER.read_bytes()
            script = (ROOT / "src/wsl_bootstrap.h").read_text().split('R"SH(', 1)[1].split(')SH"', 1)[0]
            version = re.search(r'helperVersion = "([^"]+)"', (ROOT / "src/wsl_protocol.h").read_text()).group(1)
            command = ["/bin/sh", "-c", script, "poppy-bootstrap", version,
                       hashlib.sha256(binary).hexdigest(), str(len(binary)), "--events"]
            environment = dict(os.environ, HOME=str(bootstrap_home))
        if extra_environment:
            environment = dict(environment or os.environ, **extra_environment)
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, bufsize=0, env=environment)
        self.addCleanup(self.stop, process)
        if binary:
            os.set_blocking(process.stdin.fileno(), False)
            offset = 0
            deadline = time.monotonic() + 5
            while offset < len(binary):
                self.assertTrue(select.select([], [process.stdin], [], max(0, deadline - time.monotonic()))[1],
                                "Notification helper upload timed out")
                try:
                    offset += os.write(process.stdin.fileno(), binary[offset:offset + 65536])
                except BlockingIOError:
                    continue
            os.set_blocking(process.stdin.fileno(), True)
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

    def test_cleanup_waits_for_only_the_containers_with_this_socket(self):
        docker = self.root / "docker"
        docker.write_text(f"#!{sys.executable}\n" + '''
import json
import os
from pathlib import Path
import sys
import time

root = Path(os.environ["POPPY_DOCKER_FIXTURE"])
args = sys.argv[1:]
with (root / "calls.jsonl").open("a") as output:
    output.write(json.dumps(args) + "\\n")
owned, sibling = "a" * 64, "b" * 64
if args == ["ps", "--all", "--quiet", "--no-trunc"]:
    if not (root / "query-release").exists():
        (root / "querying").touch()
        sys.exit(1)
    if not (root / "removed").exists():
        print(owned)
    print(sibling)
elif args[:2] == ["inspect", "--format"]:
    assert str((root / "socket").read_text()) in args[2]
    if owned in args[3:]:
        print(owned)
elif args == ["stop", "--time", "1", owned]:
    (root / "stopping").touch()
    while not (root / "release").exists():
        time.sleep(0.01)
    (root / "stopped").touch()
elif args == ["rm", "--force", owned]:
    assert (root / "stopped").exists()
    (root / "removed").touch()
else:
    raise AssertionError(args)
''')
        docker.chmod(0o755)
        process, filename = self.start_server(extra_environment={
            "PATH": str(self.root) + os.pathsep + os.environ.get("PATH", ""),
            "POPPY_DOCKER_FIXTURE": str(self.root),
        })
        self.addCleanup(lambda: ((self.root / "release").touch(), (self.root / "query-release").touch()))
        (self.root / "socket").write_text(str(filename))
        process.stdin.close(); process.stdin = None
        deadline = time.monotonic() + 3
        while not (self.root / "querying").exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue((self.root / "querying").exists())
        self.assertIsNone(process.poll(), "Docker query failure was treated as successful cleanup")
        (self.root / "query-release").touch()
        deadline = time.monotonic() + 3
        while not (self.root / "stopping").exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue((self.root / "stopping").exists(), "The relay did not request container shutdown")
        self.assertIsNone(process.poll(), "The relay exited before the container stopped")
        (self.root / "release").touch()
        self.stop(process)
        self.assertTrue((self.root / "stopped").exists())
        self.assertTrue((self.root / "removed").exists())
        calls = [json.loads(line) for line in (self.root / "calls.jsonl").read_text().splitlines()]
        self.assertEqual([call for call in calls if call[0] == "stop"], [["stop", "--time", "1", "a" * 64]])

    def test_cleanup_stops_wsl_descendants_and_releases_locks(self):
        other, other_socket = self.start_server()
        script = self.root / "process.py"
        script.write_text('''
import fcntl
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

root = Path(sys.argv[1])
if len(sys.argv) == 3:
    os.setsid()
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    lock = (root / "lock").open("w")
    fcntl.flock(lock, fcntl.LOCK_EX)
    (root / "pid").write_text(str(os.getpid()))
else:
    environment = dict(os.environ)
    environment.pop("AICONTAINER_EVENT_SOCKET", None)
    child = subprocess.Popen([sys.executable, __file__, str(root), "child"], env=environment)
while True:
    time.sleep(1)
''')
        parent = subprocess.Popen([sys.executable, str(script), str(self.root)],
                                  env=self.environment, stdin=subprocess.DEVNULL)
        sibling = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"],
                                   env=dict(os.environ, AICONTAINER_EVENT_SOCKET=str(other_socket)))
        self.addCleanup(sibling.wait)
        self.addCleanup(lambda: sibling.poll() is None and sibling.kill())
        self.addCleanup(lambda: parent.poll() is None and parent.kill())
        deadline = time.monotonic() + 3
        while not (self.root / "pid").exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue((self.root / "pid").exists(), "WSL descendant did not acquire the lock")
        child = int((self.root / "pid").read_text())
        self.addCleanup(lambda: self.kill_if_alive(child))
        self.stop(self.server)
        parent.wait(timeout=3)
        self.assertIsNone(sibling.poll(), "Closing one terminal stopped a sibling")
        with (self.root / "lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.assertFalse(self.alive(child), "A detached WSL descendant survived shutdown")
        self.stop(other)
        sibling.wait(timeout=3)

    @staticmethod
    def alive(pid):
        try:
            return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0] not in ("Z", "X")
        except FileNotFoundError:
            return False

    @classmethod
    def kill_if_alive(cls, pid):
        if cls.alive(pid):
            os.kill(pid, signal.SIGKILL)

    def test_bootstrap_installs_and_reuses_standalone_notification_helper(self):
        home = self.root / "home 日本語 ' $"
        home.mkdir()
        modified = None
        for _ in range(2):
            process, filename = self.start_server(home)
            installed = list(home.glob(".local/share/yumayo/Poppy/helpers/*/poppy-wsl-helper"))
            self.assertEqual(len(installed), 1)
            self.assertEqual(installed[0].read_bytes(), HELPER.read_bytes())
            if modified is not None:
                self.assertEqual(installed[0].stat().st_mtime_ns, modified)
            modified = installed[0].stat().st_mtime_ns
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(3); connection.connect(str(filename)); connection.sendall(self.valid())
                self.assertEqual(connection.recv(16), b"OK\n")
            self.assertEqual(json.loads(self.frame(process)["body"]), {"status": "running"})
            self.stop(process)
            self.assertFalse(filename.exists())
            self.assertFalse(filename.parent.exists())

    def test_bootstrap_rejects_unsupported_mode_before_creating_files(self):
        script = (ROOT / "src/wsl_bootstrap.h").read_text().split('R"SH(', 1)[1].split(')SH"', 1)[0]
        result = subprocess.run(["/bin/sh", "-c", script, "poppy-bootstrap", "test", "test", "0", "--unsupported"],
                                env=dict(os.environ, HOME=str(self.root)), input=b"", capture_output=True, timeout=3)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"Unsupported Poppy helper mode", result.stderr)
        self.assertFalse((self.root / ".local").exists())

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
