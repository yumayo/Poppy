#!/usr/bin/env python3
"""Remove only Poppy's hook commands; preserve other hooks and settings."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shlex
import shutil
import tempfile


def uninstall(target_dir: Path, tool: str) -> None:
    target_dir = target_dir.expanduser().resolve()
    config_path = target_dir / ("hooks.json" if tool == "codex" else "settings.json")
    script = target_dir / "hooks" / "poppy" / "poppy_status.py"
    if not config_path.exists():
        return
    config = json.loads(config_path.read_text(encoding="utf-8-sig"))
    if not isinstance(config, dict) or not isinstance(config.get("hooks", {}), dict):
        raise ValueError("Invalid hook configuration")
    changed = False
    for event, groups in config.get("hooks", {}).items():
        if not isinstance(groups, list):
            raise ValueError("Invalid hook groups")
        retained = []
        for group in groups:
            if not isinstance(group, dict) or not isinstance(group.get("hooks"), list):
                raise ValueError("Invalid hook group")
            handlers = []
            for handler in group["hooks"]:
                if not isinstance(handler, dict):
                    raise ValueError("Invalid hook handler")
                try:
                    text = handler.get("command", "")
                    command = shlex.split(text) if isinstance(text, str) else []
                except ValueError:
                    command = []
                if (handler.get("type") == "command" and len(command) in (3, 4)
                        and command[:2] == ["python3", str(script)]
                        and command[2] in ("running", "waiting", "interrupted", "completed", "none", "resume", "tool-failed", "activity")
                        and (len(command) == 3 or command[3] in ("1", "2", "3", "4", "5"))):
                    changed = True
                else:
                    handlers.append(handler)
            if handlers or not group["hooks"]:
                retained.append({**group, "hooks": handlers})
        config["hooks"][event] = retained
    if not changed:
        return
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    shutil.copy2(config_path, config_path.with_name(f"{config_path.name}.poppy-{timestamp}.bak"))
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=target_dir, delete=False) as output:
        temporary = Path(output.name)
        try:
            json.dump(config, output, ensure_ascii=False, indent=2)
            output.write("\n")
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise
    try:
        temporary.replace(config_path)
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tool", choices=("codex", "claude"))
    parser.add_argument("--config-dir", type=Path)
    args = parser.parse_args()
    default = os.environ.get("CODEX_HOME") if args.tool == "codex" else os.environ.get("CLAUDE_CONFIG_DIR")
    directory = args.config_dir or Path(default or Path.home() / (".codex" if args.tool == "codex" else ".claude"))
    uninstall(directory, args.tool)


if __name__ == "__main__":
    main()
