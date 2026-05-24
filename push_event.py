#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from pathlib import Path
from typing import Any


DEFAULT_SERVER = "http://127.0.0.1:8099"
DEFAULT_TOKEN = "changeme"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Push a NAS app notification to the local ESP8266 NAS message server."
    )
    parser.add_argument("--server", default=DEFAULT_SERVER, help="Server base URL, e.g. http://127.0.0.1:8099")
    parser.add_argument("--token", default=DEFAULT_TOKEN, help="Status server token")
    parser.add_argument("--app", help="App name, e.g. Docker")
    parser.add_argument("--level", choices=["info", "warning", "error"], help="Severity level")
    parser.add_argument("--message", help="Event message text")
    parser.add_argument("--time", dest="time_text", help="Display time, default is current local time")
    parser.add_argument("--id", dest="event_id", help="Event id; if omitted, one is generated")
    parser.add_argument("--json-file", type=Path, help="Read a raw upstream JSON payload from a file")
    parser.add_argument(
        "--stdin-json",
        action="store_true",
        help="Read a raw upstream JSON payload from stdin",
    )
    parser.add_argument(
        "--print-payload",
        action="store_true",
        help="Print normalized payload before POST",
    )
    return parser


def load_raw_json(args: argparse.Namespace) -> dict[str, Any] | None:
    if args.json_file:
        return json.loads(args.json_file.read_text(encoding="utf-8"))
    if args.stdin_json:
        text = sys.stdin.read().strip()
        if not text:
            raise ValueError("stdin JSON is empty")
        return json.loads(text)
    return None


def first_non_empty(data: dict[str, Any], keys: tuple[str, ...]) -> str:
    for key in keys:
        value = data.get(key)
        if value is None:
            continue
        text = str(value).strip()
        if text:
            return text
    return ""


def normalize_payload(args: argparse.Namespace, raw: dict[str, Any] | None) -> dict[str, str]:
    raw = raw or {}

    app_name = args.app or first_non_empty(raw, ("app", "app_name", "source", "module", "title"))
    level = args.level or first_non_empty(raw, ("level", "severity", "priority", "status"))
    message = args.message or first_non_empty(raw, ("message", "content", "text", "body", "description"))
    time_text = args.time_text or first_non_empty(raw, ("time", "timestamp", "created_at", "createdAt"))
    event_id = args.event_id or first_non_empty(raw, ("id", "event_id", "eventId", "uuid"))

    if not app_name:
        app_name = "NAS"
    if not level:
        level = "info"
    level = level.lower()
    if level not in {"info", "warning", "error"}:
        level = "info"

    if not message:
        raise ValueError("message is required; pass --message or provide it in JSON")

    if not time_text:
        time_text = datetime.now().strftime("%Y-%m-%d %H:%M")

    if not event_id:
        digest = hashlib.md5(f"{app_name}|{level}|{time_text}|{message}".encode("utf-8")).hexdigest()[:10]
        stamp = datetime.now().strftime("%Y%m%d%H%M%S")
        event_id = f"msg-{stamp}-{digest}"

    return {
        "id": event_id,
        "app": app_name,
        "level": level,
        "time": time_text,
        "message": message,
    }


def post_event(server: str, token: str, payload: dict[str, str]) -> dict[str, Any]:
    base = server.rstrip("/")
    url = f"{base}/event/report?token={urllib.parse.quote(token)}"
    request_data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=request_data,
        headers={"Content-Type": "application/json; charset=utf-8"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        body = resp.read().decode("utf-8")
        return json.loads(body)


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    try:
        raw = load_raw_json(args)
        payload = normalize_payload(args, raw)
        if args.print_payload:
            print(json.dumps(payload, ensure_ascii=False, indent=2))
        response = post_event(args.server, args.token, payload)
        print(json.dumps(response, ensure_ascii=False, indent=2))
        return 0
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        print(f"HTTP {exc.code}: {body}", file=sys.stderr)
        return 1
    except urllib.error.URLError as exc:
        print(f"Request failed: {exc}", file=sys.stderr)
        return 1
    except (ValueError, json.JSONDecodeError) as exc:
        print(f"Invalid input: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
