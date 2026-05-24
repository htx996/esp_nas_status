#!/usr/bin/env python3

from __future__ import annotations

import json
import os
import socket
import time
from datetime import datetime
from pathlib import Path
from typing import Any

import psutil
from flask import Flask, jsonify, request


app = Flask(__name__)

TOKEN = os.getenv("TOKEN", "changeme")
HOST = os.getenv("HOST", "0.0.0.0")
PORT = int(os.getenv("PORT", "8099"))
DISK_PATH = os.getenv("DISK_PATH", "/")
EVENT_STORE_PATH = Path(os.getenv("EVENT_STORE_PATH", "./data/latest_event.json"))
EVENT_MAX_AGE_SEC = int(os.getenv("EVENT_MAX_AGE_SEC", "0"))

_net_prev = psutil.net_io_counters()
_net_prev_ts = time.time()


def json_response(payload: dict[str, Any], status: int = 200):
    return jsonify(payload), status


def ensure_parent(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def check_token() -> bool:
    candidate = request.args.get("token") or request.headers.get("X-NAS-Token")
    if not candidate and request.is_json:
      body = request.get_json(silent=True) or {}
      candidate = body.get("token")
    return candidate == TOKEN


def read_event_store() -> dict[str, Any] | None:
    if not EVENT_STORE_PATH.exists():
        return None
    try:
        raw = EVENT_STORE_PATH.read_text(encoding="utf-8").strip()
        if not raw:
            return None
        data = json.loads(raw)
        if not isinstance(data, dict):
            return None
        return data
    except (OSError, json.JSONDecodeError):
        return None


def write_event_store(payload: dict[str, Any] | None) -> None:
    ensure_parent(EVENT_STORE_PATH)
    if payload is None:
        EVENT_STORE_PATH.write_text("", encoding="utf-8")
        return
    EVENT_STORE_PATH.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def current_event() -> dict[str, Any] | None:
    event = read_event_store()
    if not event:
        return None
    if EVENT_MAX_AGE_SEC > 0:
        received_at = int(event.get("_received_ts", 0))
        if received_at > 0 and time.time() - received_at > EVENT_MAX_AGE_SEC:
            return None
    if not event.get("id"):
        return None
    return {
        "id": str(event.get("id", "")).strip(),
        "app": str(event.get("app", "NAS")).strip() or "NAS",
        "level": str(event.get("level", "info")).strip() or "info",
        "time": str(event.get("time", "")).strip(),
        "message": str(event.get("message", "")).strip(),
    }


def uptime_string() -> str:
    seconds = int(time.time() - psutil.boot_time())
    days, seconds = divmod(seconds, 86400)
    hours, seconds = divmod(seconds, 3600)
    minutes, seconds = divmod(seconds, 60)
    return f"{days}d {hours:02d}:{minutes:02d}:{seconds:02d}"


def cpu_temperature_celsius() -> int:
    try:
        temps = psutil.sensors_temperatures()
    except Exception:
        temps = {}

    for name in ("coretemp", "k10temp", "cpu_thermal", "soc_thermal", "acpitz"):
        entries = temps.get(name)
        if not entries:
            continue
        for entry in entries:
            if entry.current:
                return int(round(entry.current))
    return 0


def format_rate(value: float) -> str:
    units = ["B/s", "KB/s", "MB/s", "GB/s", "TB/s"]
    rate = float(max(0.0, value))
    unit = units[0]
    for candidate in units:
        unit = candidate
        if rate < 1024.0 or candidate == units[-1]:
            break
        rate /= 1024.0
    if rate >= 100:
        return f"{rate:.0f}{unit}"
    if rate >= 10:
        return f"{rate:.1f}{unit}"
    return f"{rate:.1f}{unit}"


def network_speeds() -> tuple[str, str]:
    global _net_prev, _net_prev_ts

    now = time.time()
    current = psutil.net_io_counters()
    elapsed = max(0.25, now - _net_prev_ts)

    down_bps = (current.bytes_recv - _net_prev.bytes_recv) / elapsed
    up_bps = (current.bytes_sent - _net_prev.bytes_sent) / elapsed

    _net_prev = current
    _net_prev_ts = now
    return format_rate(down_bps), format_rate(up_bps)


def local_ip() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def collect_status() -> dict[str, Any]:
    disk = psutil.disk_usage(DISK_PATH)
    memory = psutil.virtual_memory()
    down, up = network_speeds()

    return {
        "cpu": int(round(psutil.cpu_percent(interval=0.15))),
        "mem": int(round(memory.percent)),
        "disk": int(round(disk.percent)),
        "temp": cpu_temperature_celsius(),
        "down": down,
        "up": up,
        "uptime": uptime_string(),
        "ip": local_ip(),
        "event": current_event(),
    }


def normalize_event_payload(data: dict[str, Any]) -> dict[str, Any]:
    event_id = str(data.get("id", "")).strip()
    if not event_id:
        event_id = f"msg-{int(time.time())}"

    app_name = str(data.get("app", "NAS")).strip() or "NAS"
    level = str(data.get("level", "info")).strip().lower() or "info"
    if level not in {"info", "warning", "error"}:
        level = "info"

    message = str(data.get("message", "")).strip()
    if not message:
        raise ValueError("message is required")

    time_text = str(data.get("time", "")).strip()
    if not time_text:
        time_text = datetime.now().strftime("%Y-%m-%d %H:%M")

    return {
        "id": event_id,
        "app": app_name,
        "level": level,
        "time": time_text,
        "message": message,
        "_received_ts": int(time.time()),
    }


@app.get("/health")
def health():
    return json_response({"ok": True})


@app.get("/status")
def status():
    if not check_token():
        return json_response({"error": "unauthorized"}, 401)
    return json_response(collect_status())


@app.get("/event/current")
def event_current():
    if not check_token():
        return json_response({"error": "unauthorized"}, 401)
    return json_response({"event": current_event()})


@app.post("/event/report")
def event_report():
    if not check_token():
        return json_response({"error": "unauthorized"}, 401)

    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        return json_response({"error": "invalid json body"}, 400)

    try:
        event = normalize_event_payload(data)
    except ValueError as exc:
        return json_response({"error": str(exc)}, 400)

    write_event_store(event)
    return json_response({"ok": True, "event": current_event()})


@app.post("/event/clear")
def event_clear():
    if not check_token():
        return json_response({"error": "unauthorized"}, 401)
    write_event_store(None)
    return json_response({"ok": True, "event": None})


if __name__ == "__main__":
    ensure_parent(EVENT_STORE_PATH)
    app.run(host=HOST, port=PORT, debug=False)
