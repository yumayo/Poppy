#!/usr/bin/env python3
"""AIフックからPoppyへ状態と公開応答の抜粋を通知する。通知失敗でAIを止めない。"""

import json
import os
from pathlib import Path
import re
import select
import socket
import sys
import time

MAX_INPUT_BYTES = 1024 * 1024
MAX_TRANSCRIPT_BYTES = 512 * 1024
MAX_TEXT_LENGTH = 120
ACTIONS = ("running", "waiting", "interrupted", "completed", "none", "resume", "tool-failed", "activity")


def send_relay(filename, kind, payload, timeout):
    message = json.dumps({"type": kind, "body": json.dumps(payload, ensure_ascii=False, separators=(",", ":"))},
                         ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n"
    if len(message) > 16384:
        raise ValueError("notification is too large")
    deadline = time.monotonic() + float(timeout)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        def remaining_timeout():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("notification relay timed out")
            connection.settimeout(remaining)
        remaining_timeout()
        connection.connect(filename)
        remaining_timeout()
        connection.sendall(message)
        reply = bytearray()
        while b"\n" not in reply and len(reply) < 16:
            remaining_timeout()
            chunk = connection.recv(16 - len(reply))
            if not chunk:
                break
            reply.extend(chunk)
        if reply != b"OK\n":
            raise OSError("notification relay did not accept the event")


def read_hook_input():
    # 手動実行時やstdinを閉じない呼び出しでも、入力待ちでAIを止めない。
    if sys.stdin.isatty():
        return {}
    deadline = time.monotonic() + 0.2
    content = bytearray()
    while len(content) <= MAX_INPUT_BYTES:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([sys.stdin], [], [], remaining)[0]:
            break
        chunk = os.read(sys.stdin.fileno(), min(65536, MAX_INPUT_BYTES + 1 - len(content)))
        if not chunk:
            break
        content.extend(chunk)
    if len(content) > MAX_INPUT_BYTES:
        raise ValueError("hook input is too large")
    data = json.loads(content) if content else {}
    if not isinstance(data, dict):
        raise ValueError("hook input must be an object")
    return data


def message_text(message):
    content = message.get("content", [])
    if isinstance(content, str):
        return content
    if not isinstance(content, list):
        return ""
    return "\n".join(part["text"] for part in content
                     if isinstance(part, dict) and part.get("type") in ("text", "output_text")
                     and isinstance(part.get("text"), str))


def latest_message(transcript_path):
    if not isinstance(transcript_path, str) or not transcript_path:
        return ""
    try:
        path = Path(transcript_path)
        if not path.is_file():
            return ""
        with path.open("rb") as transcript:
            size = os.fstat(transcript.fileno()).st_size
            offset = max(0, size - MAX_TRANSCRIPT_BYTES)
            transcript.seek(offset)
            lines = transcript.read(MAX_TRANSCRIPT_BYTES).splitlines()
        if offset:
            lines = lines[1:]  # 末尾から読んだときの途中のレコードを除く。
    except (OSError, ValueError):
        return ""

    latest = ""
    for line in lines:
        try:
            record = json.loads(line)
        except (ValueError, UnicodeError):
            continue  # 書き込み途中の最終行は次のフックで読み直す。
        if not isinstance(record, dict) or record.get("isSidechain") is True:
            continue
        kind = record.get("type")
        payload = record.get("payload")
        if kind == "event_msg" and isinstance(payload, dict):
            if payload.get("type") in ("user_message", "task_started"):
                latest = ""
            elif (payload.get("type") == "agent_message" and isinstance(payload.get("message"), str)
                  and payload.get("phase") in (None, "commentary", "final_answer")):
                latest = payload["message"]
            continue
        # Codexのresponse_itemとClaude Codeのassistantレコードだけを読む。
        message = payload if kind == "response_item" else record.get("message") if kind in ("assistant", "user") else None
        if not isinstance(message, dict):
            continue
        role = message.get("role")
        if role == "user":
            # ツール結果は会話の区切りとして扱わない。
            content = message.get("content", [])
            if isinstance(content, str) or (isinstance(content, list) and any(
                isinstance(part, dict) and part.get("type") in ("text", "input_text") for part in content
            )):
                latest = ""
        elif (role == "assistant" and message.get("recipient") in (None, "all")
              and message.get("phase") in (None, "commentary", "final_answer")):
            text = message_text(message)
            if text:
                latest = text
    return latest


def excerpt(text):
    # 本文の先頭を抜粋する。思考・ツール出力・依頼文は入力対象にしない。
    text = re.sub(r"```[\s\S]*?(?:```|$)", " ", text)
    text = re.sub(r"!?\[([^\]\n]+)\]\([^\n]*?\)", r"\1", text)
    text = re.sub(r"(?m)^\s*(?:#{1,6}\s+|[-*+]\s+|\d+[.)]\s+|>\s*)", "", text)
    text = text.replace("**", "").replace("__", "").replace("`", "")
    text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)
    text = re.sub(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]", "", text)
    text = " ".join(text.split())
    return text if len(text) <= MAX_TEXT_LENGTH else text[:MAX_TEXT_LENGTH - 1].rstrip() + "…"


def build_payload(action, data):
    status = "running" if action in ("resume", "tool-failed") else action
    if action == "tool-failed" and data.get("is_interrupt") is True:
        status = "interrupted"
    payload = {} if action == "activity" else {"status": status}
    if action in ("resume", "tool-failed"):
        payload["onlyIfActive"] = True
    # ClaudeのStopは、バックグラウンドのサブエージェントを待つ間にも発生する。
    # Claude自身が渡す一覧で判定し、子の終了後に親が応答を終えたStopで完了にする。
    background_tasks = data.get("background_tasks")
    if (action == "completed" and data.get("hook_event_name") == "Stop"
            and isinstance(background_tasks, list) and any(
                isinstance(task, dict) and task.get("status") in ("running", "pending")
                for task in background_tasks)):
        payload.update(status="running", onlyIfActive=True)
    # 依頼開始と終了では、以前の発言を再送しない。
    if action not in ("running", "none"):
        text = data.get("last_assistant_message") if action == "completed" else None
        if not isinstance(text, str) or not text.strip():
            text = latest_message(data.get("transcript_path"))
        text = excerpt(text)
        if text:
            payload["activityText"] = text
    return payload


def main():
    if len(sys.argv) not in (2, 3) or sys.argv[1] not in ACTIONS:
        print(f"Usage: poppy_status.py {'|'.join(ACTIONS)} [timeout-seconds: 1-5]", file=sys.stderr)
        return 2
    action = sys.argv[1]
    timeout = sys.argv[2] if len(sys.argv) == 3 else "5"
    if timeout not in ("1", "2", "3", "4", "5"):
        print("Poppy: timeout must be between 1 and 5 seconds", file=sys.stderr)
        return 2

    # 通知経路は起動元が自動設定するUnixソケットだけを使う。
    relay = os.environ.get("AICONTAINER_EVENT_SOCKET", "")
    if not relay:
        return 0

    try:
        data = read_hook_input()
    except (OSError, ValueError):
        if action == "tool-failed":
            print("Poppy: ツール失敗フックの入力を読み取れませんでした。", file=sys.stderr)
            return 0
        data = {}
    payload = build_payload(action, data)
    if not payload:
        return 0
    endpoint = "activity" if action == "activity" else "status"
    try:
        send_relay(relay, endpoint, payload, timeout)
    except (OSError, ValueError):
        print("Poppy: 通知リレーへ送信できませんでした。PoppyのWSLターミナルを開き直してください。", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
