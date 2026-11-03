"""Offline tests for Poppy notifications and reversible hook installation."""
import sys
sys.dont_write_bytecode = True
import importlib.util
import fcntl
import json
import os
from pathlib import Path
import queue
import pty
import select
import shlex
import socket
import subprocess
import tempfile
import threading
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1] / "integration" / "ai" / "scripts"


def module(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / (name + ".py"))
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


sender = module("poppy_status")


class LogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="poppy-log-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.script = self.root / "poppy_status.py"
        self.script.write_bytes((SCRIPTS / self.script.name).read_bytes())
        self.log = self.script.with_suffix(".log")
        patch = mock.patch.object(sender, "__file__", str(self.script))
        patch.start()
        self.addCleanup(patch.stop)

    def emit(self, action):
        with mock.patch.object(sys, "argv", [str(self.script), action]):
            sender.log_invocation()

    def test_rotates_before_exceeding_the_utf8_byte_limit(self):
        self.emit("日本語😀")
        entry_size = self.log.stat().st_size
        self.assertEqual(sender.MAX_LOG_BYTES, 10 * 1024 * 1024)
        with self.log.open("wb") as stream:
            stream.truncate(sender.MAX_LOG_BYTES - entry_size)
        self.emit("日本語😀")
        self.assertEqual(self.log.stat().st_size, sender.MAX_LOG_BYTES)
        self.assertFalse(self.log.with_name(self.log.name + ".1").exists())
        self.emit("日本語😀")
        self.assertEqual(self.log.with_name(self.log.name + ".1").stat().st_size, sender.MAX_LOG_BYTES)
        self.assertEqual(json.loads(self.log.read_text())["args"], ["日本語😀"])

    def test_keeps_the_latest_ten_backups_in_order(self):
        self.emit("00")
        with mock.patch.object(sender, "MAX_LOG_BYTES", self.log.stat().st_size):
            for index in range(1, 15):
                self.emit(f"{index:02d}")
        self.assertEqual(json.loads(self.log.read_text())["args"], ["14"])
        for index in range(1, 11):
            backup = self.log.with_name(f"{self.log.name}.{index}")
            self.assertEqual(json.loads(backup.read_text())["args"], [f"{14 - index:02d}"])
        backups = [path for path in self.root.glob("poppy_status.log.*") if path.suffix != ".lock"]
        self.assertEqual(len(backups), 10)

    def test_parallel_hooks_preserve_records_during_rotation(self):
        code = ("import sys\n"
                "sys.path.insert(0, sys.argv[1])\n"
                "import poppy_status\n"
                "poppy_status.MAX_LOG_BYTES = 1024\n"
                "sys.argv = ['poppy_status.py', sys.argv[2]]\n"
                "poppy_status.log_invocation()\n")
        processes = []
        try:
            with self.log.with_name(self.log.name + ".lock").open("a") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                for index in range(24):
                    processes.append(subprocess.Popen(
                        [sys.executable, "-c", code, str(self.root), str(index)],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    ))
            for process in processes:
                _, error = process.communicate(timeout=10)
                self.assertEqual(process.returncode, 0, error.decode())
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                process.communicate()
        records = []
        logs = [path for path in self.root.glob("poppy_status.log*") if path.suffix != ".lock"]
        self.assertGreater(len(logs), 1)
        for path in logs:
            self.assertLessEqual(path.stat().st_size, 1024)
            records.extend(json.loads(line)["args"][-1] for line in path.read_text().splitlines())
        self.assertEqual(len(records), 24)
        self.assertEqual(set(records), {str(index) for index in range(24)})


class HookDeliveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="poppy-hook-socket-")
        self.addCleanup(self.temp.cleanup)
        self.requests = queue.Queue()
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.filename = str(Path(self.temp.name) / "events.sock")
        self.server.bind(self.filename); self.server.listen(4); self.server.settimeout(0.05)
        self.stopping = threading.Event()
        def receive():
            while not self.stopping.is_set():
                try:
                    connection, _ = self.server.accept()
                except socket.timeout:
                    continue
                with connection:
                    connection.settimeout(2)
                    with connection.makefile("rb") as stream:
                        event = json.loads(stream.readline(16385))
                    self.requests.put((event["type"], json.loads(event["body"])))
                    connection.sendall(b"OK\n")
        self.thread = threading.Thread(target=receive)
        self.thread.start()
        self.environment = {**os.environ, "AICONTAINER_EVENT_SOCKET": self.filename}

    def tearDown(self):
        self.stopping.set(); self.thread.join(timeout=3); self.server.close()
        self.assertFalse(self.thread.is_alive(), "Hook test receiver did not stop")

    def invoke(self, command, data, environment=None):
        subprocess.run(
            shlex.split(command), input=json.dumps(data), text=True,
            env=self.environment if environment is None else environment,
            capture_output=True, check=True, timeout=10,
        )

    def check_tool(self, tool):
        with tempfile.TemporaryDirectory(prefix="poppy hooks 日本語 ' ") as directory:
            target = Path(directory)
            module(f"install-{tool}-hooks").install(target)
            config = target / ("hooks.json" if tool == "codex" else "settings.json")
            hooks = json.loads(config.read_text(encoding="utf-8"))["hooks"]
            transcript = target / "transcript.jsonl"
            transcript.write_text(json.dumps({
                "type": "assistant", "message": {"role": "assistant", "content": "設定を確認しています。"},
            }), encoding="utf-8")
            for name, data, endpoint, payload in (
                ("UserPromptSubmit", {}, "status", {"status": "running"}),
                ("PostToolUse", {}, "status", {"status": "running", "onlyIfActive": True}),
                ("PreToolUse", {"transcript_path": str(transcript)}, "activity", {"activityText": "設定を確認しています。"}),
                ("Stop", {"last_assistant_message": "完了しました。"}, "status", {"status": "completed", "activityText": "完了しました。"}),
            ):
                with self.subTest(tool=tool, event=name):
                    self.invoke(hooks[name][0]["hooks"][0]["command"], data)
                    self.assertEqual(self.requests.get(timeout=1), (
                        endpoint, payload,
                    ))

    def test_codex_hooks_deliver_to_poppy(self):
        self.check_tool("codex")

    def test_claude_hooks_deliver_to_poppy(self):
        self.check_tool("claude")

    def test_missing_socket_does_not_send(self):
        with mock.patch.dict(os.environ, {}, clear=True), \
                mock.patch.object(sender, "MOUNTED_SOCKET", Path(self.temp.name) / "missing.sock"), \
                mock.patch.object(sys, "argv", ["poppy_status.py", "running"]), \
                mock.patch.object(sender, "report_error") as report:
            self.assertEqual(sender.main(), 0)
        self.assertEqual(report.call_args.args[0], "missing-event-socket")
        self.assertTrue(self.requests.empty())

    def test_missing_environment_recovers_only_the_explicit_mounted_socket(self):
        with mock.patch.dict(os.environ, {}, clear=True), \
                mock.patch.object(sender, "MOUNTED_SOCKET", Path(self.filename)), \
                mock.patch.object(sys, "argv", ["poppy_status.py", "running"]), \
                mock.patch.object(sender, "read_hook_input", return_value={}), \
                mock.patch.object(sender, "log_invocation"), \
                mock.patch.object(sender, "report_error") as report:
            self.assertEqual(sender.main(), 0)
        self.assertEqual(report.call_args.args[0], "event-socket-recovered")
        self.assertEqual(self.requests.get(timeout=1), ("status", {"status": "running"}))

    def test_explicit_socket_is_not_replaced_by_another_mount(self):
        with mock.patch.dict(os.environ, {"AICONTAINER_EVENT_SOCKET": "/missing/explicit.sock"}), \
                mock.patch.object(sender, "MOUNTED_SOCKET", Path(self.filename)):
            self.assertEqual(sender.notification_socket(), "/missing/explicit.sock")

    def test_failed_delivery_reports_an_error_without_failing_the_hook(self):
        with mock.patch.dict(os.environ, {"AICONTAINER_EVENT_SOCKET": "/missing/explicit.sock"}), \
                mock.patch.object(sys, "argv", ["poppy_status.py", "running"]), \
                mock.patch.object(sender, "read_hook_input", return_value={}), \
                mock.patch.object(sender, "send_relay", side_effect=OSError("relay unavailable")), \
                mock.patch.object(sender, "report_error") as report:
            self.assertEqual(sender.main(), 0)
        self.assertEqual(report.call_args.args[0], "relay-send-failed")

    def test_rotation_failure_does_not_block_notification(self):
        script = Path(self.temp.name) / "poppy_status.py"
        script.write_bytes((SCRIPTS / script.name).read_bytes())
        log = script.with_suffix(".log")
        with log.open("wb") as stream:
            stream.truncate(sender.MAX_LOG_BYTES)
        # A directory at the next backup path makes the rename fail.
        log.with_name(log.name + ".1").mkdir()
        command = shlex.join([sys.executable, str(script), "running"])
        self.invoke(command, {})
        self.assertEqual(self.requests.get(timeout=1), ("status", {"status": "running"}))



class SenderTests(unittest.TestCase):
    def test_diagnostic_uses_tty_when_hook_output_is_captured(self):
        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        with mock.patch.object(sender, "log_invocation") as log, \
                mock.patch.object(sender.os, "open", return_value=slave), \
                mock.patch("sys.stderr"):
            sender.report_error("missing-event-socket", "接続先が未設定です")
        self.assertTrue(select.select([master], [], [], 1)[0])
        self.assertEqual(os.read(master, 1024), b"\x1b]777;poppy;error;missing-event-socket\x07")
        self.assertEqual(log.call_args.kwargs["error"], "missing-event-socket")

    def test_diagnostic_failure_does_not_interrupt_ai(self):
        with mock.patch.object(sender, "log_invocation"), \
                mock.patch.object(sender.os, "open", side_effect=OSError("no terminal")), \
                mock.patch("sys.stderr"):
            sender.report_error("relay-send-failed", "送信できません")

    def test_detached_monitor_can_use_its_own_cli_terminal(self):
        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        with mock.patch.object(sender, "log_invocation"), \
                mock.patch.object(sender.os, "open", side_effect=[OSError("no controlling tty"), slave]) as opened, \
                mock.patch("sys.stderr"):
            sender.report_error("relay-send-failed", "送信できません", tty_pid=123)
        self.assertEqual(opened.call_args.args[0], "/proc/123/fd/0")
        self.assertTrue(select.select([master], [], [], 1)[0])
        self.assertEqual(os.read(master, 1024), b"\x1b]777;poppy;error;relay-send-failed\x07")

    def test_resume_is_conditional(self):
        self.assertEqual(sender.build_payload("resume", {}), {"status": "running", "onlyIfActive": True})

    def test_recoverable_tool_failure_is_not_session_failure(self):
        self.assertEqual(sender.build_payload("tool-failed", {}), {"status": "running", "onlyIfActive": True})
        self.assertEqual(sender.build_payload("tool-failed", {"is_interrupt": True}), {"status": "interrupted", "onlyIfActive": True})

    def test_claude_background_work_is_not_completed(self):
        for status in ("running", "pending"):
            result = sender.build_payload("completed", {"hook_event_name": "Stop", "background_tasks": [{"status": status}]})
            self.assertEqual(result, {"status": "running", "onlyIfActive": True})

    def test_stop_and_activity_are_distinct(self):
        self.assertEqual(sender.build_payload("completed", {"last_assistant_message": "done"}), {"status": "completed", "activityText": "done"})
        self.assertEqual(sender.build_payload("activity", {}), {})
        self.assertEqual(sender.build_payload("none", {"last_assistant_message": "old"}), {"status": "none"})

    def test_text_is_bounded_and_public(self):
        self.assertLessEqual(len(sender.excerpt("a" * 1000)), 120)
        with tempfile.TemporaryDirectory() as directory:
            transcript = Path(directory) / "transcript.jsonl"
            transcript.write_text("\n".join(json.dumps(value) for value in (
                {"type": "assistant", "message": {"role": "assistant", "content": "public"}},
                {"type": "assistant", "isSidechain": True, "message": {"role": "assistant", "content": "private child"}},
                {"type": "response_item", "payload": {"role": "assistant", "recipient": "tool", "content": "tool input"}},
            )), encoding="utf-8")
            self.assertEqual(sender.latest_message(str(transcript)), "public")


class InstallTests(unittest.TestCase):
    def check_tool(self, tool):
        installer = module("install-" + tool + "-hooks")
        uninstaller = module("uninstall-hooks")
        with tempfile.TemporaryDirectory(prefix="poppy 日本語 ' ") as directory:
            target = Path(directory)
            filename = target / ("hooks.json" if tool == "codex" else "settings.json")
            original = {"custom": {"preserve": True}, "hooks": {"Stop": [{"hooks": [{"type": "command", "command": "echo custom"}]}]}}
            filename.write_text(json.dumps(original), encoding="utf-8")
            installer.install(target)
            once = json.loads(filename.read_text(encoding="utf-8"))
            installer.install(target)
            self.assertEqual(json.loads(filename.read_text(encoding="utf-8")), once)
            self.assertTrue((target / "hooks/poppy/poppy_status.py").is_file())
            if tool == "codex":
                self.assertTrue((target / "hooks/poppy/poppy_codex_poll.py").is_file())
            self.assertEqual(once["custom"], original["custom"])
            self.assertIn(original["hooks"]["Stop"][0], once["hooks"]["Stop"])
            self.assertTrue(list(target.glob("*.poppy-*.bak")))
            uninstaller.uninstall(target, tool)
            restored = json.loads(filename.read_text(encoding="utf-8"))
            self.assertEqual(restored["custom"], original["custom"])
            self.assertEqual(restored["hooks"]["Stop"], original["hooks"]["Stop"])
            self.assertTrue(all(not groups for name, groups in restored["hooks"].items() if name != "Stop"))
            uninstaller.uninstall(target, tool)
            self.assertEqual(json.loads(filename.read_text(encoding="utf-8")), restored)

    def test_codex(self):
        self.check_tool("codex")

    def test_claude(self):
        self.check_tool("claude")

    def test_bad_configuration_is_preserved(self):
        for tool in ("codex", "claude"):
            with tempfile.TemporaryDirectory() as directory:
                target = Path(directory)
                filename = target / ("hooks.json" if tool == "codex" else "settings.json")
                filename.write_text('{"hooks": {"Stop": "invalid"}}', encoding="utf-8")
                before = filename.read_bytes()
                with self.assertRaises(ValueError):
                    module("install-" + tool + "-hooks").install(target)
                self.assertEqual(filename.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
