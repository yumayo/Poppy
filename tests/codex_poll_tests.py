"""Offline recovery tests: real polling processes, SQLite, JSONL and Unix sockets."""
import sys
sys.dont_write_bytecode = True
import fcntl
import json
import os
from pathlib import Path
import queue
import select
import socket
import sqlite3
import subprocess
import tempfile
import threading
import time
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1] / "integration/ai/scripts"
sys.path.insert(0, str(SCRIPTS))
import poppy_codex_poll as poller


def record(kind, **payload):
    return json.dumps({"type": kind, "payload": payload}, ensure_ascii=False) + "\n"


class HistoryFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="poppy-poll-history-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root / "sessions/2026/11/01/rollout-test.jsonl"
        self.path.parent.mkdir(parents=True)
        self.path.write_text(record("session_meta", id="thread", source="cli"))

    def append(self, text):
        with self.path.open("a", encoding="utf-8") as stream:
            stream.write(text)

    def database(self):
        database = sqlite3.connect(self.root / "thread_history_1.sqlite")
        self.addCleanup(database.close)
        database.executescript("""
            PRAGMA journal_mode=WAL;
            CREATE TABLE thread_turns(thread_id TEXT, turn_id TEXT, status TEXT, rollout_ordinal INTEGER);
            CREATE TABLE thread_items(thread_id TEXT, turn_id TEXT, item_type TEXT, item_json TEXT, rollout_ordinal INTEGER);
        """)
        return database


class HistoryTests(HistoryFixture):
    def test_new_conversation_is_idle_without_an_inactivity_timeout(self):
        reader = poller.Transcript(self.path)
        self.assertEqual(reader.read(), ("none", None, ""))
        self.append(record("event_msg", type="task_started", turn_id="first"))
        self.assertEqual(reader.read()[0], "running")
        self.assertEqual(reader.read()[0], "running")

    def test_partial_lines_compaction_and_late_completion(self):
        reader = poller.Transcript(self.path)
        self.append(record("event_msg", type="task_started", turn_id="first"))
        self.assertEqual(reader.read(), ("running", "first", ""))
        self.append(record("compacted", message="summary"))
        self.append(record("response_item", role="user", content="compacted context"))
        self.assertEqual(reader.read()[0], "running")
        self.append(record("event_msg", type="task_started", turn_id="second"))
        self.append(record("event_msg", type="task_complete", turn_id="first"))
        self.assertEqual(reader.read()[:2], ("running", "second"))
        completion = record("event_msg", type="task_complete", turn_id="second", last_agent_message="公開の完了報告。")
        self.append(completion[:-3])
        self.assertEqual(reader.read()[0], "running")
        self.append(completion[-3:])
        self.assertEqual(reader.read(), ("completed", "second", "公開の完了報告。"))

    def test_replaced_transcript_does_not_clear_running(self):
        reader = poller.Transcript(self.path)
        self.append(record("event_msg", type="task_started", turn_id="first"))
        reader.read()
        replacement = self.path.with_suffix(".new")
        replacement.write_text(record("session_meta", id="thread", source="cli") + record("compacted", message="summary"))
        replacement.replace(self.path)
        self.assertEqual(reader.read()[0], "running")

    def test_database_recovers_a_turn_missing_from_jsonl_and_keeps_private_text_private(self):
        database = self.database()
        self.append(record("event_msg", type="task_complete", turn_id="old"))
        database.execute("INSERT INTO thread_turns VALUES ('thread','current','inProgress',10)")
        for ordinal, phase, text in ((11, "commentary", "確認しています。"), (12, "analysis", "private reasoning")):
            database.execute("INSERT INTO thread_items VALUES ('thread','current','agentMessage',?,?)",
                             (json.dumps({"phase": phase, "text": text}), ordinal))
        database.commit()
        reader = poller.Snapshot(self.path)
        self.addCleanup(reader.close)
        self.assertEqual(reader.read(), ("running", "current", "確認しています。"))
        database.execute("UPDATE thread_turns SET status='interrupted'")
        database.commit()
        self.assertEqual(reader.read()[0], "interrupted")
        database.execute("DROP TABLE thread_turns")
        database.commit()
        self.assertIsNone(reader.read(), "An unavailable DB restored stale JSONL completion")

    def test_unsupported_database_uses_jsonl(self):
        database = sqlite3.connect(self.root / "thread_history_1.sqlite")
        database.close()
        self.append(record("event_msg", type="task_started", turn_id="legacy"))
        reader = poller.Snapshot(self.path)
        self.addCleanup(reader.close)
        self.assertEqual(reader.read()[:2], ("running", "legacy"))

    def test_subagent_hooks_do_not_start_another_monitor(self):
        self.path.write_text(record("session_meta", id="child", source={"subagent": {"parent_thread_id": "parent"}}))
        with mock.patch.object(poller.subprocess, "Popen", side_effect=AssertionError("Child started a monitor")):
            self.assertTrue(poller.ensure_monitor("none", {"transcript_path": str(self.path)}, "/unused"))


class PollingTests(HistoryFixture):
    # These tests use the same temporary transcript/database helpers, but each
    # monitor has an isolated runtime directory and never targets the real CLI.
    def setUp(self):
        super().setUp()
        self.runtime = mock.patch.object(poller.tempfile, "gettempdir", return_value=str(self.root))
        self.runtime.start()
        self.addCleanup(self.runtime.stop)
        self.owner = subprocess.Popen([sys.executable, "-u", "-c",
                                       "import sys\nf = open(sys.argv[1], 'rb')\nprint('ready')\n"
                                       "for path in sys.stdin:\n f.close()\n f = open(path.rstrip('\\n'), 'rb')\n print('ready')",
                                       str(self.path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.addCleanup(self.stop_owner)
        self.owner_ready()
        self.identity = (self.owner.pid, poller.process_identity(self.owner.pid)[1])
        self.database_connection = self.database()
        self.requests = queue.Queue()
        self.stopping = threading.Event()
        self.reject = 0
        self.attempts = 0
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket_path = str(self.root / "events.sock")
        self.server.bind(self.socket_path)
        self.server.listen(8)
        self.server.settimeout(0.05)

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
                    self.attempts += 1
                    if self.reject:
                        self.reject -= 1
                        connection.sendall(b"BUSY\n")
                    else:
                        connection.sendall(b"OK\n")
                        self.requests.put(json.loads(event["body"]))

        self.thread = threading.Thread(target=receive)
        self.thread.start()
        self.addCleanup(self.stop_server)

    def stop_owner(self):
        if self.owner.poll() is None:
            self.owner.stdin.close()
            self.owner.wait(timeout=3)
        self.owner.stdout.close()

    def owner_ready(self):
        self.assertTrue(select.select([self.owner.stdout], [], [], 3)[0])
        self.assertEqual(self.owner.stdout.readline(), "ready\n")

    def stop_server(self):
        for path in self.root.glob("poppy-codex-poll-*/*.json"):
            path.with_suffix(".stop").touch()
        deadline = time.monotonic() + 4
        for path in self.root.glob("poppy-codex-poll-*/*.lock"):
            if path.name.endswith(".update.lock"):
                continue
            with path.open("a") as lock:
                while True:
                    try:
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        break
                    except BlockingIOError:
                        if time.monotonic() >= deadline:
                            self.fail("Polling process did not stop")
                        time.sleep(0.02)
        self.stopping.set()
        self.thread.join(timeout=3)
        self.server.close()
        self.assertFalse(self.thread.is_alive())

    def turn(self, turn, status, ordinal):
        self.database_connection.execute("DELETE FROM thread_turns WHERE turn_id=?", (turn,))
        self.database_connection.execute("INSERT INTO thread_turns VALUES ('thread',?,?,?)", (turn, status, ordinal))
        self.database_connection.commit()

    def hook(self, action="activity"):
        self.assertTrue(poller.ensure_monitor(action, {"transcript_path": str(self.path)}, self.socket_path, self.identity))

    def wait_status(self, status, timeout=4):
        deadline = time.monotonic() + timeout
        while True:
            payload = self.requests.get(timeout=max(0.01, deadline - time.monotonic()))
            if payload["status"] == status:
                self.assertNotIn("onlyIfActive", payload)
                return payload

    def test_recovers_all_lifecycle_states_without_their_hooks(self):
        self.turn("first", "inProgress", 1)
        self.hook()  # No UserPromptSubmit hook.
        self.wait_status("running")
        self.turn("first", "completed", 1)
        self.wait_status("completed")  # No Stop hook.
        self.turn("second", "inProgress", 2)
        self.wait_status("running")
        self.turn("second", "interrupted", 2)
        self.wait_status("interrupted")  # No Interrupt hook.
        self.stop_owner()
        self.wait_status("none")  # No SessionEnd hook.

    def test_retries_a_rejected_notification_and_refreshes_unchanged_state(self):
        self.reject = 1
        self.turn("first", "inProgress", 1)
        self.hook()
        self.wait_status("running")
        self.assertGreaterEqual(self.attempts, 2)
        self.wait_status("running")  # Periodic repair even when the history is unchanged.

    def test_waiting_survives_activity_and_does_not_leak_to_the_next_turn(self):
        self.turn("first", "inProgress", 1)
        self.hook()
        self.wait_status("running")
        self.hook("waiting")
        self.hook("activity")
        self.wait_status("waiting")
        self.hook("resume")
        self.wait_status("running")
        self.hook("waiting")
        self.wait_status("waiting")
        self.turn("first", "completed", 1)
        self.wait_status("completed")
        self.turn("second", "inProgress", 2)
        self.wait_status("running")

    def test_repeated_hooks_reuse_the_monitor_and_none_does_not_clear_an_active_turn(self):
        self.turn("first", "inProgress", 1)
        self.hook()
        self.wait_status("running")
        with mock.patch.object(poller.subprocess, "Popen", side_effect=AssertionError("Duplicate monitor")):
            self.hook("none")
            self.hook()
        self.wait_status("running")

    def test_follows_a_new_conversation_without_a_session_start_hook(self):
        self.turn("first", "completed", 1)
        self.hook()
        self.wait_status("completed")
        next_path = self.path.with_name("rollout-next.jsonl")
        next_path.write_text(record("session_meta", id="next-thread", source="cli"))
        self.database_connection.execute("INSERT INTO thread_turns VALUES ('next-thread','next','inProgress',1)")
        self.database_connection.commit()
        self.owner.stdin.write(str(next_path) + "\n")
        self.owner.stdin.flush()
        self.owner_ready()
        self.wait_status("running")

    def test_can_stop_monitoring_without_stopping_codex(self):
        self.turn("first", "inProgress", 1)
        self.hook()
        self.wait_status("running")
        poller.stop_monitors(SCRIPTS / "poppy_codex_poll.py")
        self.wait_status("none")
        self.assertIsNone(self.owner.poll())

    def test_session_end_notifies_before_the_container_can_kill_the_monitor(self):
        self.turn("first", "inProgress", 1)
        self.hook()
        self.wait_status("running")
        self.assertTrue(poller.ensure_monitor("none", {
            "transcript_path": str(self.path), "hook_event_name": "SessionEnd",
        }, self.socket_path, self.identity))
        self.wait_status("none")
        self.assertIsNone(self.owner.poll())
        with self.assertRaises(queue.Empty):
            self.requests.get(timeout=0.7)


if __name__ == "__main__":
    unittest.main()
