#!/usr/bin/env python3
"""Package compiler supervisor: one child, no asynchronous shell timer jobs."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time


def safe(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9._+:-]", "_", value)


def group_alive(pid: int) -> bool:
    try:
        os.killpg(pid, 0)
        return True
    except ProcessLookupError:
        return False


def stop_group(child: subprocess.Popen, grace: int) -> None:
    if group_alive(child.pid):
        try:
            os.killpg(child.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        deadline = time.monotonic() + grace
        while group_alive(child.pid) and time.monotonic() < deadline:
            child.poll()
            time.sleep(0.05)
        if group_alive(child.pid):
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    child.wait(timeout=10)


def main(argv: list[str]) -> int:
    environment = os.environ
    test_mode = environment.get("BIOSEQ_COMPILER_WATCHDOG_TEST_MODE", "0")
    timeout_override = environment.get("BIOSEQ_COMPILER_WATCHDOG_TIMEOUT_SEC", "")
    grace_override = environment.get("BIOSEQ_COMPILER_WATCHDOG_GRACE_SEC", "")
    error = ""
    if environment.get("BIOSEQ_COMPILER_RT_PRIORITY", "1") != "1":
        error = "bioseq package compiler priority must be exactly 1"
    elif test_mode not in ("0", "1"):
        error = "BIOSEQ_COMPILER_WATCHDOG_TEST_MODE must be exactly 1 when enabled"
    elif test_mode != "1" and (timeout_override or grace_override):
        error = "bioseq production compiler watchdog limits cannot be overridden"
    elif not argv:
        error = "bioseq compiler launcher requires a compiler argv"
    timeout_text, grace_text = timeout_override or "3300", grace_override or "60"
    if not all(re.fullmatch(r"[1-9][0-9]*", value) for value in (timeout_text, grace_text)):
        error = "bioseq compiler watchdog limits must be positive integer seconds"
    if error:
        print(error, file=sys.stderr)
        return 64
    for tool in ("chrt", "ps"):
        if not shutil.which(tool):
            print(f"{tool} is required for the bioseq package compiler launcher", file=sys.stderr)
            return 69
    timeout_sec, grace_sec = int(timeout_text), int(grace_text)
    cancellation = 0

    def cancelled(signum, _frame):
        nonlocal cancellation
        cancellation = signum

    for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, cancelled)
    # Establish the child process group before Popen returns. Waiting on this
    # one child avoids the shell's fast-exit/timer/SIGCHLD bookkeeping race.
    child = subprocess.Popen(["chrt", "--rr", "1", *argv], start_new_session=True)
    deadline = time.monotonic() + timeout_sec
    try:
        while not cancellation:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                status = child.wait(timeout=min(0.25, remaining))
                if group_alive(child.pid):
                    stop_group(child, grace_sec)
                    if status == 0:
                        status = 70
                return status if status >= 0 else 128 - status
            except subprocess.TimeoutExpired:
                continue
        if cancellation:
            stop_group(child, grace_sec)
            return 128 + cancellation

        rows = subprocess.check_output(["ps", "-eo", "pid=,pgid=,rss="], text=True, timeout=5)
        members = [tuple(map(int, line.split())) for line in rows.splitlines() if line.strip()]
        members = [row for row in members if row[1] == child.pid]
        stop_group(child, grace_sec)
        try:
            version = subprocess.check_output(
                [argv[0], "--version"], stdin=subprocess.DEVNULL,
                stderr=subprocess.STDOUT, text=True, timeout=5).splitlines()[0]
        except (OSError, subprocess.SubprocessError, IndexError):
            version = "unknown"
        suffixes = (".c", ".cc", ".cpp", ".cppm", ".cxx", ".ixx", ".m", ".mm")
        source = next((Path(arg).name for arg in reversed(argv) if arg.endswith(suffixes)), "unknown")
        digest = hashlib.sha256(b"".join(os.fsencode(arg) + b"\0" for arg in argv)).hexdigest()
        fields = {
            "source": safe(source),
            "profile": safe(environment.get("BIOSEQ_TEMPLATE_FLATTEN_PROFILE", "unknown")),
            "arch": safe(os.uname().machine), "compiler_version": safe(version),
            "argv_sha256": digest, "root_pid": child.pid, "pgid": child.pid,
            "pids": ",".join(str(row[0]) for row in sorted(members)) or "none",
            "rss_kib": sum(row[2] for row in members),
            "timeout_sec": timeout_sec, "grace_sec": grace_sec,
        }
        print("BIOSEQ_COMPILER_WATCHDOG_TIMEOUT " +
              " ".join(f"{k}={v}" for k, v in fields.items()), file=sys.stderr)
        return 124
    finally:
        if child.poll() is None or group_alive(child.pid):
            stop_group(child, grace_sec)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
