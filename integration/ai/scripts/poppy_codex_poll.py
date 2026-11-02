#!/usr/bin/env python3
"""Codexのローカル履歴を定期確認し、Poppyの表示を修復する。外部通信はしない。"""

import argparse
from datetime import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import quote

from poppy_status import excerpt, message_text, send_relay

INTERVAL = 0.5
HEARTBEAT = 2
MAX_READ = 1024 * 1024
INITIAL_READ = 4 * MAX_READ


def process_identity(pid):
    try:
        # commには空白や括弧を含められるため、最後の ')' の後から読む。
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return int(fields[1]), fields[19] if fields[0] != "Z" else ""
    except (OSError, ValueError, IndexError):
        return None, ""


def codex_owner():
    pid = os.getppid()
    while pid:
        parent, identity = process_identity(pid)
        try:
            if Path(f"/proc/{pid}/exe").readlink().name == "codex":
                return pid, identity
        except OSError:
            pass
        pid = parent
    return None, ""


def session_metadata(path):
    try:
        with Path(path).open("rb") as stream:
            record = json.loads(stream.readline(MAX_READ))
        if isinstance(record, dict) and record.get("type") == "session_meta":
            payload = record.get("payload")
            if isinstance(payload, dict) and isinstance(payload.get("id"), str):
                return payload
    except (OSError, ValueError):
        pass
    return None


def ensure_monitor(action, data, relay, owner=None):
    """起動は一度だけ。各状態のhookが欠けても履歴から再取得できる。"""
    transcript = data.get("transcript_path")
    if not isinstance(transcript, str) or not transcript:
        return False
    metadata = session_metadata(transcript)
    if not metadata:
        return False  # Claudeや非対応の形式は従来のhookで通知する。
    if isinstance(metadata.get("source"), dict):
        return True  # 同じCLIが動かす子エージェントから親の表示を変更しない。
    pid, identity = owner or codex_owner()
    if not pid or not identity or not relay:
        return False

    directory = Path(tempfile.gettempdir()) / f"poppy-codex-poll-{os.getuid()}"
    directory.mkdir(mode=0o700, exist_ok=True)
    key = hashlib.sha256(f"{relay}\n{pid}\n{identity}".encode()).hexdigest()
    state_path = directory / (key + ".json")
    # 並列ツールのhookからも、入力待ちの情報と監視対象を原子的に更新する。
    with (directory / (key + ".update.lock")).open("a") as update_lock:
        fcntl.flock(update_lock, fcntl.LOCK_EX)
        try:
            state = json.loads(state_path.read_text())
        except (OSError, ValueError):
            state = {}
        if not isinstance(state, dict):
            state = {}
        if state.get("transcript") != transcript:
            state = {"waiting": False}
        state.update(pid=pid, identity=identity, relay=relay, transcript=transcript,
                     script=str(Path(__file__).resolve()))
        ending = action == "none" and data.get("hook_event_name") == "SessionEnd"
        if ending:
            state["ending"] = True
        if action == "waiting":
            state.update(waiting=True, waitingToken=time.time_ns())
        elif action in ("running", "resume", "tool-failed", "completed", "interrupted"):
            state["waiting"] = False
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=directory, delete=False) as output:
            temporary = Path(output.name)
            try:
                json.dump(state, output, ensure_ascii=False)
                output.close()
                temporary.replace(state_path)
            finally:
                temporary.unlink(missing_ok=True)

    if state.get("ending"):
        # CLIがPID 1の場合、終了直後にコンテナと監視も終了する。
        # 最終通知は監視の次のpollを待たず、このhookでも届ける。
        if ending:
            try:
                send_relay(relay, "status", {"status": "none"}, 1)
            except (OSError, ValueError):
                pass
        return True

    # ロックFDを子へ引き継ぎ、起動途中にも別のhookが二重起動できないようにする。
    lock = os.open(directory / (key + ".lock"), os.O_CREAT | os.O_RDWR, 0o600)
    try:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return True
        process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--state", str(state_path),
                                   "--lock-fd", str(lock)], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, start_new_session=True, pass_fds=(lock,))
        threading.Thread(target=process.wait, daemon=True).start()
    finally:
        os.close(lock)
    return True


class Transcript:
    """旧形式のJSONLも差分だけ読み、途中の行とコンパクトを状態変更にしない。"""

    def __init__(self, path):
        self.path = Path(path)
        self.identity = None
        self.offset = 0
        self.pending = b""
        self.discard = False
        self.status = None
        self.turn = None
        self.activity = ""
        self.changed_at = None
        self.saw_metadata = False

    def read(self):
        try:
            with self.path.open("rb") as stream:
                stat = os.fstat(stream.fileno())
                identity = (stat.st_dev, stat.st_ino)
                if identity != self.identity or stat.st_size < self.offset:
                    self.identity = identity
                    self.offset = max(0, stat.st_size - INITIAL_READ)
                    self.pending = b""
                    self.discard = self.offset > 0
                    # 同じ会話のファイル置換だけでは、確定済みの状態を消さない。
                stream.seek(self.offset)
                chunk = stream.read(MAX_READ)
                self.offset += len(chunk)
        except OSError:
            return None
        lines = (self.pending + chunk).split(b"\n")
        self.pending = lines.pop()
        for line in lines:
            if self.discard:
                self.discard = False
                continue
            try:
                self.apply(json.loads(line))
            except (ValueError, TypeError):
                continue
        if len(self.pending) > MAX_READ:
            self.pending = b""
            self.discard = True
        if self.offset < stat.st_size:
            return None
        if self.status:
            return self.status, self.turn, self.activity
        return ("none", None, "") if self.saw_metadata else None

    def apply(self, record):
        if not isinstance(record, dict) or not isinstance(record.get("payload"), dict):
            return
        payload = record["payload"]
        kind = payload.get("type")
        if record.get("type") == "session_meta":
            self.saw_metadata = True
        if record.get("type") == "event_msg":
            if kind == "task_started":
                self.status, self.turn, self.activity = "running", payload.get("turn_id"), ""
            elif kind in ("task_complete", "turn_aborted"):
                turn = payload.get("turn_id")
                if self.turn and turn and turn != self.turn:
                    return
                self.status = "completed" if kind == "task_complete" else "interrupted"
                self.turn = turn or self.turn
                text = payload.get("last_agent_message")
                if isinstance(text, str):
                    self.activity = excerpt(text)
            elif kind == "agent_message" and isinstance(payload.get("message"), str):
                if payload.get("phase") in (None, "commentary", "final_answer"):
                    self.activity = excerpt(payload["message"])
            if kind in ("task_started", "task_complete", "turn_aborted"):
                try:
                    # DBのstarted_at/completed_atはUnix秒。精度を合わせ、
                    # 同じ秒の開始ログでDBの完了を上書きしない。
                    self.changed_at = int(datetime.fromisoformat(record["timestamp"].replace("Z", "+00:00")).timestamp())
                except (KeyError, TypeError, ValueError, AttributeError, OverflowError):
                    self.changed_at = None
        elif (record.get("type") == "response_item" and payload.get("role") == "assistant"
              and payload.get("recipient") in (None, "all")
              and payload.get("phase") in (None, "commentary", "final_answer")):
            text = message_text(payload)
            if text:
                self.activity = excerpt(text)


class Snapshot:
    def __init__(self, transcript):
        self.transcript = Transcript(transcript)
        self.metadata = session_metadata(transcript) or {}
        self.root = next((p.parent for p in Path(transcript).parents if p.name == "sessions"), None)
        self.database = None
        self.database_observed = False

    def read(self):
        # JSONLも毎回追跡する。再開後にDBだけが更新されなくなる形式では、
        # 古い完了・中断のレコードより新しいJSONLの状態を使う。
        transcript = self.transcript.read()
        try:
            if self.database is None and self.root:
                paths = sorted(self.root.glob("thread_history_*.sqlite"), key=lambda p: p.stat().st_mtime, reverse=True)
                if paths:
                    self.database = sqlite3.connect(f"file:{quote(str(paths[0]))}?mode=ro", uri=True, timeout=0.1)
                    self.database.row_factory = sqlite3.Row
            if self.database:
                thread = self.metadata.get("id")
                row = self.database.execute(
                    "SELECT * FROM thread_turns WHERE thread_id=? ORDER BY rollout_ordinal DESC LIMIT 1",
                    (thread,)).fetchone()
                if row:
                    status = {"inProgress": "running", "completed": "completed", "interrupted": "interrupted",
                              "failed": "interrupted"}.get(row["status"])
                    if status:
                        timestamp = "started_at" if status == "running" else "completed_at"
                        updated = row[timestamp] if timestamp in row.keys() else None
                        if (transcript and self.transcript.changed_at is not None
                                and isinstance(updated, (int, float)) and self.transcript.changed_at > updated):
                            self.database_observed = True
                            return transcript
                        activity = ""
                        for (raw,) in self.database.execute(
                                "SELECT item_json FROM thread_items WHERE thread_id=? AND turn_id=? AND item_type='agentMessage' "
                                "ORDER BY rollout_ordinal DESC LIMIT 4", (thread, row["turn_id"])):
                            try:
                                item = json.loads(raw)
                            except ValueError:
                                continue
                            if (isinstance(item, dict) and item.get("phase") in (None, "commentary", "final_answer")
                                    and isinstance(item.get("text"), str)):
                                activity = excerpt(item["text"])
                                break
                        self.database_observed = True
                        return status, row["turn_id"], activity
        except (OSError, sqlite3.Error, ValueError, TypeError):
            if self.database_observed:
                return None  # DBの一時的な読取失敗を、古いJSONLの状態で上書きしない。
            # 旧スキーマ・DBなしではJSONLを使い、表示をnoneにしない。
        return transcript

    def close(self):
        if self.database:
            self.database.close()


def monitor(state_path):
    stopped = False

    def stop(*_):
        nonlocal stopped
        stopped = True

    for name in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(name, stop)
    reader = None
    path = None
    last = None
    sent = 0
    waiting_token = None
    waiting_turn = None
    state = json.loads(Path(state_path).read_text())
    try:
        while not stopped:
            try:
                if Path(state_path).with_suffix(".stop").exists():
                    break
                state = json.loads(Path(state_path).read_text())
                if process_identity(state["pid"])[1] != state["identity"]:
                    break
                current_path = transcript_for_process(state["pid"], state["transcript"]) or state["transcript"]
                if path != current_path:
                    if reader:
                        reader.close()
                    path = current_path
                    reader = Snapshot(path)
                    last = None
                    waiting_token = None
                snapshot = None if state.get("ending") and state["transcript"] == path else reader.read()
                if snapshot:
                    status, turn, activity = snapshot
                    if state["transcript"] == path and state.get("waitingToken") != waiting_token:
                        waiting_token = state.get("waitingToken")
                        waiting_turn = turn
                    if (status == "running" and state["transcript"] == path
                            and state.get("waiting") and waiting_turn == turn):
                        status = "waiting"
                    payload = {"status": status}
                    if activity:
                        payload["activityText"] = activity
                    now = time.monotonic()
                    if payload != last or now - sent >= HEARTBEAT:
                        send_relay(state["relay"], "status", payload, 1)
                        last, sent = payload, now
            except (OSError, ValueError, KeyError):
                pass  # 失敗を記録済みにせず、次のpollで再送する。
            time.sleep(INTERVAL)
    finally:
        if reader:
            reader.close()
        try:
            send_relay(state["relay"], "status", {"status": "none"}, 1)
        except (OSError, ValueError, KeyError):
            pass
        Path(state_path).unlink(missing_ok=True)
        Path(state_path).with_suffix(".stop").unlink(missing_ok=True)


def stop_monitors(script):
    directory = Path(tempfile.gettempdir()) / f"poppy-codex-poll-{os.getuid()}"
    for path in directory.glob("*.json"):
        try:
            state = json.loads(path.read_text())
            if isinstance(state, dict) and state.get("script") == str(Path(script).resolve()):
                path.with_suffix(".stop").touch()
        except (OSError, ValueError):
            continue


def transcript_for_process(pid, preferred=None):
    try:
        candidates = []
        for descriptor in Path(f"/proc/{pid}/fd").iterdir():
            try:
                target = descriptor.readlink()
            except OSError:
                continue
            if target.name.startswith("rollout-") and target.suffix == ".jsonl":
                if str(target) == preferred:
                    return preferred
                candidates.append(target)
        for target in candidates:
            metadata = session_metadata(target)
            if metadata and not isinstance(metadata.get("source"), dict):
                return str(target)
    except OSError:
        pass
    return None


def attach(relay, pid=None):
    """hookなしでも、同じ端末で動くCLIの開いている会話へ接続できる。"""
    candidates = [Path(f"/proc/{pid}")] if pid else Path("/proc").iterdir()
    for process in candidates:
        if not process.name.isdigit():
            continue
        try:
            if process.joinpath("exe").readlink().name != "codex":
                continue
            if not pid and f"AICONTAINER_EVENT_SOCKET={relay}".encode() not in process.joinpath("environ").read_bytes().split(b"\0"):
                continue
            target = transcript_for_process(process.name)
            if target:
                return ensure_monitor("activity", {"transcript_path": target}, relay,
                                      (int(process.name), process_identity(process.name)[1]))
        except OSError:
            continue
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, help="監視するCodexのPID。省略時は同じ通知ソケットのCLIを探す")
    parser.add_argument("--stop", action="store_true", help="このスクリプトから起動した監視を終了する")
    parser.add_argument("--state", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--lock-fd", type=int, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.stop:
        stop_monitors(__file__)
    elif args.state:
        monitor(args.state)
    else:
        relay = os.environ.get("AICONTAINER_EVENT_SOCKET", "")
        if not relay or not attach(relay, args.pid):
            parser.exit(1, "Poppy: 同じターミナルで動くCodexの会話が見つかりませんでした。\n")
        print("Poppy: Codexのpolling監視を開始しました。")


if __name__ == "__main__":
    main()
