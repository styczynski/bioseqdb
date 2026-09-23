#!/usr/bin/env python3
"""Low-overhead monitor for the local and AWS E2E benchmark fleet.

This script is intentionally observation-only.  It polls the exact MCP state
files already used by receipt-pinned benchmark runs, records state changes, and
never starts, cancels, rebuilds, or modifies a PostgreSQL instance.  That keeps
it safe to leave running while q1024 workers execute.

Run once::

    python3 bench/monitor_e2e_fleet.py --once

Run persistently (default 10-minute interval)::

    nohup python3 bench/monitor_e2e_fleet.py --interval-seconds 600 \\
      > /tmp/bioseq-e2e-fleet-monitor.stdout 2>&1 &
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


REPOSITORY = Path(__file__).resolve().parent.parent
STATE_PATH = "/tmp/bioseq_build_mcp_state.json"
OUTPUT_DIR = REPOSITORY / "reporting" / "bioseq_benchmark_e2e"
SNAPSHOT_PATH = OUTPUT_DIR / "fleet_monitor_snapshot.json"
EVENT_LOG_PATH = OUTPUT_DIR / "fleet_monitor_events.jsonl"
DEFAULT_KEY = "/tmp/bioseq668-remote-recovery-ed25519"
OTHER_KEY = "infra/testing_infra/.secrets/bioseq-bench-aws.pem"

# Explicit identities avoid reliance on stale Terraform state.  The monitor is
# intentionally limited to the fleet the user asked to keep occupied.
BASE_HOSTS = (
    ("default-c7g", "34.239.157.97", DEFAULT_KEY, "i-0cab5f0ab9a1cb002"),
    ("default-m4", "3.238.196.238", DEFAULT_KEY, "i-09b8f0b9dc8c60564"),
    ("default-m7a", "3.239.90.172", DEFAULT_KEY, "i-0c724c456e6a87366"),
    ("default-m7i", "98.82.140.62", DEFAULT_KEY, "i-0e647495fa0691f94"),
    ("test3-c7g", "3.237.1.216", OTHER_KEY, None),
    ("test3-m4", "98.92.19.200", OTHER_KEY, None),
    ("test3-m7a", "100.28.211.184", OTHER_KEY, None),
    ("test3-m7i", "3.236.247.55", OTHER_KEY, None),
    ("test4-c7g", "34.204.194.144", OTHER_KEY, None),
    ("test4-m4", "34.236.134.98", OTHER_KEY, None),
    ("test4-m7a", "3.229.137.70", OTHER_KEY, None),
    ("test4-m7i", "44.197.203.175", OTHER_KEY, None),
)


def registered_hosts(*, include_retired: bool = False) -> tuple:
    path = REPOSITORY / "bench/e2e_fleet_replicas.json"
    registry = json.loads(path.read_bytes()) if path.exists() else {"hosts": {}}
    retirement = REPOSITORY / "bench/e2e_fleet_retirement.json"
    retired = json.loads(retirement.read_bytes())["hosts"] if retirement.exists() else {}
    hosts = BASE_HOSTS + tuple((name, row["address"], OTHER_KEY, None)
                              for name, row in sorted(registry["hosts"].items()))
    return tuple(host for host in hosts if include_retired or not retired.get(host[0], {}).get("retired"))


HOSTS = registered_hosts()


def _run(command: list[str], *, timeout: int = 20) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=timeout, check=False, cwd=REPOSITORY)


def _state_projection(document: dict[str, Any]) -> dict[str, Any]:
    if "benchmark-e2e" not in document:
        return {"status": "missing"}
    entry = document.get("benchmark-e2e", {})
    if not isinstance(entry, dict):
        return {"status": "missing", "error": "benchmark-e2e entry missing"}
    config = entry.get("bench_e2e_config")
    if isinstance(config, str) and config.startswith("/home/ubuntu/bioseqdb/"):
        config = config.removeprefix("/home/ubuntu/bioseqdb/")
    return {
        "status": entry.get("status"),
        "phase": entry.get("active_phase"),
        "pid": entry.get("active_pid"),
        "run_receipt_id": entry.get("run_receipt_id"),
        "build_receipt_id": (
            entry.get("reuse_build_receipt")
            or entry.get("reused_build_receipt")
            or entry.get("build_receipt_id")
            or (entry.get("pending_build_receipt_id")
                if entry.get("status") == "running" else None)
        ),
        "build_only": entry.get("build_only"),
        "cleanup_unverified": entry.get("cleanup_unverified"),
        "template_profile": document.get("_fleet_template_profile"),
        "bench_e2e_config": config,
        "report_files": entry.get("report_files", []),
        "exit_code": entry.get("exit_code"),
        "notes": entry.get("notes"),
        "log_file": entry.get("log_file"),
        "q1024_work_deadline": entry.get("q1024_work_deadline"),
    }


def _local_state() -> dict[str, Any]:
    try:
        document = json.loads(Path(STATE_PATH).read_text(encoding="utf-8"))
        entry = document.get("benchmark-e2e", {})
        cache = Path(str(entry.get("build_dir", ""))) / "CMakeCache.txt"
        if cache.is_file():
            document["_fleet_template_profile"] = next((line.split("=", 1)[1]
                for line in cache.read_text().splitlines()
                if line.startswith("BIOSEQ_TEMPLATE_FLATTEN_PROFILE:")), None)
        ninja = cache.with_name("build.ninja")
        if not document.get("_fleet_template_profile") and ninja.is_file():
            with ninja.open() as source:
                for line in source:
                    match = re.search(r"-DBIOSEQ_TEMPLATE_FLATTEN_PROFILE=([a-z0-9_]+)", line)
                    if match:
                        document["_fleet_template_profile"] = match.group(1)
                        break
        return _state_projection(document)
    except (OSError, json.JSONDecodeError) as exc:
        return {"status": "unreadable", "error": str(exc)}


def _refresh_default_key(instance_id: str) -> str | None:
    public = f"{DEFAULT_KEY}.pub"
    result = _run([
        "aws", "ec2-instance-connect", "send-ssh-public-key",
        "--instance-id", instance_id, "--availability-zone", "us-east-1f",
        "--instance-os-user", "ubuntu", "--ssh-public-key", f"file://{public}",
        "--region", "us-east-1",
    ])
    return None if result.returncode == 0 else result.stderr.strip() or result.stdout.strip()


def _remote_state(name: str, host: str, key: str, instance_id: str | None) -> dict[str, Any]:
    if instance_id is not None:
        key_error = _refresh_default_key(instance_id)
        if key_error is not None:
            return {"status": "unreachable", "error": f"EIC key refresh: {key_error}"}
    remote_python = (
        "import json,os,pathlib,re; p='/tmp/bioseq_build_mcp_state.json'; "
        "d=json.load(open(p)) if os.path.exists(p) else {}; "
        "e=d.get('benchmark-e2e',{}); c=pathlib.Path(str(e.get('build_dir','')))/'CMakeCache.txt'; "
        "d['_fleet_template_profile']=next((s.split('=',1)[1] for s in c.read_text().splitlines() "
        "if s.startswith('BIOSEQ_TEMPLATE_FLATTEN_PROFILE:')),None) if c.is_file() else None; "
        "\nn=c.with_name('build.ninja')\n"
        "if not d.get('_fleet_template_profile') and n.is_file():\n"
        " with n.open() as source:\n"
        "  for line in source:\n"
        "   match=re.search(r'-DBIOSEQ_TEMPLATE_FLATTEN_PROFILE=([a-z0-9_]+)',line)\n"
        "   if match:\n"
        "    d['_fleet_template_profile']=match.group(1)\n"
        "    break\n"
        "print(json.dumps(d))"
    )
    remote_command = f"python3 -c {shlex.quote(remote_python)}"
    result = _run([
        "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
        "-o", f"HostKeyAlias={name}", "-o", "StrictHostKeyChecking=no",
        "-i", key, f"ubuntu@{host}", remote_command,
    ])
    if result.returncode != 0:
        return {"status": "unreachable", "error": result.stderr.strip() or result.stdout.strip()}
    try:
        return _state_projection(json.loads(result.stdout))
    except json.JSONDecodeError as exc:
        return {"status": "unreadable", "error": f"invalid remote JSON: {exc}"}


def _write_snapshot(snapshot: dict[str, Any], previous: dict[str, Any] | None) -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    changed: dict[str, Any] = {}
    old_hosts = (previous or {}).get("hosts", {}) if isinstance(previous, dict) else {}
    for name, current in snapshot["hosts"].items():
        old = old_hosts.get(name)
        if old != current:
            changed[name] = {"before": old, "after": current}
    temporary = SNAPSHOT_PATH.with_suffix(".tmp")
    temporary.write_text(json.dumps(snapshot, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, SNAPSHOT_PATH)
    if changed:
        event = {"timestamp_utc": snapshot["timestamp_utc"], "transitions": changed}
        with EVENT_LOG_PATH.open("a", encoding="utf-8") as log:
            log.write(json.dumps(event, sort_keys=True) + "\n")
        print(json.dumps(event, sort_keys=True), flush=True)


def _previous_snapshot() -> dict[str, Any] | None:
    try:
        return json.loads(SNAPSHOT_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def _poll_once() -> None:
    hosts = {"local": _local_state()}
    for name, host, key, instance_id in registered_hosts():
        hosts[name] = _remote_state(name, host, key, instance_id)
    snapshot = {
        "schema": "bioseq.e2e.fleet-monitor",
        "version": 1,
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "hosts": hosts,
    }
    _write_snapshot(snapshot, _previous_snapshot())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--interval-seconds", type=int, default=600)
    args = parser.parse_args()
    if args.interval_seconds < 60:
        parser.error("--interval-seconds must be at least 60")
    while True:
        _poll_once()
        if args.once:
            return 0
        time.sleep(args.interval_seconds)


if __name__ == "__main__":
    raise SystemExit(main())
