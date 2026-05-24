#!/usr/bin/env python3

from __future__ import annotations

import os
import signal
import subprocess
import sys
import time
from typing import Final


FALSE_VALUES: Final = {"0", "false", "no", "off", ""}


def env_enabled(name: str, default: str = "1") -> bool:
    return os.getenv(name, default).strip().lower() not in FALSE_VALUES


def terminate_processes(processes: list[tuple[str, subprocess.Popen[str]]], sig: int = signal.SIGTERM) -> None:
    for _, proc in processes:
        if proc.poll() is None:
            proc.send_signal(sig)


def wait_processes(processes: list[tuple[str, subprocess.Popen[str]]], timeout: float = 10.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if all(proc.poll() is not None for _, proc in processes):
            return
        time.sleep(0.2)
    for _, proc in processes:
        if proc.poll() is None:
            proc.kill()


def build_processes() -> list[tuple[str, list[str]]]:
    python = sys.executable
    run_server = env_enabled("RUN_SERVER", "1")
    run_bridge = env_enabled("RUN_BRIDGE", "1")

    processes: list[tuple[str, list[str]]] = []

    if run_server:
        processes.append(("nas-message-server", [python, "-u", "nas_status_server.py"]))

    if run_bridge:
        bridge_config = os.getenv("BRIDGE_CONFIG", "/app/bridge_config.json").strip() or "/app/bridge_config.json"
        bridge_cmd = [python, "-u", "bridge_poll.py", "--config", bridge_config]
        if run_server:
            bridge_cmd.extend(
                [
                    "--server-url",
                    f"http://127.0.0.1:{os.getenv('PORT', '8099').strip() or '8099'}",
                    "--server-token",
                    os.getenv("TOKEN", "changeme").strip() or "changeme",
                ]
            )
        processes.append(("nas-message-bridge", bridge_cmd))

    return processes


def main() -> int:
    planned = build_processes()
    if not planned:
        print("Nothing to run: both RUN_SERVER and RUN_BRIDGE are disabled.", file=sys.stderr, flush=True)
        return 1

    children: list[tuple[str, subprocess.Popen[str]]] = []
    stopping = False

    def request_stop(signum: int, _frame) -> None:
        nonlocal stopping
        if stopping:
            return
        stopping = True
        print(f"Received signal {signum}, stopping child processes...", flush=True)
        terminate_processes(children, signal.SIGTERM)

    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)

    for name, cmd in planned:
        print(f"Starting {name}: {' '.join(cmd)}", flush=True)
        children.append((name, subprocess.Popen(cmd, text=True)))

    exit_code = 0
    try:
        while True:
            for name, proc in children:
                code = proc.poll()
                if code is None:
                    continue
                if not stopping:
                    print(f"{name} exited with code {code}, stopping remaining processes...", flush=True)
                    stopping = True
                    terminate_processes(children, signal.SIGTERM)
                    exit_code = code if code != 0 else 1
                wait_processes(children)
                return exit_code
            time.sleep(0.5)
    finally:
        if children:
            terminate_processes(children, signal.SIGTERM)
            wait_processes(children)


if __name__ == "__main__":
    raise SystemExit(main())
