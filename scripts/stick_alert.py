#!/usr/bin/env python3
import asyncio
import hashlib
import json
import os
import pathlib
import sys
from datetime import datetime

from bleak import BleakClient, BleakScanner

DEVICE_NAMES = {"StickS3-AI", "StickS3-Codex"}
SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
LOG_PATH = pathlib.Path.home() / ".stick-s3-ai-alert.log"


def log(line: str) -> None:
    timestamp = datetime.now().isoformat(timespec="seconds")
    with LOG_PATH.open("a", encoding="utf-8") as handle:
        handle.write(f"{timestamp} {line}\n")


def clean(value: str, limit: int) -> str:
    allowed = []
    for char in (value or "").upper():
        if char.isalnum() or char in {"-", "_", " "}:
            allowed.append(char)
        if len(allowed) >= limit:
            break
    return "".join(allowed) or "-"


def normalize_state(raw: str) -> str:
    value = (raw or "ping").strip().lower()
    if value in {"ask", "approval", "permission", "permissionrequest", "notification"}:
        return "ASK"
    if value in {"done", "stop", "complete", "completed", "subagentstop"}:
        return "DONE"
    return clean(value, 7)


def normalize_tool(raw: str) -> str:
    value = (raw or "codex").strip().lower()
    if value in {"claude", "claude-code", "claudecode", "cc"}:
        return "CC"
    if value in {"codex", "cx"}:
        return "CX"
    return clean(value, 7)


def load_payload() -> dict:
    raw = sys.stdin.read()
    if not raw:
        return {}
    try:
        data = json.loads(raw)
        log(f"payload {json.dumps(data, ensure_ascii=False)[:600]}")
        return data if isinstance(data, dict) else {}
    except Exception as exc:
        log(f"stdin ignored: {exc}")
        return {}


def session_label(tool: str, payload: dict) -> str:
    session = str(
        payload.get("session_id")
        or payload.get("sessionId")
        or payload.get("conversation_id")
        or payload.get("conversationId")
        or ""
    )
    cwd = str(
        payload.get("cwd")
        or payload.get("project_dir")
        or payload.get("workspace")
        or os.environ.get("CLAUDE_PROJECT_DIR")
        or os.environ.get("PWD")
        or os.getcwd()
    )
    base = pathlib.Path(cwd).name or tool
    seed = f"{tool}:{session}:{cwd}:{os.getppid()}"
    suffix = hashlib.sha1(seed.encode("utf-8")).hexdigest()[:2].upper()
    return clean(f"{base}-{suffix}", 15)


async def find_device():
    def match(device, advertisement_data):
        name = device.name or advertisement_data.local_name or ""
        if name in DEVICE_NAMES:
            return True
        return SERVICE_UUID in [uuid.lower() for uuid in advertisement_data.service_uuids]

    return await BleakScanner.find_device_by_filter(match, timeout=4.0)


async def send(message: str) -> bool:
    device = await find_device()
    if device is None:
        log(f"miss {message}: device not found")
        return False

    async with BleakClient(device, timeout=5.0) as client:
        await client.write_gatt_char(RX_UUID, message.encode("ascii"), response=True)
    log(f"sent {message}")
    return True


def main() -> int:
    args = [arg for arg in sys.argv[1:] if arg]
    if len(args) >= 2:
        tool = normalize_tool(args[0])
        state = normalize_state(args[1])
    elif len(args) == 1:
        tool = "CX"
        state = normalize_state(args[0])
    else:
        tool = "AI"
        state = "PING"

    payload = load_payload()
    event = payload.get("hook_event_name") or payload.get("hookEventName")
    if event and len(args) < 2:
        state = normalize_state(str(event))

    label = session_label(tool, payload)
    now = datetime.now().strftime("%H:%M")
    message = f"{tool}|{label}|{state}|{now}"

    try:
        asyncio.run(asyncio.wait_for(send(message), timeout=8.0))
    except Exception as exc:
        detail = str(exc) or exc.__class__.__name__
        log(f"error {message}: {detail}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
