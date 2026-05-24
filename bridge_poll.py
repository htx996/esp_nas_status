#!/usr/bin/env python3

from __future__ import annotations

import argparse
import base64
import copy
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from datetime import datetime
from pathlib import Path
from typing import Any

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

import push_event


DEFAULT_CONFIG = Path("bridge_config.json")
DEFAULT_STATE = Path("data/bridge_state.json")
DEFAULT_INTERVAL = 30
DEFAULT_UGREEN_CLIENT_VERSION = "1.16.0.77932"
DEFAULT_UGREEN_MESSAGE_PATH = "/ugreen/v1/desktop/message/list"
DEFAULT_UGREEN_VERIFY_CHECK_PATH = "/ugreen/v1/verify/check"
DEFAULT_UGREEN_VERIFY_LOGIN_PATH = "/ugreen/v1/verify/login"
UGREEN_RELOGIN_CODES = {1010, 1024}
PLACEHOLDER_PASSWORDS = {"changeme", "change_me", "change-me", "your-password", "your_password", "changeme!"}
DEFAULT_ALLOW_APPS = [
    "Docker",
    "SAN Manager",
    "UGREEN AI",
    "存储管理",
    "监控中心",
    "控制面板",
    "任务中心",
    "Task Center",
    "日志中心",
    "网盘工具",
    "文件管理",
    "迅雷",
    "应用中心",
]
DEFAULT_APP_ALIASES = {}
DEFAULT_TEXT_REPLACEMENTS = []
DEFAULT_CHAR_REPLACEMENTS = {
    "\r": " ",
    "\n": " ",
    "\t": " ",
    "，": ",",
    "。": ".",
    "：": ":",
    "；": ";",
    "（": "(",
    "）": ")",
    "【": "[",
    "】": "]",
    "《": "<",
    "》": ">",
    "“": "\"",
    "”": "\"",
    "‘": "'",
    "’": "'",
    "！": "!",
    "？": "?",
    "、": ",",
    "·": " ",
    "—": "-",
    "－": "-",
    "～": "~",
    "℃": "C",
}

APP_KEYS = ("app", "app_name", "source", "module", "title")
LEVEL_KEYS = ("level", "severity", "priority", "status")
MESSAGE_KEYS = ("message", "content", "text", "body", "description")
TIME_KEYS = ("time", "timestamp", "created_at", "createdAt", "date")
ID_KEYS = ("id", "event_id", "eventId", "uuid")
SUBJECT_HINT_KEYS = (
    "name",
    "title",
    "subject",
    "target_name",
    "targetname",
    "item_name",
    "itemname",
    "resource_name",
    "resourcename",
    "package_name",
    "packagename",
    "plugin_name",
    "pluginname",
    "display_name",
    "displayname",
    "app_name",
    "appname",
)
GENERIC_SUBJECTS = {"nas", "应用中心", "控制面板", "消息中心", "系统通知", "通知", "消息"}
ACTION_ONLY_MESSAGES = {
    "安装成功",
    "安装失败",
    "卸载成功",
    "卸载失败",
    "升级成功",
    "升级失败",
    "更新成功",
    "更新失败",
    "下载成功",
    "下载失败",
}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Poll an upstream message source and forward the latest unseen event.")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG, help="Bridge config JSON path")
    parser.add_argument("--server-url", help="Override server_url from config")
    parser.add_argument("--server-token", help="Override server_token from config")
    parser.add_argument("--interval", type=int, help="Override poll_interval_sec from config")
    parser.add_argument("--once", action="store_true", help="Run one poll and exit")
    parser.add_argument("--dry-run", action="store_true", help="Do not POST, only print the normalized event")
    parser.add_argument("--print-source", action="store_true", help="Print raw source JSON for debugging")
    return parser


def env_is_set(name: str) -> bool:
    return name in os.environ and os.environ[name].strip() != ""


def parse_env_bool(name: str, default: bool) -> bool:
    if not env_is_set(name):
        return default
    return os.environ[name].strip().lower() not in {"0", "false", "no", "off"}


def parse_env_int(name: str, default: int) -> int:
    if not env_is_set(name):
        return default
    return int(os.environ[name].strip())


def parse_env_float(name: str, default: float) -> float:
    if not env_is_set(name):
        return default
    return float(os.environ[name].strip())


def parse_env_list(name: str, default: list[str]) -> list[str]:
    raw = os.getenv(name, "").strip()
    if not raw:
        return list(default)
    items = [part.strip() for part in re.split(r"[\n,;，；]+", raw) if part.strip()]
    return items or list(default)


def deep_merge_dict(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
    merged = dict(base)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(merged.get(key), dict):
            merged[key] = deep_merge_dict(merged[key], value)
        else:
            merged[key] = value
    return merged


def build_env_config() -> dict[str, Any]:
    port = os.getenv("PORT", "8099").strip() or "8099"
    server_token = os.getenv("TOKEN", push_event.DEFAULT_TOKEN).strip() or push_event.DEFAULT_TOKEN
    username = os.getenv("UGREEN_NAS_USERNAME", "your-username").strip() or "your-username"
    password_env = os.getenv("UGREEN_PASSWORD_ENV", "UGREEN_NAS_PASSWORD").strip() or "UGREEN_NAS_PASSWORD"

    return {
        "server_url": os.getenv("BRIDGE_SERVER_URL", f"http://127.0.0.1:{port}").strip() or f"http://127.0.0.1:{port}",
        "server_token": server_token,
        "poll_interval_sec": parse_env_int("UGREEN_POLL_INTERVAL_SEC", 10),
        "state_path": os.getenv("BRIDGE_STATE_PATH", str(DEFAULT_STATE)).strip() or str(DEFAULT_STATE),
        "allow_apps": parse_env_list("UGREEN_ALLOW_APPS", DEFAULT_ALLOW_APPS),
        "items_order": os.getenv("UGREEN_ITEMS_ORDER", "newest_first").strip() or "newest_first",
        "level_map": {
            "important": "warning",
            "success": "info",
        },
        "source": {
            "mode": os.getenv("UGREEN_SOURCE_MODE", "ugreen_message_api").strip() or "ugreen_message_api",
            "base_url": os.getenv("UGREEN_BASE_URL", "http://127.0.0.1:8023").strip() or "http://127.0.0.1:8023",
            "client_version": os.getenv("UGREEN_CLIENT_VERSION", DEFAULT_UGREEN_CLIENT_VERSION).strip() or DEFAULT_UGREEN_CLIENT_VERSION,
            "locale": os.getenv("UGREEN_LOCALE", "zh-CN").strip() or "zh-CN",
            "payload": {
                "level": parse_env_list("UGREEN_LEVELS", ["info", "important", "warning"]),
                "page": parse_env_int("UGREEN_PAGE", 1),
                "size": parse_env_int("UGREEN_PAGE_SIZE", 1000),
                "module": os.getenv("UGREEN_MODULE", "ALL").strip() or "ALL",
                "all_limits": parse_env_int("UGREEN_ALL_LIMITS", 30),
            },
            "timeout_sec": parse_env_float("UGREEN_TIMEOUT_SEC", 10.0),
            "login": {
                "username": username,
                "password_env": password_env,
                "keepalive": parse_env_bool("UGREEN_KEEPALIVE", True),
                "otp": parse_env_bool("UGREEN_OTP", True),
                "is_simple": parse_env_bool("UGREEN_IS_SIMPLE", False),
            },
        },
        "items_path": os.getenv("UGREEN_ITEMS_PATH", "data.List").strip() or "data.List",
    }


def build_env_overrides() -> dict[str, Any]:
    overrides: dict[str, Any] = {}

    if env_is_set("TOKEN"):
        overrides["server_token"] = os.environ["TOKEN"].strip()
    if env_is_set("PORT") and not env_is_set("BRIDGE_SERVER_URL"):
        overrides["server_url"] = f"http://127.0.0.1:{os.environ['PORT'].strip()}"
    if env_is_set("BRIDGE_SERVER_URL"):
        overrides["server_url"] = os.environ["BRIDGE_SERVER_URL"].strip()
    if env_is_set("UGREEN_POLL_INTERVAL_SEC"):
        overrides["poll_interval_sec"] = parse_env_int("UGREEN_POLL_INTERVAL_SEC", 10)
    if env_is_set("BRIDGE_STATE_PATH"):
        overrides["state_path"] = os.environ["BRIDGE_STATE_PATH"].strip()
    if env_is_set("UGREEN_ALLOW_APPS"):
        overrides["allow_apps"] = parse_env_list("UGREEN_ALLOW_APPS", DEFAULT_ALLOW_APPS)
    if env_is_set("UGREEN_ITEMS_ORDER"):
        overrides["items_order"] = os.environ["UGREEN_ITEMS_ORDER"].strip()
    if any(env_is_set(name) for name in ("UGREEN_SOURCE_MODE", "UGREEN_BASE_URL", "UGREEN_CLIENT_VERSION", "UGREEN_LOCALE", "UGREEN_TIMEOUT_SEC", "UGREEN_MODULE", "UGREEN_PAGE", "UGREEN_PAGE_SIZE", "UGREEN_ALL_LIMITS", "UGREEN_LEVELS", "UGREEN_ITEMS_PATH", "UGREEN_NAS_USERNAME", "UGREEN_PASSWORD_ENV", "UGREEN_KEEPALIVE", "UGREEN_OTP", "UGREEN_IS_SIMPLE")):
        source_override: dict[str, Any] = {}
        login_override: dict[str, Any] = {}
        payload_override: dict[str, Any] = {}

        if env_is_set("UGREEN_SOURCE_MODE"):
            source_override["mode"] = os.environ["UGREEN_SOURCE_MODE"].strip()
        if env_is_set("UGREEN_BASE_URL"):
            source_override["base_url"] = os.environ["UGREEN_BASE_URL"].strip()
        if env_is_set("UGREEN_CLIENT_VERSION"):
            source_override["client_version"] = os.environ["UGREEN_CLIENT_VERSION"].strip()
        if env_is_set("UGREEN_LOCALE"):
            source_override["locale"] = os.environ["UGREEN_LOCALE"].strip()
        if env_is_set("UGREEN_TIMEOUT_SEC"):
            source_override["timeout_sec"] = parse_env_float("UGREEN_TIMEOUT_SEC", 10.0)
        if env_is_set("UGREEN_LEVELS"):
            payload_override["level"] = parse_env_list("UGREEN_LEVELS", ["info", "important", "warning"])
        if env_is_set("UGREEN_PAGE"):
            payload_override["page"] = parse_env_int("UGREEN_PAGE", 1)
        if env_is_set("UGREEN_PAGE_SIZE"):
            payload_override["size"] = parse_env_int("UGREEN_PAGE_SIZE", 1000)
        if env_is_set("UGREEN_MODULE"):
            payload_override["module"] = os.environ["UGREEN_MODULE"].strip()
        if env_is_set("UGREEN_ALL_LIMITS"):
            payload_override["all_limits"] = parse_env_int("UGREEN_ALL_LIMITS", 30)
        if env_is_set("UGREEN_NAS_USERNAME"):
            login_override["username"] = os.environ["UGREEN_NAS_USERNAME"].strip()
        if env_is_set("UGREEN_PASSWORD_ENV"):
            login_override["password_env"] = os.environ["UGREEN_PASSWORD_ENV"].strip()
        if env_is_set("UGREEN_KEEPALIVE"):
            login_override["keepalive"] = parse_env_bool("UGREEN_KEEPALIVE", True)
        if env_is_set("UGREEN_OTP"):
            login_override["otp"] = parse_env_bool("UGREEN_OTP", True)
        if env_is_set("UGREEN_IS_SIMPLE"):
            login_override["is_simple"] = parse_env_bool("UGREEN_IS_SIMPLE", False)
        if payload_override:
            source_override["payload"] = payload_override
        if login_override:
            source_override["login"] = login_override
        if source_override:
            overrides["source"] = source_override
        if env_is_set("UGREEN_ITEMS_PATH"):
            overrides["items_path"] = os.environ["UGREEN_ITEMS_PATH"].strip()

    return overrides


def load_config(path: Path) -> dict[str, Any]:
    config = build_env_config()
    if path.exists():
        file_config = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(file_config, dict):
            raise ValueError("config root must be an object")
        config = deep_merge_dict(config, file_config)
    config = deep_merge_dict(config, build_env_overrides())
    return config


def ensure_parent(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def read_state(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def write_state(path: Path, state: dict[str, Any]) -> None:
    ensure_parent(path)
    path.write_text(json.dumps(state, ensure_ascii=False, indent=2), encoding="utf-8")


def request_json(
    url: str,
    *,
    method: str = "GET",
    headers: dict[str, Any] | None = None,
    json_body: Any = None,
    timeout: float = 10,
) -> tuple[Any, dict[str, str]]:
    request_headers = {str(k): str(v) for k, v in (headers or {}).items()}
    data = None
    if json_body is not None:
        data = json.dumps(json_body, ensure_ascii=False).encode("utf-8")
        if "Content-Type" not in request_headers and "content-type" not in request_headers:
            request_headers["Content-Type"] = "application/json; charset=utf-8"

    req = urllib.request.Request(url, data=data, headers=request_headers, method=method.upper())
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        body = resp.read().decode("utf-8", errors="replace")
        response_headers = {str(k).lower(): str(v) for k, v in resp.headers.items()}

    if not body.strip():
        return {}, response_headers
    return json.loads(body), response_headers


def fetch_current_server_event(server: str, token: str) -> dict[str, Any] | None:
    url = f"{server.rstrip('/')}/event/current?token={urllib.parse.quote(token)}"
    try:
        payload, _ = request_json(url, timeout=10)
    except (urllib.error.URLError, urllib.error.HTTPError, json.JSONDecodeError):
        return None
    if not isinstance(payload, dict):
        return None
    event = payload.get("event")
    return event if isinstance(event, dict) else None


def fetch_http_json(source: dict[str, Any]) -> Any:
    url = str(source.get("url", "")).strip()
    if not url:
        raise ValueError("source.url is required for http_json mode")

    headers = source.get("headers", {})
    if not isinstance(headers, dict):
        headers = {}

    method = str(source.get("method", "GET")).strip().upper()
    if method not in {"GET", "POST"}:
        raise ValueError(f"Unsupported source.method: {method}")

    payload = None
    if method == "POST":
        payload = source.get("json_body")
        if payload is None:
            payload = source.get("body")

    response, _ = request_json(
        url,
        method=method,
        headers=headers,
        json_body=payload,
        timeout=float(source.get("timeout_sec", 10)),
    )
    return response


def fetch_command_json(source: dict[str, Any], base_dir: Path) -> Any:
    command = source.get("command")
    if isinstance(command, str):
        completed = subprocess.run(command, shell=True, capture_output=True, text=True, check=True, cwd=base_dir)
    elif isinstance(command, list) and command:
        completed = subprocess.run([str(part) for part in command], capture_output=True, text=True, check=True, cwd=base_dir)
    else:
        raise ValueError("source.command is required for command_json mode")

    output = completed.stdout.strip()
    if not output:
        raise ValueError("source command returned empty stdout")
    return json.loads(output)


def get_path_value(payload: Any, path: str) -> Any:
    current = payload
    for part in path.split("."):
        if not part:
            continue
        if isinstance(current, dict):
            current = current.get(part)
        elif isinstance(current, list) and part.isdigit():
            index = int(part)
            current = current[index] if 0 <= index < len(current) else None
        else:
            return None
        if current is None:
            return None
    return current


def merge_text_pairs(config_value: Any, defaults: list[tuple[str, str]]) -> list[tuple[str, str]]:
    pairs = list(defaults)
    if isinstance(config_value, dict):
        for old, new in config_value.items():
            old_text = str(old)
            if old_text:
                pairs.append((old_text, str(new)))
        return pairs
    if isinstance(config_value, list):
        for item in config_value:
            if isinstance(item, (list, tuple)) and len(item) == 2:
                old_text = str(item[0])
                if old_text:
                    pairs.append((old_text, str(item[1])))
    return pairs


def merge_text_map(config_value: Any, defaults: dict[str, str]) -> dict[str, str]:
    merged = dict(defaults)
    if isinstance(config_value, dict):
        for key, value in config_value.items():
            merged[str(key)] = str(value)
    return merged


def is_notice_safe_char(ch: str) -> bool:
    if len(ch) != 1:
        return False
    codepoint = ord(ch)
    if 32 <= codepoint <= 126:
        return True
    try:
        ch.encode("gb2312")
        return True
    except UnicodeEncodeError:
        return False


def collapse_spaces(text: str) -> str:
    return " ".join(text.split())


def cleanup_notice_text(text: str) -> str:
    collapsed = collapse_spaces(text)
    collapsed = re.sub(r"\s+([,.;:!?])", r"\1", collapsed)
    collapsed = re.sub(r"([,.;:!?])[,. ;:!?]+", r"\1", collapsed)
    return collapsed.strip(" ,.;:!?-_")


def normalize_notice_text(text: Any, config: dict[str, Any], *, kind: str) -> str:
    value = str(text or "").strip()
    if not value:
        return "NAS" if kind == "app" else ""

    if kind == "app":
        app_aliases = merge_text_map(config.get("app_aliases"), DEFAULT_APP_ALIASES)
        value = app_aliases.get(value, value)

    for old, new in merge_text_pairs(config.get("text_replacements"), DEFAULT_TEXT_REPLACEMENTS):
        value = value.replace(old, new)

    char_replacements = merge_text_map(config.get("char_replacements"), DEFAULT_CHAR_REPLACEMENTS)
    normalized_parts: list[str] = []
    for ch in value:
        replacement = char_replacements.get(ch)
        if replacement is not None:
            normalized_parts.append(replacement)
            continue
        if is_notice_safe_char(ch):
            normalized_parts.append(ch)

    normalized = cleanup_notice_text("".join(normalized_parts))
    if normalized:
        return normalized
    return "NAS" if kind == "app" else "消息更新"


def collect_subject_candidates(payload: Any, config: dict[str, Any], results: list[str], *, depth: int = 0) -> None:
    if depth > 2:
        return
    if isinstance(payload, dict):
        for key, value in payload.items():
            key_text = str(key).strip().lower().replace("-", "_")
            if isinstance(value, str):
                if any(hint in key_text for hint in SUBJECT_HINT_KEYS):
                    candidate = normalize_notice_text(value, config, kind="message")
                    if candidate:
                        results.append(candidate)
            else:
                collect_subject_candidates(value, config, results, depth=depth + 1)
    elif isinstance(payload, list):
        for value in payload:
            collect_subject_candidates(value, config, results, depth=depth + 1)


def choose_subject_candidate(item: dict[str, Any], config: dict[str, Any], app_name: str, message: str) -> str:
    candidates: list[str] = []
    collect_subject_candidates(item, config, candidates)
    seen: set[str] = set()
    for candidate in candidates:
        value = candidate.strip()
        lower_value = value.lower()
        if not value or value in seen:
            continue
        seen.add(value)
        if value == app_name or value == message:
            continue
        if lower_value in GENERIC_SUBJECTS:
            continue
        if value in message:
            continue
        if len(value) > 20:
            continue
        return value
    return ""


def enrich_action_only_message(item: dict[str, Any], config: dict[str, Any], app_name: str, message: str) -> str:
    if message not in ACTION_ONLY_MESSAGES:
        return message
    if app_name != "应用中心":
        return message
    subject = choose_subject_candidate(item, config, app_name, message)
    if not subject:
        return message
    return f"{subject}{message}"


def rsa_encrypt_b64(public_key_pem: str, text: str) -> str:
    public_key = serialization.load_pem_public_key(public_key_pem.encode("utf-8"))
    encrypted = public_key.encrypt(text.encode("utf-8"), padding.PKCS1v15())
    return base64.b64encode(encrypted).decode("ascii")


def aes_gcm_encrypt_b64(key_text: str, plaintext: str) -> str:
    iv = os.urandom(12)
    ciphertext = AESGCM(key_text.encode("utf-8")).encrypt(iv, plaintext.encode("utf-8"), None)
    return base64.b64encode(iv + ciphertext).decode("ascii")


def aes_gcm_decrypt_b64(key_text: str, payload: str) -> str:
    raw = base64.b64decode(payload)
    iv = raw[:12]
    ciphertext = raw[12:]
    return AESGCM(key_text.encode("utf-8")).decrypt(iv, ciphertext, None).decode("utf-8")


def normalize_pem_text(raw: Any) -> str:
    text = str(raw or "").strip()
    if not text:
        return ""
    text = text.replace("\\n", "\n")
    if "BEGIN " in text:
        return text if text.endswith("\n") else text + "\n"
    try:
        decoded = base64.b64decode(text).decode("utf-8")
        decoded = decoded.replace("\\n", "\n").strip()
        if "BEGIN " in decoded:
            return decoded if decoded.endswith("\n") else decoded + "\n"
    except Exception:
        pass
    return text if text.endswith("\n") else text + "\n"


def resolve_password(login: dict[str, Any], base_dir: Path) -> str:
    env_name = str(login.get("password_env", "")).strip()
    if env_name:
        env_value = os.getenv(env_name, "").strip()
        if env_value:
            return env_value

    file_value = str(login.get("password_file", "")).strip()
    if file_value:
        password_path = Path(file_value)
        if not password_path.is_absolute():
            password_path = base_dir / password_path
        text = password_path.read_text(encoding="utf-8").strip()
        if text and text.lower() not in PLACEHOLDER_PASSWORDS:
            return text

    password = str(login.get("password", "")).strip()
    if password and password.lower() not in PLACEHOLDER_PASSWORDS:
        return password

    raise ValueError("UGREEN login password is not configured; set source.login.password, password_env, or password_file")


def resolve_ugreen_base_url(source: dict[str, Any]) -> str:
    base_url = str(source.get("base_url", "")).strip().rstrip("/")
    if not base_url:
        raise ValueError("source.base_url is required for ugreen_message_api mode")
    return base_url


def resolve_ugreen_client_id(source: dict[str, Any], state: dict[str, Any]) -> str:
    configured = str(source.get("client_id", "")).strip()
    if configured:
        return configured

    from_state = str(state.get("ugreen_client_id", "")).strip()
    if from_state:
        return from_state

    generated = uuid.uuid4().hex
    state["ugreen_client_id"] = generated
    return generated


def resolve_ugreen_context(source: dict[str, Any], state: dict[str, Any]) -> dict[str, Any]:
    return {
        "base_url": resolve_ugreen_base_url(source),
        "client_id": resolve_ugreen_client_id(source, state),
        "client_version": str(source.get("client_version", DEFAULT_UGREEN_CLIENT_VERSION)).strip()
        or DEFAULT_UGREEN_CLIENT_VERSION,
        "locale": str(source.get("locale", "zh-CN")).strip() or "zh-CN",
        "timeout": float(source.get("timeout_sec", 10)),
    }


def build_ugreen_common_headers(context: dict[str, Any]) -> dict[str, str]:
    return {
        "UG-Agent": "PC/WEB",
        "UG-Client-Id": context["client_id"],
        "Client-Id": context["client_id"],
        "Client-Version": context["client_version"],
        "Cache-Control": "no-cache",
        "X-Specify-Language": context["locale"],
    }


def build_url(base_url: str, path: str) -> str:
    if path.startswith("http://") or path.startswith("https://"):
        return path
    return f"{base_url}{path}"


def parse_ugreen_code(payload: Any) -> int | None:
    if not isinstance(payload, dict):
        return None
    code = payload.get("code")
    if isinstance(code, int):
        return code
    if isinstance(code, str) and code.isdigit():
        return int(code)
    return None


def parse_ugreen_msg(payload: Any) -> str:
    if not isinstance(payload, dict):
        return ""
    message = payload.get("msg")
    if message is None:
        message = payload.get("message")
    return str(message or "").strip()


def extract_ugreen_session(source: dict[str, Any], state: dict[str, Any], context: dict[str, Any]) -> dict[str, Any] | None:
    candidate = state.get("ugreen_session")
    if isinstance(candidate, dict):
        api_token = str(candidate.get("api_token", "")).strip()
        public_key = normalize_pem_text(candidate.get("public_key", ""))
        if api_token and public_key and str(candidate.get("base_url", context["base_url"])).strip() == context["base_url"]:
            return {
                "api_token": api_token,
                "public_key": public_key,
                "token_where": str(candidate.get("token_where", "header")).strip().lower() or "header",
                "static_token": str(candidate.get("static_token", "")).strip(),
                "third_token": str(candidate.get("third_token", "")).strip(),
                "username": str(candidate.get("username", "")).strip(),
                "client_id": context["client_id"],
                "client_version": context["client_version"],
                "base_url": context["base_url"],
                "updated_at": str(candidate.get("updated_at", "")).strip(),
            }

    api_token = str(source.get("api_token", "")).strip()
    public_key = normalize_pem_text(source.get("public_key", ""))
    if api_token and public_key:
        return {
            "api_token": api_token,
            "public_key": public_key,
            "token_where": str(source.get("token_where", "header")).strip().lower() or "header",
            "static_token": str(source.get("static_token", "")).strip(),
            "third_token": str(source.get("third_token", "")).strip(),
            "username": str(source.get("username", "")).strip(),
            "client_id": context["client_id"],
            "client_version": context["client_version"],
            "base_url": context["base_url"],
            "updated_at": "",
        }
    return None


def has_ugreen_login(source: dict[str, Any]) -> bool:
    login = source.get("login")
    return isinstance(login, dict) and bool(str(login.get("username", "")).strip())


def perform_ugreen_login(source: dict[str, Any], state: dict[str, Any], base_dir: Path, context: dict[str, Any]) -> dict[str, Any]:
    login = source.get("login")
    if not isinstance(login, dict):
        raise ValueError("source.login is required for automatic UGREEN login")

    username = str(login.get("username", "")).strip()
    if not username or username.lower() in {"your-username", "changeme", "change-me", "change_me"}:
        raise ValueError("UGREEN NAS username is not configured; set UGREEN_NAS_USERNAME or source.login.username")

    password_mode = str(login.get("password_mode", "rsa")).strip().lower() or "rsa"
    password = resolve_password(login, base_dir) if password_mode == "rsa" else ""
    common_headers = build_ugreen_common_headers(context)

    check_payload = {"username": username}
    check_response, check_headers = request_json(
        build_url(context["base_url"], str(source.get("verify_check_path", DEFAULT_UGREEN_VERIFY_CHECK_PATH))),
        method="POST",
        headers=common_headers,
        json_body=check_payload,
        timeout=context["timeout"],
    )
    check_code = parse_ugreen_code(check_response)
    if check_code not in (None, 200):
        raise ValueError(f"UGREEN verify/check failed: code={check_code} msg={parse_ugreen_msg(check_response)}")

    request_public_key = normalize_pem_text(check_headers.get("x-rsa-token", ""))
    if not request_public_key and password_mode == "rsa":
        raise ValueError("UGREEN verify/check did not return x-rsa-token")

    if password_mode == "uuid":
        password_value = uuid.uuid4().hex
    elif password_mode == "rsa":
        password_value = rsa_encrypt_b64(request_public_key, password)
    else:
        raise ValueError("source.login.password_mode must be 'rsa' or 'uuid'")

    login_payload = {
        "username": username,
        "password": password_value,
        "keepalive": bool(login.get("keepalive", True)),
        "otp": bool(login.get("otp", True)),
        "is_simple": bool(login.get("is_simple", False)),
    }
    if "login_event" in login:
        login_payload["login_event"] = login.get("login_event")

    login_response, _ = request_json(
        build_url(context["base_url"], str(source.get("verify_login_path", DEFAULT_UGREEN_VERIFY_LOGIN_PATH))),
        method="POST",
        headers=common_headers,
        json_body=login_payload,
        timeout=context["timeout"],
    )

    login_code = parse_ugreen_code(login_response)
    if login_code not in (None, 200):
        raise ValueError(f"UGREEN verify/login failed: code={login_code} msg={parse_ugreen_msg(login_response)}")

    if isinstance(login_response, dict) and isinstance(login_response.get("data"), dict):
        login_data = login_response["data"]
    else:
        login_data = login_response if isinstance(login_response, dict) else {}

    api_token = str(login_data.get("token") or login_response.get("token") or "").strip()
    request_key = normalize_pem_text(login_data.get("public_key") or login_response.get("public_key") or "")
    if not api_token or not request_key:
        raise ValueError("UGREEN verify/login response is missing token or public_key")

    session = {
        "api_token": api_token,
        "public_key": request_key,
        "token_where": str(login_data.get("auth_type") or login_response.get("auth_type") or "header").strip().lower()
        or "header",
        "static_token": str(login_data.get("static_token") or login_response.get("static_token") or "").strip(),
        "third_token": str(login_data.get("third_token") or login_response.get("third_token") or "").strip(),
        "username": username,
        "client_id": context["client_id"],
        "client_version": context["client_version"],
        "base_url": context["base_url"],
        "updated_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
    }
    state["ugreen_session"] = session
    return session


def ensure_ugreen_session(source: dict[str, Any], state: dict[str, Any], base_dir: Path, *, force_login: bool = False) -> tuple[dict[str, Any], dict[str, Any]]:
    context = resolve_ugreen_context(source, state)
    if force_login:
        if not has_ugreen_login(source):
            raise ValueError("UGREEN session expired but source.login is not configured")
        return context, perform_ugreen_login(source, state, base_dir, context)

    session = extract_ugreen_session(source, state, context)
    if session is not None and state.get("ugreen_session") == session:
        return context, session

    if session is not None and not has_ugreen_login(source):
        return context, session

    if session is not None and state.get("ugreen_session"):
        return context, session

    if has_ugreen_login(source):
        return context, perform_ugreen_login(source, state, base_dir, context)

    if session is not None:
        return context, session

    raise ValueError("UGREEN source is missing both source.login and source.api_token/public_key credentials")


def request_ugreen_api(
    source: dict[str, Any],
    context: dict[str, Any],
    session: dict[str, Any],
    *,
    path: str,
    method: str = "GET",
    params: dict[str, Any] | None = None,
    payload: dict[str, Any] | None = None,
) -> Any:
    key_text = os.urandom(16).hex()
    query_text = urllib.parse.urlencode(params or {}, doseq=True)
    encrypted_query = aes_gcm_encrypt_b64(key_text, query_text)
    request_body = None
    if payload is not None:
        body_json = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
        encrypted_body = aes_gcm_encrypt_b64(key_text, body_json)
        request_body = {
            "encrypt_req_body": encrypted_body,
            "req_body_sha256": hashlib.sha256(body_json.encode("utf-8")).hexdigest(),
        }

    url = f"{build_url(context['base_url'], path)}?{urllib.parse.urlencode({'encrypt_query': encrypted_query})}"
    headers = build_ugreen_common_headers(context)
    headers.update(
        {
            "Content-Type": "application/json",
            "X-Ugreen-Token": rsa_encrypt_b64(session["public_key"], session["api_token"]),
            "X-Ugreen-Security-Key": hashlib.md5(session["api_token"].encode("utf-8")).hexdigest(),
            "X-Ugreen-Security-Code": rsa_encrypt_b64(session["public_key"], key_text),
        }
    )

    response, _ = request_json(url, method=method, headers=headers, json_body=request_body, timeout=context["timeout"])
    if not isinstance(response, dict):
        raise ValueError("UGREEN API returned a non-object response")

    encrypted_response = response.get("encrypt_resp_body")
    if not isinstance(encrypted_response, str) or not encrypted_response:
        return response
    return json.loads(aes_gcm_decrypt_b64(key_text, encrypted_response))


def post_ugreen_message_list(source: dict[str, Any], context: dict[str, Any], session: dict[str, Any]) -> Any:
    payload = source.get("payload")
    if payload is None:
        payload = {
            "level": ["info", "important", "warning"],
            "page": 1,
            "size": 1000,
            "module": "ALL",
            "all_limits": 30,
        }
    if not isinstance(payload, dict):
        raise ValueError("source.payload must be an object")

    message_path = str(source.get("message_path", DEFAULT_UGREEN_MESSAGE_PATH)).strip() or DEFAULT_UGREEN_MESSAGE_PATH
    return request_ugreen_api(source, context, session, path=message_path, method="POST", payload=payload)


def should_retry_ugreen_login(payload: Any) -> bool:
    return parse_ugreen_code(payload) in UGREEN_RELOGIN_CODES


def fetch_ugreen_with_relogin(
    source: dict[str, Any],
    state: dict[str, Any],
    base_dir: Path,
    request_fn,
) -> Any:
    context, session = ensure_ugreen_session(source, state, base_dir)
    try:
        response = request_fn(context, session)
    except urllib.error.HTTPError as exc:
        if exc.code in {401, 403} and has_ugreen_login(source):
            context, session = ensure_ugreen_session(source, state, base_dir, force_login=True)
            response = request_fn(context, session)
        else:
            raise

    if should_retry_ugreen_login(response) and has_ugreen_login(source):
        context, session = ensure_ugreen_session(source, state, base_dir, force_login=True)
        response = request_fn(context, session)

    return response


def fetch_ugreen_message_json(config: dict[str, Any], state: dict[str, Any], base_dir: Path) -> Any:
    source = config.get("source") or {}
    if not isinstance(source, dict):
        raise ValueError("source config must be an object")
    return fetch_ugreen_with_relogin(
        source,
        state,
        base_dir,
        lambda context, session: post_ugreen_message_list(source, context, session),
    )


def detect_items(payload: Any, items_path: str | None) -> list[dict[str, Any]]:
    if items_path:
        payload = get_path_value(payload, items_path)

    if isinstance(payload, list):
        return [item for item in payload if isinstance(item, dict)]
    if isinstance(payload, dict):
        for key in ("notifications", "messages", "events", "items", "data", "rows"):
            value = payload.get(key)
            if isinstance(value, list):
                return [item for item in value if isinstance(item, dict)]
        return [payload]
    return []


def normalize_time_text(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, (int, float)):
        timestamp = float(value)
    else:
        text = str(value).strip()
        if not text:
            return ""
        if text.isdigit():
            timestamp = float(text)
        else:
            return text

    if timestamp > 10_000_000_000:
        timestamp /= 1000.0
    return datetime.fromtimestamp(timestamp).strftime("%Y-%m-%d %H:%M")


def normalize_item(item: dict[str, Any], config: dict[str, Any]) -> dict[str, str] | None:
    allow_apps = config.get("allow_apps") or []
    if not isinstance(allow_apps, list):
        allow_apps = []

    raw_app_name = push_event.first_non_empty(item, APP_KEYS) or "NAS"
    app_name = normalize_notice_text(raw_app_name, config, kind="app")
    raw_allow_apps = [str(v) for v in allow_apps]
    normalized_allow_apps = [normalize_notice_text(v, config, kind="app") for v in allow_apps]
    if allow_apps and raw_app_name not in raw_allow_apps and app_name not in normalized_allow_apps:
        return None

    level = push_event.first_non_empty(item, LEVEL_KEYS).lower() or "info"
    if level == "important":
        level = "warning"
    if level == "success":
        level = "info"
    level_map = config.get("level_map") or {}
    if isinstance(level_map, dict) and level in level_map:
        level = str(level_map[level]).lower()
    if level not in {"info", "warning", "error"}:
        level = "info"

    raw_message = push_event.first_non_empty(item, MESSAGE_KEYS)
    if not raw_message:
        return None
    message = normalize_notice_text(raw_message, config, kind="message")
    message = enrich_action_only_message(item, config, app_name, message)

    time_text = ""
    for key in TIME_KEYS:
        time_text = normalize_time_text(item.get(key))
        if time_text:
            break
    if not time_text:
        time_text = datetime.now().strftime("%Y-%m-%d %H:%M")

    event_id = push_event.first_non_empty(item, ID_KEYS)
    if not event_id:
        digest = hashlib.md5(f"{app_name}|{level}|{time_text}|{message}".encode("utf-8")).hexdigest()[:10]
        event_id = f"msg-{datetime.now().strftime('%Y%m%d%H%M%S')}-{digest}"

    return {
        "id": event_id,
        "app": app_name,
        "level": level,
        "time": time_text,
        "message": message,
    }


def choose_latest_event(items: list[dict[str, Any]], config: dict[str, Any]) -> dict[str, str] | None:
    order = str(config.get("items_order", "newest_first")).strip().lower()
    if order == "oldest_first":
        iterable = reversed(items)
    else:
        iterable = items

    for item in iterable:
        normalized = normalize_item(item, config)
        if normalized:
            return normalized
    return None


def fetch_source_payload(config: dict[str, Any], base_dir: Path, state: dict[str, Any]) -> Any:
    source = config.get("source") or {}
    if not isinstance(source, dict):
        raise ValueError("source config must be an object")

    mode = str(source.get("mode", "http_json")).strip().lower()
    if mode == "http_json":
        return fetch_http_json(source)
    if mode == "ugreen_message_api":
        return fetch_ugreen_message_json(config, state, base_dir)
    if mode == "command_json":
        return fetch_command_json(source, base_dir)
    raise ValueError(f"Unsupported source.mode: {mode}")


def apply_overrides(config: dict[str, Any], args: argparse.Namespace) -> dict[str, Any]:
    merged = dict(config)
    if args.server_url:
        merged["server_url"] = args.server_url
    if args.server_token:
        merged["server_token"] = args.server_token
    if args.interval is not None:
        merged["poll_interval_sec"] = args.interval
    return merged


def run_once(config_path: Path, args: argparse.Namespace) -> int:
    config = apply_overrides(load_json(config_path), args)
    base_dir = config_path.resolve().parent
    state_path = Path(str(config.get("state_path", DEFAULT_STATE)))
    if not state_path.is_absolute():
        state_path = base_dir / state_path
    state = read_state(state_path)
    original_state = copy.deepcopy(state)

    source_payload = fetch_source_payload(config, base_dir, state)
    if args.print_source:
        print(json.dumps(source_payload, ensure_ascii=False, indent=2))

    items = detect_items(source_payload, config.get("items_path"))
    latest = choose_latest_event(items, config)
    state_changed = state != original_state
    server = str(config.get("server_url", push_event.DEFAULT_SERVER))
    token = str(config.get("server_token", push_event.DEFAULT_TOKEN))

    if latest is None:
        if state_changed:
            write_state(state_path, state)
        print(json.dumps({"ok": True, "forwarded": False, "reason": "no mappable event"}, ensure_ascii=False))
        return 0

    if latest["id"] == str(state.get("last_event_id", "")):
        if not args.dry_run:
            current_event = fetch_current_server_event(server, token)
            current_event_id = str((current_event or {}).get("id", "")).strip()
            if current_event_id != latest["id"]:
                response = push_event.post_event(server, token, latest)
                state["last_forwarded_at"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                write_state(state_path, state)
                print(
                    json.dumps(
                        {
                            "ok": True,
                            "forwarded": True,
                            "reason": "resynced_current_event",
                            "event": latest,
                            "response": response,
                        },
                        ensure_ascii=False,
                        indent=2,
                    )
                )
                return 0
        if state_changed:
            write_state(state_path, state)
        print(json.dumps({"ok": True, "forwarded": False, "reason": "duplicate", "event": latest}, ensure_ascii=False))
        return 0

    if args.dry_run:
        if state_changed:
            write_state(state_path, state)
        print(json.dumps({"ok": True, "forwarded": False, "dry_run": True, "event": latest}, ensure_ascii=False, indent=2))
        return 0

    response = push_event.post_event(server, token, latest)

    state["last_event_id"] = latest["id"]
    state["last_forwarded_at"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    write_state(state_path, state)

    print(json.dumps({"ok": True, "forwarded": True, "event": latest, "response": response}, ensure_ascii=False, indent=2))
    return 0


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    try:
        config = apply_overrides(load_json(args.config), args)
        interval = int(config.get("poll_interval_sec", DEFAULT_INTERVAL))
        if args.once:
            return run_once(args.config, args)

        while True:
            try:
                run_once(args.config, args)
            except subprocess.CalledProcessError as exc:
                print(f"Source command failed: {exc}", file=sys.stderr)
            except urllib.error.URLError as exc:
                print(f"Source request failed: {exc}", file=sys.stderr)
            except urllib.error.HTTPError as exc:
                print(f"Source HTTP error {exc.code}: {exc.read().decode('utf-8', errors='replace')}", file=sys.stderr)
            except (ValueError, json.JSONDecodeError) as exc:
                print(f"Bridge error: {exc}", file=sys.stderr)
            time.sleep(max(1, interval))
    except FileNotFoundError as exc:
        print(f"Config not found: {exc}", file=sys.stderr)
        return 1
    except (ValueError, json.JSONDecodeError) as exc:
        print(f"Invalid config: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
