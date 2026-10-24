"""Offline tests for Poppy notifications and reversible hook installation."""
import sys
sys.dont_write_bytecode = True
import importlib.util
import json
import os
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import queue
import shlex
import shutil
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


@unittest.skipUnless(shutil.which("curl"), "Poppy's hook sender requires curl")
class HookDeliveryTests(unittest.TestCase):
    def setUp(self):
        self.requests = queue.Queue()
        requests = self.requests

        class Receiver(BaseHTTPRequestHandler):
            def receive(self):
                body = self.rfile.read(int(self.headers["Content-Length"]))
                requests.put((self.command, self.path, json.loads(body)))
                self.send_response(204)
                self.end_headers()

            do_POST = do_PUT = receive

            def log_message(self, *_):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Receiver)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.05})
        self.thread.start()
        self.environment = {
            **os.environ,
            "POPPY_EVENTS_URL": f"http://127.0.0.1:{self.server.server_port}/",
            "POPPY_SESSION_ID": "0123456789abcdef0123456789abcdef",
            "NO_PROXY": "127.0.0.1",
            "no_proxy": "127.0.0.1",
        }

    def tearDown(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()

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
            for name, data, method, endpoint, payload in (
                ("UserPromptSubmit", {}, "POST", "status", {"status": "running"}),
                ("PostToolUse", {}, "POST", "status", {"status": "running", "onlyIfActive": True}),
                ("PreToolUse", {"transcript_path": str(transcript)}, "PUT", "activity", {"activityText": "設定を確認しています。"}),
                ("Stop", {"last_assistant_message": "完了しました。"}, "POST", "status", {"status": "completed", "activityText": "完了しました。"}),
            ):
                with self.subTest(tool=tool, event=name):
                    self.invoke(hooks[name][0]["hooks"][0]["command"], data)
                    self.assertEqual(self.requests.get(timeout=1), (
                        method, f"/tasks/sessions/{self.environment['POPPY_SESSION_ID']}/{endpoint}", payload,
                    ))

    def test_codex_hooks_deliver_to_poppy(self):
        self.check_tool("codex")

    def test_claude_hooks_deliver_to_poppy(self):
        self.check_tool("claude")

    def test_missing_poppy_environment_does_not_send(self):
        command = shlex.join([sys.executable, str(SCRIPTS / "poppy_status.py"), "running"])
        for name in ("POPPY_EVENTS_URL", "POPPY_SESSION_ID"):
            with self.subTest(missing=name):
                environment = self.environment.copy()
                del environment[name]
                self.invoke(command, {}, environment)
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
