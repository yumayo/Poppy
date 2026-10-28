"""Offline tests for Poppy notifications and reversible hook installation."""
import sys
sys.dont_write_bytecode = True
import importlib.util
import json
import os
from pathlib import Path
import queue
import shlex
import socket
import subprocess
import tempfile
import threading
import unittest

SCRIPTS = Path(__file__).resolve().parents[1] / "integration" / "ai" / "scripts"


def module(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / (name + ".py"))
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


sender = module("poppy_status")


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
        command = shlex.join([sys.executable, str(SCRIPTS / "poppy_status.py"), "running"])
        environment = self.environment.copy()
        del environment["AICONTAINER_EVENT_SOCKET"]
        self.invoke(command, {}, environment)
        self.assertTrue(self.requests.empty())

    def test_retired_http_settings_do_not_enable_delivery(self):
        environment = self.environment.copy()
        del environment["AICONTAINER_EVENT_SOCKET"]
        environment.update(POPPY_EVENTS_URL="http://127.0.0.1:1", POPPY_SESSION_ID="0" * 32)
        result = subprocess.run([sys.executable, str(SCRIPTS / "poppy_status.py"), "running"],
                                input="{}", text=True, env=environment, capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout + result.stderr, "")
        self.assertTrue(self.requests.empty())


class SenderTests(unittest.TestCase):
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
