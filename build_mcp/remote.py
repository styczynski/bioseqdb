"""Remote build and inspection operations."""

from __future__ import annotations

from typing import List, Dict, Set, Optional, Any, Mapping

import argparse
import base64
import contextlib
import ctypes
import errno
import fcntl
import hashlib
import json
import math
import os
import platform as host_platform
import re
import select
import secrets
import shlex
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ElementTree
from dataclasses import dataclass, field
from pathlib import Path

from build_presets import (
    BENCH_CLI_MODES,
    CATCH_FILTER_MODES,
    REMOTE_RUN_REJECTED_MODES,
    REPOSITORY_ROOT,
    VALID_MODES,
    _default_debug_preset,
    _default_test_preset,
    _filter_to_cli_args,
    _unit_test_filter_to_cli_args,
    supports_bench_cli_options,
)
from process_state import (
    LOG_DIR,
    REMOTE_STATE_FILE,
    _is_pid_alive,
    _log,
    _stream_subprocess,
    _tail_path,
)
from publication import (
    durable_replace_bytes,
)





INFRA_DIR = REPOSITORY_ROOT / "infra"

PROVISIONER = INFRA_DIR / "provisioner.py"

REMOTE_REPO_PATH = "~/bioseqdb"

REMOTE_BUILD_PRESET = "aws-x64-linux"

REMOTE_BUILD_PRESETS = {
    "aws-x64-linux",
    "aws-x64-linux-gcc161",
}


REMOTE_LOCK_FILE = Path("/tmp/bioseq_remote_mcp.lock")

REMOTE_GOOD_STATUSES = {"TOOLCHAIN_READY", "VERIFY_OK_FAILS_ON_OUR_CODE",
                        "VERIFY_BUILD_SUCCEEDED"}

REMOTE_OP_TIMEOUTS = {
    "status":  6 * 60,
    "output":  6 * 60,
    "logs":    6 * 60,
    "git":    20 * 60,
    "ssh":    90 * 60,     # remote-ssh is synchronous; tolerate saturated compiles
    "up":    140 * 60,
    "down":  120 * 60,     # includes the automatic pre-destroy AMI snapshot
    "snapshot": 120 * 60,
    "verify": 90 * 60,
    "wait":  140 * 60,
    "run":   200 * 60,     # provision (if needed) + git sync + full build/bench
}

REMOTE_OP_GRACE = {
    "status": 30.0, "output": 30.0, "logs": 30.0, "git": 30.0,
    "up": 6.0, "down": 8.0, "snapshot": 8.0,
    "verify": 8.0, "wait": 8.0, "run": 6.0,
}

REMOTE_TERMINAL_STATUSES = {"completed", "failed", "killed_timeout", "aborted"}

REMOTE_MANAGE_OPS = {"up", "down", "status", "logs", "verify", "wait", "output"}

REMOTE_LOG_INLINE_CAP = 6000

REMOTE_DEBUG_INLINE_CAP = 14000


def _parse_remote_bool(parameter_description: str, value: object) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        normalized = value.lower()
        if normalized in {"true", "y"}:
            return True
        if normalized in {"false", "n"}:
            return False
    raise RuntimeError(
        f"Value passed as {parameter_description} is not a valid bool: "
        f"{value!r}")

@contextlib.contextmanager
def _remote_global_lock():
    """Cross-process advisory lock around the single remote-operation slot.

    Guards the read-check-spawn-write critical section so two sessions cannot both
    decide the slot is free and each launch an operation (e.g. each provision a box).
    """
    REMOTE_LOCK_FILE.touch()
    fd = os.open(str(REMOTE_LOCK_FILE), os.O_RDWR)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        yield
    finally:
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        finally:
            os.close(fd)

def _load_remote_state() -> dict:
    if not REMOTE_STATE_FILE.exists():
        return {}
    try:
        with REMOTE_STATE_FILE.open("r") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}

def _save_remote_state(state: dict) -> None:
    durable_replace_bytes(
        REMOTE_STATE_FILE,
        (json.dumps(state, indent=2, sort_keys=True) + "\n").encode("utf-8"),
    )

def _update_remote_entry(**fields) -> None:
    state = _load_remote_state()
    state.update(fields)
    _save_remote_state(state)

def _refresh_remote(state: dict) -> dict:
    """Promote a 'running' slot whose worker pid is dead to 'aborted' so the
    mutex frees itself if a worker is SIGKILLed / the machine reboots."""
    if state.get("status") == "running" and not _is_pid_alive(state.get("pid")):
        state = dict(state)
        state["status"] = "aborted"
        state["step"] = "aborted"
        if not state.get("completed_at"):
            state["completed_at"] = time.time()
        _save_remote_state(state)
    return state

def _provisioner_cmd(*provisioner_args: str) -> list[str]:
    """argv to invoke provisioner.py with the current interpreter."""
    return [sys.executable, str(PROVISIONER), *provisioner_args]

def _remote_ssh_argv(remote_command: str) -> list[str]:
    """provisioner.py path that runs a NON-interactive command on the box.

    CRITICAL: the remote command must be ONE argv element. provisioner's `ssh`
    subcommand forwards its REMAINDER args to ssh after `ubuntu@<ip>`, and ssh
    space-JOINS its trailing args into a single string for the remote shell — so
    passing ('bash', '-lc', script) as three args lets the remote shell re-split
    `script` and lose its grouping (a `cd x && y` would run `y` back in $HOME).
    We instead hand ssh a single string, `bash -lc '<script>'` (script shell-quoted),
    so the remote runs it under a LOGIN shell — which sources /etc/profile.d/bioseq.sh
    so the cmake/ninja/vcpkg/PG toolchain env is on PATH — with the script intact.
    ssh_base() requests no tty, so output streams back cleanly to the caller.
    Workhorse for remote-ssh, remote-git, the logs/verify manage ops, and every
    box-side step of remote-run. (cmd_ssh/cmd_logs themselves os.execvp an
    interactive `ssh -t`, which we avoid here.)"""
    # ssh.service itself is intentionally reniced by bootstrap so fresh SSH
    # sessions stay reachable under memory-heavy compiles. Commands launched
    # through those sessions should not inherit the same elevated priority,
    # otherwise a remote build started over SSH competes with sshd at equal nice.
    normal_priority_command = (
        "renice -n 0 -p $$ >/dev/null 2>&1 || true; "
        "ionice -c 2 -n 4 -p $$ >/dev/null 2>&1 || true; "
        + remote_command
    )
    return _provisioner_cmd("ssh",
                            "bash -lc " + shlex.quote(normal_priority_command))

def _remote_exec(cmd: list[str], log_handle, deadline: float) -> int:
    """Run cmd inside the infra dir, streaming combined output to the worker log.
    Returns the exit code; raises TimeoutError past the deadline."""
    return _stream_subprocess(cmd, log_handle, deadline,
                              cwd=INFRA_DIR, env=os.environ.copy())

def _provisioner_outputs() -> dict:
    """provisioner.py output (Terraform outputs) as a dict, or {} if none.

    provisioner prints a dim `$ tofu output -json` line before the JSON body, so
    we parse from the first '{'."""
    try:
        res = subprocess.run(_provisioner_cmd("output"), cwd=str(INFRA_DIR),
                             text=True, capture_output=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        return {}
    out = (res.stdout or "")
    idx = out.find("{")
    if idx < 0:
        return {}
    try:
        data = json.loads(out[idx:])
        return data if isinstance(data, dict) else {}
    except json.JSONDecodeError:
        return {}

def _provisioner_region() -> str | None:
    return (_provisioner_outputs() or {}).get("region")

def _capture_cmd(argv: list[str], *, cwd: Path | None = None,
                 timeout: int = 20) -> tuple[int | None, str]:
    try:
        res = subprocess.run(
            argv, cwd=str(cwd) if cwd else None, text=True,
            capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, f"timed out after {timeout}s: {' '.join(argv)}"
    except (OSError, subprocess.SubprocessError) as e:
        return None, f"failed to launch {' '.join(argv)}: {e}"
    body = res.stdout or ""
    if (res.stderr or "").strip():
        body += ("\n----- stderr -----\n" + res.stderr)
    return res.returncode, body.strip() or "(no output)"

def _format_cmd_result(title: str, argv: list[str], *, cwd: Path | None = None,
                       timeout: int = 20, cap: int = REMOTE_DEBUG_INLINE_CAP) -> str:
    rc, body = _capture_cmd(argv, cwd=cwd, timeout=timeout)
    if len(body) > cap:
        body = "…(truncated)…\n" + body[-cap:]
    return f"## {title}\nexit={rc}\n```\n{body}\n```"


def _json_dump_limited(obj: Any, cap: int = REMOTE_DEBUG_INLINE_CAP) -> str:
    text = json.dumps(obj, indent=2, sort_keys=True, default=str)
    if len(text) > cap:
        text = text[:cap] + "\n…(truncated)…"
    return text

def _aws_json(args: list[str], *, region: str | None,
              timeout: int = 20) -> tuple[int | None, Any | str]:
    argv = ["aws", *args]
    if region:
        argv += ["--region", region]
    argv += ["--output", "json"]
    rc, body = _capture_cmd(argv, timeout=timeout)
    if rc != 0:
        return rc, body
    try:
        return rc, json.loads(body)
    except json.JSONDecodeError:
        return rc, body

def _summarize_instance(data: Any) -> dict:
    if not isinstance(data, dict):
        return {"raw": data}
    reservations = data.get("Reservations") or []
    instances = []
    for res in reservations:
        instances.extend(res.get("Instances") or [])
    if not instances:
        return {"instances": []}
    inst = instances[0]
    return {
        "instance_id": inst.get("InstanceId"),
        "state": (inst.get("State") or {}).get("Name"),
        "instance_type": inst.get("InstanceType"),
        "launch_time": inst.get("LaunchTime"),
        "public_ip": inst.get("PublicIpAddress"),
        "private_ip": inst.get("PrivateIpAddress"),
        "subnet_id": inst.get("SubnetId"),
        "vpc_id": inst.get("VpcId"),
        "cpu_options": inst.get("CpuOptions"),
        "root_device_name": inst.get("RootDeviceName"),
        "volumes": [
            {
                "device": b.get("DeviceName"),
                "volume_id": ((b.get("Ebs") or {}).get("VolumeId")),
                "delete_on_termination": ((b.get("Ebs") or {}).get("DeleteOnTermination")),
            }
            for b in inst.get("BlockDeviceMappings", [])
        ],
    }

def _summarize_instance_status(data: Any) -> Any:
    if not isinstance(data, dict):
        return data
    statuses = data.get("InstanceStatuses") or []
    if not statuses:
        return {"instance_statuses": []}
    st = statuses[0]
    return {
        "instance_id": st.get("InstanceId"),
        "state": (st.get("InstanceState") or {}).get("Name"),
        "system_status": st.get("SystemStatus"),
        "instance_status": st.get("InstanceStatus"),
        "events": st.get("Events"),
    }

def _summarize_cpu_datapoints(data: Any) -> Any:
    if not isinstance(data, dict):
        return data
    points = data.get("Datapoints") or []
    points = sorted(points, key=lambda p: str(p.get("Timestamp")))[-20:]
    return [
        {
            "timestamp": p.get("Timestamp"),
            "avg": p.get("Average"),
            "max": p.get("Maximum"),
            "unit": p.get("Unit"),
        }
        for p in points
    ]

def _summarize_ssm_info(data: Any) -> Any:
    if not isinstance(data, dict):
        return data
    infos = data.get("InstanceInformationList") or []
    if not infos:
        return {"instance_information": []}
    info = infos[0]
    return {
        "instance_id": info.get("InstanceId"),
        "ping_status": info.get("PingStatus"),
        "last_ping": info.get("LastPingDateTime"),
        "agent_version": info.get("AgentVersion"),
        "is_latest_version": info.get("IsLatestVersion"),
        "platform": info.get("PlatformName"),
        "platform_version": info.get("PlatformVersion"),
        "private_ip": info.get("IPAddress"),
        "computer_name": info.get("ComputerName"),
    }

def _ssm_read_only_probe(instance_id: str, region: str | None,
                         timeout: int) -> tuple[int | None, Any | str]:
    commands = [
        "echo now=$(date -u '+%Y-%m-%dT%H:%M:%SZ')",
        "cat /opt/bioseq/PROVISION_STATUS 2>/dev/null || true",
        "git -C ~/bioseqdb rev-parse --short HEAD 2>/dev/null || true",
        ("ps -eo pid,ppid,stat,pcpu,pmem,etime,args "
         "| grep -E 'dispatch_processor\\.cppm|cmake --build .*/aws-x64-linux|"
         "ninja .*postgresql_bioseq|clang\\+\\+|ld\\.lld' | grep -v grep | tail -40"),
        "free -h | sed -n '1,2p'",
        "df -h / /home /opt 2>/dev/null || true",
    ]
    rc, data = _aws_json(
        ["ssm", "send-command",
         "--instance-ids", instance_id,
         "--document-name", "AWS-RunShellScript",
         "--comment", "bioseq-build remote-debug read-only process probe",
         "--parameters", json.dumps({"commands": commands}),
         "--timeout-seconds", str(timeout)],
        region=region,
        timeout=timeout)
    if rc != 0 or not isinstance(data, dict):
        return rc, data
    command_id = ((data.get("Command") or {}).get("CommandId"))
    if not command_id:
        return rc, data

    deadline = time.time() + timeout
    last: Any | str = data
    while time.time() < deadline:
        time.sleep(2)
        rc, last = _aws_json(
            ["ssm", "get-command-invocation",
             "--command-id", command_id,
             "--instance-id", instance_id],
            region=region,
            timeout=max(5, min(20, int(deadline - time.time()) or 5)))
        if rc == 0 and isinstance(last, dict):
            status = last.get("Status")
            if status not in {"Pending", "InProgress", "Delayed"}:
                return rc, {
                    "command_id": command_id,
                    "status": status,
                    "response_code": last.get("ResponseCode"),
                    "stdout": last.get("StandardOutputContent"),
                    "stderr": last.get("StandardErrorContent"),
                }
    return None, {"command_id": command_id, "status": "timed_out_waiting"}

def _current_local_branch() -> str | None:
    """The checked-out branch of the local repo, used as the default branch to
    clone/sync onto the box (so the box builds the same branch you are on)."""
    try:
        res = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"],
                             cwd=str(REPOSITORY_ROOT), text=True,
                             capture_output=True, timeout=10)
        b = (res.stdout or "").strip()
        return b if b and b != "HEAD" else None
    except (OSError, subprocess.SubprocessError):
        return None

def _remote_provision_status(deadline: float) -> str | None:
    """ssh `cat /opt/bioseq/PROVISION_STATUS`, bounded by the deadline."""
    timeout = max(5, min(60, int(deadline - time.time())))
    try:
        res = subprocess.run(
            _remote_ssh_argv("cat /opt/bioseq/PROVISION_STATUS 2>/dev/null"),
            cwd=str(INFRA_DIR), text=True, capture_output=True, timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        return None
    lines = [ln for ln in (res.stdout or "").splitlines() if ln.strip()]
    return lines[-1] if lines else None

def _ensure_remote_box(branch: str, auto_provision: bool, from_scratch: bool,
                       log_handle, deadline: float) -> None:
    """Make sure a build-ready box exists. Provision + wait if there is none
    (and auto_provision allows it). Raises on failure."""
    outs = _provisioner_outputs()
    ip = outs.get("public_ip")
    if ip:
        st = _remote_provision_status(deadline)
        log_handle.write(f"# existing box {outs.get('instance_id')} @ {ip} "
                         f"(PROVISION_STATUS={st or 'unknown'})\n")
        if from_scratch:
            log_handle.write(
                "# from_scratch=true was requested, but a box already exists; "
                "remote-run will not destroy it implicitly. Tear down only after "
                "explicit user confirmation if a brand-new base-Ubuntu box is required.\n")
        log_handle.flush()
        if st in REMOTE_GOOD_STATUSES:
            return
        log_handle.write("# box present but not yet at a ready status — waiting "
                         "for on-box provisioning to reach a terminal status...\n")
        log_handle.flush()
        _remote_exec(_provisioner_cmd("wait", "--timeout", "120"),
                     log_handle, deadline)
        return  # best-effort; the build step will surface any real problem

    if not auto_provision:
        raise RuntimeError(
            "no remote box is running and auto_provision=false — start one with "
            "remote-manage {operation:'up'} first, or pass auto_provision=true")

    log_handle.write(f"# no box found — provisioning a new one (branch={branch})...\n")
    log_handle.flush()
    argv = _provisioner_cmd("up", "--branch", branch, "--wait", "--timeout", "120")
    if from_scratch:
        argv.append("--from-scratch")
    rc = _remote_exec(argv, log_handle, deadline)
    if rc != 0:
        raise RuntimeError(f"provisioner up failed (exit {rc}) — see log")
    if not _provisioner_outputs().get("public_ip"):
        raise RuntimeError("provisioner up finished but no public_ip in outputs")

def _drive_remote_run(payload: dict, log_handle, deadline: float) -> int:
    """The remote-run sequence: ensure box → optional git sync → run the build
    or inspect on the box by invoking the box-side server.py over SSH. Returns the
    box-side command's exit code (0 ok / 1 build fails as expected on ::NEON / 124
    timeout)."""
    operation = payload.get("operation")
    branch = payload.get("branch") or "main"
    do_sync = bool(payload.get("sync", True))
    auto_provision = bool(payload.get("auto_provision", True))
    from_scratch = bool(payload.get("from_scratch", False))

    _ensure_remote_box(branch, auto_provision, from_scratch, log_handle, deadline)

    if do_sync:
        log_handle.write(f"\n# syncing box repo {REMOTE_REPO_PATH} to origin/{branch} "
                         f"(builds the latest PUSHED commit on that branch)\n")
        log_handle.flush()
        # -f force: the box is disposable and we build the PUSHED commit, so
        # discard any working-tree drift (a half-built tree, a manually-staged
        # file, etc.) rather than letting `checkout` refuse. Use sync=false to
        # preserve a hand-arranged box instead.
        sync_script = (
            f"set -euo pipefail; cd {REMOTE_REPO_PATH} && "
            f"git fetch origin {shlex.quote(branch)} && "
            f"git checkout -f -B {shlex.quote(branch)} origin/{shlex.quote(branch)} && "
            f"git --no-pager log -1 --oneline"
        )
        rc = _remote_exec(_remote_ssh_argv(sync_script), log_handle, deadline)
        if rc != 0:
            raise RuntimeError(
                f"git sync of box to origin/{branch} failed (exit {rc}). Is the "
                f"branch pushed to origin? Use remote-git / remote-ssh to inspect.")

    active_preset = payload.get("preset", REMOTE_BUILD_PRESET)
    remote_debug_preset = _default_debug_preset(active_preset)
    remote_test_preset = _default_test_preset(active_preset)
    remote_test_debug_preset = _default_test_preset(remote_debug_preset)
    env_prefix = (
        f"BIOSEQ_BUILD_PRESET={shlex.quote(active_preset)} "
        "BIOSEQ_BUILD_PRESET_DEBUG="
        f"{shlex.quote(remote_debug_preset)} "
        "BIOSEQ_TEST_BUILD_PRESET="
        f"{shlex.quote(remote_test_preset)} "
        "BIOSEQ_TEST_BUILD_PRESET_DEBUG="
        f"{shlex.quote(remote_test_debug_preset)} "
    )
    server_dir = f"{REMOTE_REPO_PATH}/build_mcp"
    if operation == "build":
        mode = payload["mode"]
        build_only = bool(payload.get("build_only", False))
        debug = bool(payload.get("debug", False))
        bench_flags = payload.get("bench_flags") or []   # parity with local path
        test_flags = payload.get("focused_test_args") or []
        bench_e2e_config = payload.get("bench_e2e_config") or ""
        remote_cmd = (
            f"cd {server_dir} && {env_prefix}python3 server.py "
            f"--run-build-sync {shlex.quote(mode)} "
            f"--build-only {'true' if build_only else 'false'} "
            f"--debug {'true' if debug else 'false'} "
            f"--bench-flags {shlex.quote(json.dumps(bench_flags))} "
            f"--test-flags {shlex.quote(json.dumps(test_flags))} "
            f"--benchmark-e2e-config {shlex.quote(bench_e2e_config)}"
        )
    elif operation == "inspect":
        inspect_args = payload.get("inspect_args") or {}
        remote_cmd = (
            f"cd {server_dir} && {env_prefix}python3 server.py "
            f"--run-inspect {shlex.quote(json.dumps(inspect_args))}"
        )
    else:
        raise RuntimeError(f"unknown remote-run operation '{operation}'")

    log_handle.write(f"\n# running on box: {remote_cmd}\n")
    log_handle.flush()
    return _remote_exec(_remote_ssh_argv(remote_cmd), log_handle, deadline)

def _classify_remote_run(payload: dict, rc: int, log_path: Path) -> str | None:
    """Turn a remote build's exit code + log into a human verdict, mirroring
    verify.sh's classification (3rd-party vs our-code vs the expected ::NEON gap)."""
    if payload.get("operation") != "build":
        return None
    try:
        text = log_path.read_text(errors="replace")
    except OSError:
        text = ""
    if rc == 0:
        return ("build_succeeded — the remote build completed with no errors. The "
                "::NEON / dispatch.h x86_64 gap appears to be closed on this branch.")
    if "selected Vulkan execution requires a hardware GPU" in text:
        return ("expected_vulkan_software_device_rejection — selected Vulkan "
                "execution refused the active software/CPU Vulkan ICD instead of "
                "silently falling back to CPU. This is the expected policy on a "
                "box without a trusted hardware Vulkan device.")
    err_lines = [ln for ln in text.splitlines()
                 if " error: " in ln or " fatal error: " in ln]
    our_source_marker = "/bioseqdb/bioseq/"
    our = [ln for ln in err_lines if our_source_marker in ln]
    third = [ln for ln in err_lines
             if any(s in ln for s in ("vcpkg_installed", "/opt/vcpkg", "/vendor/",
                                      "/usr/lib/llvm", "/usr/include/", "/opt/llvm"))
             and our_source_marker not in ln]
    neon = ("NEON" in text) or ("dispatch/dispatch.h" in text)
    if third:
        return (f"UNEXPECTED third-party/toolchain failure — {len(third)} error "
                f"line(s) outside our code. This is NOT the expected gap; "
                f"investigate. First: {third[0].strip()[:200]}")
    if our:
        tag = "the expected ::NEON / macOS-ism gap" if neon else "our own code"
        return (f"expected_our_code_failure — configure succeeded and every "
                f"third-party dependency compiled; the only errors are in {tag} "
                f"({len(our)} line(s) under /bioseqdb/bioseq/). This is exactly the "
                f"VERIFY_OK_FAILS_ON_OUR_CODE outcome: the remote build pipeline is "
                f"healthy and the failure is the known x86_64 SIMD port gap. "
                f"First: {our[0].strip()[:200]}")
    return ("failed_unclassified — non-zero exit but no 'error:' lines matched by "
            "path. Read the full log to see what happened.")

def _run_remote_worker(op_id: str) -> int:
    """Detached worker entry point (invoked via `--run-remote <op_id>`). Waits for
    the slot it owns (matched by op_id, to ignore any stale prior state), runs the
    recorded command/payload streaming to the log, and records the verdict."""
    state = {}
    for _ in range(100):  # ~10s — tolerate the benign spawn→write race
        state = _load_remote_state()
        if (
            state.get("op_id") == op_id
            and state.get("status") == "running"
            and state.get("pid") == os.getpid()
        ):
            break
        time.sleep(0.1)
    else:
        _log(f"remote worker {op_id}: state slot never became ready, exiting")
        return 1

    kind = state.get("kind", "")
    started_at = float(state.get("started_at", time.time()))
    log_path = Path(state.get("log_file",
                              str(LOG_DIR / f"bioseq_remote_mcp_{op_id}.log")))
    timeout_sec = float(state.get("timeout_sec",
                                  REMOTE_OP_TIMEOUTS.get(kind, 60 * 60)))
    deadline = started_at + timeout_sec

    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_handle = log_path.open("w", buffering=1)
    log_handle.write(
        f"# bioseq remote worker — kind={kind} op_id={op_id} pid={os.getpid()} "
        f"started={time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(started_at))}\n"
        f"# label={state.get('label')}\n"
        f"# timeout_sec={timeout_sec}\n"
    )
    log_handle.flush()

    classification = None
    try:
        _update_remote_entry(step="running")
        if kind == "run":
            payload = state.get("payload") or {}
            rc = _drive_remote_run(payload, log_handle, deadline)
            classification = _classify_remote_run(payload, rc, log_path)
        else:
            argv = state.get("argv") or []
            if not argv:
                raise RuntimeError(f"remote worker kind={kind} has no argv")
            rc = _remote_exec(argv, log_handle, deadline)

        completed_at = time.time()
        _update_remote_entry(
            status="completed" if rc == 0 else "failed",
            step="completed" if rc == 0 else "failed",
            completed_at=completed_at, exit_code=rc,
            classification=classification,
            notes=None if rc == 0 else f"exited with code {rc}",
        )
        log_handle.write(f"\n# remote worker exiting (rc={rc}) at "
                         f"{time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(completed_at))}\n")
        return 0 if rc == 0 else 1

    except TimeoutError as e:
        _update_remote_entry(status="killed_timeout", step="killed_timeout",
                             completed_at=time.time(), exit_code=None,
                             notes=str(e), classification=classification)
        log_handle.write(f"\n# killed by hard timeout: {e}\n")
        return 124
    except Exception as e:
        _update_remote_entry(status="failed", step="failed",
                             completed_at=time.time(), exit_code=None,
                             notes=str(e), classification=classification)
        log_handle.write(f"\n# remote worker failed: {e}\n")
        return 1
    finally:
        log_handle.close()

def _remote_elapsed(started_at, completed_at=None) -> str:
    end = float(completed_at) if completed_at else time.time()
    return f"{(end - float(started_at)) / 60.0:.1f} min"

def _remote_log_tail(log_file: str | None, cap: int = REMOTE_LOG_INLINE_CAP) -> str:
    if not log_file:
        return ""
    try:
        text = Path(log_file).read_text(errors="replace")
    except OSError:
        return ""
    if len(text) <= cap:
        return text
    return "…(truncated — Read the log for the full output)…\n" + text[-cap:]

def _remote_busy_message(state: dict) -> str:
    kind = state.get("kind", "?")
    return (
        "🔒 A remote operation is already in progress — the remote mutex allows "
        "only ONE at a time (the guard that stops stacking paid EC2 instances or "
        "mutating the box mid-build).\n"
        f"  in-flight : {state.get('label', kind)} (kind={kind})\n"
        f"  step      : {state.get('step', 'running')}, "
        f"elapsed {_remote_elapsed(state.get('started_at', time.time()))}, "
        f"pid={state.get('pid')}\n"
        f"  live log  : {state.get('log_file')}\n"
        "It frees automatically when its process exits (or is killed). Re-call any "
        "remote tool to poll, or use remote-ssh (NOT under the mutex) to inspect "
        "the box meanwhile."
    )

def _remote_running_message(state: dict, poll_hint: str) -> str:
    kind = state.get("kind", "?")
    return (
        f"⏳ Remote operation '{state.get('label', kind)}' (kind={kind}) is in "
        f"progress (step={state.get('step', 'running')}, "
        f"elapsed {_remote_elapsed(state.get('started_at', time.time()))}).\n"
        f"It runs detached and survives this session. {poll_hint}\n"
        f"Live log (Read to follow): {state.get('log_file')}"
    )

def _remote_finished_message(state: dict) -> str:
    status = state.get("status")
    kind = state.get("kind", "?")
    label = state.get("label", kind)
    elapsed = _remote_elapsed(state.get("started_at", time.time()),
                              state.get("completed_at"))
    head = {
        "completed": f"✅ Remote operation '{label}' completed in {elapsed} "
                     f"(exit {state.get('exit_code')}).",
        "failed": f"❌ Remote operation '{label}' failed in {elapsed} "
                  f"(exit {state.get('exit_code')}).",
        "killed_timeout": f"⏱️ Remote operation '{label}' hit its hard timeout "
                          f"after {elapsed} and was killed.",
        "aborted": f"⚠️ Remote operation '{label}' aborted after {elapsed} "
                   f"(worker died unexpectedly).",
    }.get(status, f"Remote operation '{label}' status: {status}.")
    parts = [head]
    if state.get("classification"):
        parts.append(f"Verdict: {state['classification']}")
    if state.get("notes"):
        parts.append(f"Notes: {state['notes']}")
    parts.append(f"Full log: {state.get('log_file')}")
    tail = _remote_log_tail(state.get("log_file"))
    if tail:
        parts.append(f"\n----- output -----\n{tail}")
    return "\n".join(parts)

def _remote_grace_wait(grace_sec: float) -> dict:
    """Block up to grace_sec for the running op to reach a terminal status."""
    deadline = time.time() + grace_sec
    while time.time() < deadline:
        st = _refresh_remote(_load_remote_state())
        if st.get("status") in REMOTE_TERMINAL_STATUSES:
            return st
        time.sleep(0.4)
    return _refresh_remote(_load_remote_state())

def _mark_delivered(op_id: str) -> None:
    with _remote_global_lock():
        st = _load_remote_state()
        if st.get("op_id") == op_id and not st.get("delivered"):
            st["delivered"] = True
            _save_remote_state(st)

def _start_or_poll_remote(kind: str, label: str, *,
                          argv: list[str] | None = None,
                          payload: dict | None = None) -> str:
    """The shared entry for every mutex-guarded remote tool. Either:
      - reports the in-flight op (mutex busy), or
      - delivers the previous op's result once and frees the slot, or
      - starts a fresh detached worker and grace-waits for a quick finish.
    """
    timeout_sec = REMOTE_OP_TIMEOUTS.get(kind, 60 * 60)
    grace_sec = REMOTE_OP_GRACE.get(kind, 10.0)
    op_id = f"{kind}-{os.getpid()}-{int(time.time() * 1000)}"

    with _remote_global_lock():
        state = _refresh_remote(_load_remote_state())
        status = state.get("status")
        if status == "running":
            return _remote_busy_message(state)
        if status in REMOTE_TERMINAL_STATUSES and not state.get("delivered"):
            # Deliver the previous op's result exactly once, then free the slot.
            # This makes polling a finished long op safe: re-calling returns the
            # result instead of silently launching a new operation.
            state["delivered"] = True
            _save_remote_state(state)
            return ("(The previous remote operation has finished — returning its "
                    "result; the mutex slot is now free. Call this tool again to "
                    f"start a new '{label}'.)\n\n{_remote_finished_message(state)}")

        started_at = time.time()
        timestamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(started_at))
        log_path = LOG_DIR / f"bioseq_remote_mcp_{kind}_{timestamp}.log"
        server_path = str(Path(__file__).with_name("server.py").resolve())
        reservation = {
            "op_id": op_id, "kind": kind, "label": label,
            "status": "running", "step": "spawning",
            "started_at": started_at, "completed_at": None,
            # Publish the live scheduling process before spawning so aggregate
            # status can never observe an absent remote slot in this window.
            "pid": os.getpid(),
            "log_file": str(log_path), "exit_code": None, "notes": None,
            "classification": None, "delivered": False,
            "timeout_sec": timeout_sec, "argv": argv, "payload": payload,
        }
        _save_remote_state(reservation)
        try:
            proc = subprocess.Popen(
                [sys.executable, server_path, "--run-remote", op_id],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, start_new_session=True,
                close_fds=True)
        except OSError as exc:
            reservation.update({
                "status": "failed",
                "step": "spawn_failed",
                "completed_at": time.time(),
                "notes": str(exc),
            })
            _save_remote_state(reservation)
            raise RuntimeError("could not start detached remote worker") from exc
        reservation["pid"] = proc.pid
        try:
            _save_remote_state(reservation)
        except BaseException as exc:
            # The child cannot begin until it observes this exact PID handoff.
            # Reap it before clearing the scheduler reservation so aggregate
            # activity cannot be wedged on this long-lived server process.
            try:
                proc.terminate()
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
            except OSError:
                pass
            failed_reservation = dict(reservation)
            failed_reservation.update({
                "status": "failed",
                "step": "handoff_failed",
                "completed_at": time.time(),
                "notes": str(exc),
            })
            try:
                _save_remote_state(failed_reservation)
            except BaseException:
                current = _load_remote_state()
                if (
                    current.get("op_id") == op_id
                    and current.get("status") == "running"
                    and current.get("pid") == os.getpid()
                ):
                    try:
                        REMOTE_STATE_FILE.unlink(missing_ok=True)
                    except OSError:
                        pass
            raise RuntimeError(
                "could not publish detached remote worker handoff"
            ) from exc
        _log(f"remote op started: {label} kind={kind} op_id={op_id} "
             f"pid={proc.pid} log={log_path}")

    # Grace-wait OUTSIDE the lock so other sessions aren't blocked during it.
    st = _remote_grace_wait(grace_sec)
    if st.get("op_id") == op_id and st.get("status") in REMOTE_TERMINAL_STATUSES:
        _mark_delivered(op_id)
        return _remote_finished_message(st)
    poll_hint = ("Poll by re-calling any remote tool (a running slot reports "
                 "progress; it will NOT start a second operation).")
    return _remote_running_message(st, poll_hint)

def tool_remote_status() -> str:
    """remote-status — provisioner.py status (Terraform outputs + live EC2 state +
    a running-instance count guardrail + the on-box PROVISION_STATUS marker)."""
    return _start_or_poll_remote(
        "status",
        "provisioner status (AWS box state + on-box PROVISION_STATUS)",
        argv=_provisioner_cmd("status"))

def tool_remote_debug(arguments: dict) -> str:
    """Read-only diagnostics for an active remote operation.

    This intentionally does not use the remote mutex and does not mutate the box.
    It is the fallback when an already-running remote build holds the mutex and
    new SSH sessions are slow or timing out during a saturated compile.
    """
    log_lines = max(1, min(500, int(arguments.get("log_lines", 80))))
    cloudwatch_minutes = max(5, min(360, int(arguments.get("cloudwatch_minutes", 120))))
    include_ssh = _parse_remote_bool("include_ssh", arguments.get("include_ssh", False))
    include_ssm = _parse_remote_bool("include_ssm", arguments.get("include_ssm", False))
    ssh_timeout = max(5, min(120, int(arguments.get("ssh_timeout", 20))))
    ssm_timeout = max(10, min(120, int(arguments.get("ssm_timeout", 30))))

    state = _load_remote_state()
    outputs = _provisioner_outputs()
    instance_id = outputs.get("instance_id")
    region = outputs.get("region") or _provisioner_region()
    now = time.time()

    parts: list[str] = [
        "# Remote Debug",
        "Read-only diagnostic snapshot. This tool does not acquire the remote "
        "mutex, does not start/stop instances, and does not signal build processes.",
        "",
        "## MCP Remote Slot",
        "```json\n" + _json_dump_limited({
            "status": state.get("status"),
            "step": state.get("step"),
            "kind": state.get("kind"),
            "label": state.get("label"),
            "pid": state.get("pid"),
            "worker_alive": _is_pid_alive(state.get("pid")),
            "elapsed": _remote_elapsed(state.get("started_at", now)),
            "timeout_sec": state.get("timeout_sec"),
            "log_file": state.get("log_file"),
            "exit_code": state.get("exit_code"),
            "notes": state.get("notes"),
            "classification": state.get("classification"),
            "payload": state.get("payload"),
        }) + "\n```",
    ]

    pid = state.get("pid")
    if pid:
        parts.append(_format_cmd_result(
            "Local Worker Process",
            ["ps", "-p", str(pid), "-o", "pid,ppid,stat,etime,pcpu,pmem,args"],
            timeout=5))
        parts.append(_format_cmd_result(
            "Local Worker Children",
            ["pgrep", "-P", str(pid), "-a"],
            timeout=5))

    parts += [
        "## Remote Worker Log Tail",
        f"`{state.get('log_file')}` last {log_lines} lines:",
        "```\n" + _tail_path(state.get("log_file"), log_lines) + "\n```",
        "## Terraform Outputs",
        "```json\n" + _json_dump_limited(outputs) + "\n```",
    ]

    if instance_id:
        rc, data = _aws_json(
            ["ec2", "describe-instances", "--instance-ids", instance_id],
            region=region)
        parts.append(
            "## EC2 Instance\n"
            f"exit={rc}\n```json\n{_json_dump_limited(_summarize_instance(data))}\n```")

        rc, data = _aws_json(
            ["ec2", "describe-instance-status", "--include-all-instances",
             "--instance-ids", instance_id],
            region=region)
        parts.append(
            "## EC2 Status Checks\n"
            f"exit={rc}\n```json\n{_json_dump_limited(_summarize_instance_status(data))}\n```")

        end = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now))
        start = time.strftime(
            "%Y-%m-%dT%H:%M:%SZ",
            time.gmtime(now - cloudwatch_minutes * 60))
        rc, data = _aws_json(
            ["cloudwatch", "get-metric-statistics",
             "--namespace", "AWS/EC2",
             "--metric-name", "CPUUtilization",
             "--dimensions", f"Name=InstanceId,Value={instance_id}",
             "--start-time", start,
             "--end-time", end,
             "--period", "60",
             "--statistics", "Average", "Maximum"],
            region=region)
        parts.append(
            f"## CloudWatch CPU ({cloudwatch_minutes} min)\n"
            f"exit={rc}\n```json\n{_json_dump_limited(_summarize_cpu_datapoints(data))}\n```")

        rc, data = _aws_json(
            ["ssm", "describe-instance-information",
             "--filters", f"Key=InstanceIds,Values={instance_id}"],
            region=region)
        ssm_summary = _summarize_ssm_info(data)
        parts.append(
            "## SSM Instance Status\n"
            f"exit={rc}\n```json\n{_json_dump_limited(ssm_summary)}\n```")

        if include_ssm:
            ping_status = (ssm_summary or {}).get("ping_status") if isinstance(ssm_summary, dict) else None
            if ping_status == "Online":
                rc, data = _ssm_read_only_probe(instance_id, region, ssm_timeout)
                parts.append(
                    f"## Optional SSM Probe (timeout {ssm_timeout}s)\n"
                    f"exit={rc}\n```json\n{_json_dump_limited(data)}\n```")
            else:
                parts.append(
                    "## Optional SSM Probe\n"
                    f"Skipped because SSM PingStatus is {ping_status!r}, not 'Online'.")
    else:
        parts.append("## AWS\nNo instance_id found in Terraform outputs.")

    if include_ssh:
        ssh_cmd = (
            "echo now=$(date -u '+%Y-%m-%dT%H:%M:%SZ'); "
            "echo status=$(cat /opt/bioseq/PROVISION_STATUS 2>/dev/null || true); "
            "git -C ~/bioseqdb rev-parse --short HEAD 2>/dev/null || true; "
            "ps -eo pid,ppid,stat,pcpu,pmem,etime,args "
            "| grep -E 'dispatch_processor\\.cppm|cmake --build .*/aws-x64-linux|"
            "ninja .*postgresql_bioseq|clang\\+\\+|ld\\.lld' | grep -v grep | tail -40; "
            "free -h | sed -n '1,2p'; "
            "df -h / /home /opt 2>/dev/null")
        parts.append(_format_cmd_result(
            f"Optional SSH Probe (timeout {ssh_timeout}s)",
            _remote_ssh_argv(ssh_cmd),
            cwd=INFRA_DIR,
            timeout=ssh_timeout,
            cap=REMOTE_DEBUG_INLINE_CAP))

    return "\n\n".join(parts)

REMOTE_STATUS_PS_CMD = r"""
echo "## date"
date
echo
echo "## host/build identity"
echo "host=$(hostname)"
echo "provision_status=$(cat /opt/bioseq/PROVISION_STATUS 2>/dev/null || true)"
echo "repo_commit=$(git -C ~/bioseqdb rev-parse --short HEAD 2>/dev/null || true)"
echo
echo "## load"
uptime || true
echo "nproc=$(nproc 2>/dev/null || true)"
echo
echo "## swap"
swapon --show || true
echo
echo "## memory"
free -h | sed -n '1,3p'
echo
echo "## vmstat"
vmstat 1 3 2>/dev/null || true
echo
echo "## disk"
df -h / ~/bioseqdb/build/aws-x64-linux 2>/dev/null || true
echo
echo "## build processes"
ps -eo pid,ppid,stat,ni,pcpu,pmem,rss,vsz,etime,args --cols 220 \
  | { read -r header; echo "$header"; grep -E '[c]make|[n]inja|[c]lang(\+\+)?|[g]\+\+|[g]cc|[x]gcc|[c]c1(plus)?|[l]d\.lld|[c]ollect2|/opt/[g]cc-build|postgresql_[b]ioseq|[b]ioseq_(unit_tests|integration_tests|benchmark|io_benchmark|benchmark_e2e|cache_(benchmark|bioseq_benchmark|bioseq_large_input_e2e_tests))|dispatch_processor\.cppm' | tail -n 80 || true; } \
  | awk '{ if (length($0) > 220) print substr($0, 1, 217) "..."; else print; }'
echo
echo "## postgres processes"
ps -eo pid,ppid,stat,ni,pcpu,pmem,rss,vsz,etime,args --cols 220 \
  | { read -r header; echo "$header"; grep -E '[p]ostgres(:| -D)' | tail -n 80 || true; } \
  | awk '{ if (length($0) > 220) print substr($0, 1, 217) "..."; else print; }'
echo
echo "## pg_stat_activity"
if command -v psql >/dev/null 2>&1 && id postgres >/dev/null 2>&1; then
  sudo -u postgres psql -XAt -d postgres -c "select coalesce(datname,'') || '|' || pid || '|' || state || '|' || coalesce(wait_event_type,'') || '|' || coalesce(wait_event,'') || '|' || coalesce(extract(epoch from now()-query_start)::int::text,'') || 's|' || left(regexp_replace(coalesce(query,''), E'[\\n\\r\\t]+', ' ', 'g'), 220) from pg_stat_activity order by query_start nulls last limit 30;" || true
else
  echo "(psql or postgres user unavailable)"
fi
echo
echo "## postgres waiting locks"
if command -v psql >/dev/null 2>&1 && id postgres >/dev/null 2>&1; then
  sudo -u postgres psql -XAt -d postgres -c "select pid || '|' || locktype || '|' || mode || '|' || granted || '|' || coalesce(relation::regclass::text,'') from pg_locks where not granted limit 30;" || true
else
  echo "(psql or postgres user unavailable)"
fi
"""

def tool_remote_status_ps(arguments: dict) -> str:
    """remote-status-ps — read-only SSH snapshot of build processes and resources.

    This intentionally does not use the remote mutex. It is a narrower, cheaper
    version of remote-debug for agents that only need the current on-box date,
    swap, memory, disk, and cmake/ninja/clang/linker/bioseq process list.
    """
    timeout = max(5, min(REMOTE_OP_TIMEOUTS["ssh"],
                         int(arguments.get("timeout", 300))))
    try:
        res = subprocess.run(_remote_ssh_argv(REMOTE_STATUS_PS_CMD),
                             cwd=str(INFRA_DIR), text=True,
                             capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return (f"remote-status-ps timed out after {timeout}s. "
                "The box may be too saturated for a fresh SSH session; try "
                "remote-debug without include_ssh for AWS/SSM-side diagnostics.")
    except (OSError, subprocess.SubprocessError) as e:
        return f"remote-status-ps failed to launch: {e}"

    body = res.stdout or ""
    if (res.stderr or "").strip():
        body += "\n----- stderr -----\n" + res.stderr
    if not body.strip():
        body = "(no output)"
    return f"remote-status-ps exit={res.returncode}\n{body}"

def tool_remote_manage(arguments: dict) -> str:
    """remote-manage — thin wrapper over provisioner.py up/down/status/logs/verify/
    wait/output, under the remote mutex."""
    operation = arguments.get("operation")
    if operation not in REMOTE_MANAGE_OPS:
        return (f"Invalid operation '{operation}'. remote-manage operations: "
                f"{', '.join(sorted(REMOTE_MANAGE_OPS))}.")

    if operation == "up":
        argv = _provisioner_cmd("up")
        branch = arguments.get("branch") or _current_local_branch()
        if branch:
            argv += ["--branch", str(branch)]
        # Default region to the existing stack's region so the running-instance
        # guardrail and tofu target the same place (avoids accidentally spinning a
        # second box in another region).
        region = arguments.get("region") or _provisioner_region()
        if region:
            argv += ["--region", str(region)]
        if arguments.get("instance_type"):
            argv += ["--instance-type", str(arguments["instance_type"])]
        if arguments.get("ami"):
            argv += ["--ami", str(arguments["ami"])]
        from_scratch = _parse_remote_bool("from_scratch", arguments.get("from_scratch", False))
        if from_scratch:
            argv += ["--from-scratch"]
        if arguments.get("volume_size"):
            argv += ["--volume-size", str(int(arguments["volume_size"]))]
        if _parse_remote_bool("no_verify", arguments.get("no_verify", False)):
            argv += ["--no-verify"]
        if _parse_remote_bool("wait", arguments.get("wait", True)):
            argv += ["--wait"]
        argv += ["--timeout", str(int(arguments.get("timeout", 120)))]
        if _parse_remote_bool("yes", arguments.get("yes", False)):
            argv += ["--yes"]
        label = (f"provisioner up (branch={branch or 'default'}, "
                 f"from_scratch={from_scratch})")
    elif operation == "down":
        # provisioner down prompts via input() without --yes; the detached worker
        # cannot answer, so always pass --yes.
        argv = _provisioner_cmd("down", "--yes")
        # Pin the destroy to the stack's actual region. provisioner.down recovers
        # this itself (persisted apply-state / outputs), but passing it explicitly
        # keeps MCP and CLI consistent and prevents a region-less destroy from
        # orphaning a paid instance in another region.
        region = arguments.get("region") or _provisioner_region()
        if region:
            argv += ["--region", str(region)]
        label = "provisioner down (snapshot cache, then destroy the box and managed resources)"
    elif operation == "status":
        argv = _provisioner_cmd("status")
        if arguments.get("region"):
            argv += ["--region", str(arguments["region"])]
        label = "provisioner status"
    elif operation == "output":
        argv = _provisioner_cmd("output")
        label = "provisioner output (Terraform outputs JSON)"
    elif operation == "wait":
        argv = _provisioner_cmd("wait", "--timeout",
                                str(int(arguments.get("timeout", 120))))
        label = "provisioner wait (block until on-box provisioning is terminal)"
    elif operation == "logs":
        lines = int(arguments.get("lines", 200))
        argv = _remote_ssh_argv(f"tail -n {lines} /var/log/bioseq-provision.log")
        label = f"provisioner logs (tail -n {lines})"
    else:  # verify
        argv = _remote_ssh_argv(
            f"{REMOTE_REPO_PATH}/infra/testing_infra/scripts/verify.sh")
        label = "provisioner verify (cmake configure + build; classify failures)"

    return _start_or_poll_remote(operation, label, argv=argv)

def tool_remote_snapshot(arguments: dict) -> str:
    """remote-snapshot — create a reusable AMI cache from the current AWS box."""
    argv = _provisioner_cmd("snapshot")
    if arguments.get("region"):
        argv += ["--region", str(arguments["region"])]
    if arguments.get("name"):
        argv += ["--name", str(arguments["name"])]
    if arguments.get("description"):
        argv += ["--description", str(arguments["description"])]
    if arguments.get("timeout"):
        argv += ["--timeout", str(int(arguments["timeout"]))]
    if _parse_remote_bool("no_wait", arguments.get("no_wait", False)):
        argv += ["--no-wait"]
    if _parse_remote_bool("no_reboot", arguments.get("no_reboot", False)):
        argv += ["--no-reboot"]
    label = "provisioner snapshot (create reusable cached AMI from current root volume)"
    return _start_or_poll_remote("snapshot", label, argv=argv)

def tool_remote_ssh(command: str) -> str:
    """remote-ssh — run a command on the box and return its output. NOT under the
    remote mutex: the read-only escape hatch for inspecting the box while a long
    remote-run holds the slot. Synchronous and bounded."""
    if not isinstance(command, str) or not command.strip():
        return ("Error: 'command' must be a non-empty shell command to run on the "
                "box, e.g. 'cat /opt/bioseq/PROVISION_STATUS' or "
                "'git -C ~/bioseqdb log -1 --oneline'.")
    timeout = REMOTE_OP_TIMEOUTS["ssh"]
    try:
        res = subprocess.run(_remote_ssh_argv(command), cwd=str(INFRA_DIR),
                             text=True, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return (f"remote-ssh timed out after {timeout}s running: {command}\n"
                "For long-running work use remote-run / remote-manage instead.")
    except (OSError, subprocess.SubprocessError) as e:
        return f"remote-ssh failed to launch: {e}"
    body = res.stdout or ""
    if (res.stderr or "").strip():
        body += "\n----- stderr -----\n" + res.stderr
    if not body.strip():
        body = "(no output)"
    return f"remote-ssh exit={res.returncode}\n{body}"

def tool_remote_git(command: str) -> str:
    """remote-git — proxy a git subcommand to the box's repo (under the mutex)."""
    if not isinstance(command, str) or not command.strip():
        return ("Error: 'command' must be a git subcommand, e.g. 'pull', "
                "'checkout my-branch', or 'merge origin/x -X theirs'.")
    git_args = command.strip()
    if git_args.startswith("git "):  # tolerate a leading 'git '
        git_args = git_args[4:].strip()
    remote_cmd = f"cd {REMOTE_REPO_PATH} && git {git_args}"
    return _start_or_poll_remote(
        "git", f"git {git_args} (on box {REMOTE_REPO_PATH})",
        argv=_remote_ssh_argv(remote_cmd))

def tool_remote_run(arguments: dict) -> str:
    """remote-run — run a build/test/bench (the start_build modes) or inspect on the
    box, provisioning + waiting + syncing first if needed. Under the mutex."""
    operation = arguments.get("operation")
    branch = arguments.get("branch") or _current_local_branch() or "main"
    sync = _parse_remote_bool("sync", arguments.get("sync", True))
    auto_provision = _parse_remote_bool("auto_provision", arguments.get("auto_provision", True))
    from_scratch = _parse_remote_bool("from_scratch", arguments.get("from_scratch", False))
    preset = arguments.get("preset", REMOTE_BUILD_PRESET)
    if preset not in REMOTE_BUILD_PRESETS:
        return (f"Invalid preset '{preset}'. remote-run presets: "
                f"{', '.join(sorted(REMOTE_BUILD_PRESETS))}.")

    if operation == "build":
        mode = arguments.get("mode")
        if mode not in VALID_MODES:
            return (f"Invalid mode '{mode}'. remote-run build modes: "
                    f"{', '.join(sorted(VALID_MODES))}.")
        if mode in REMOTE_RUN_REJECTED_MODES:
            return (
                f"Mode '{mode}' is local-only and is not supported by "
                f"remote-run. Use local start_build mode={mode} instead.")
        build_only = _parse_remote_bool("build_only", arguments.get("build_only", False))
        debug = _parse_remote_bool("debug", arguments.get("debug", False))
        bench_filter = arguments.get("filter")
        debug_report = _parse_remote_bool("debug_report",
                                  arguments.get("debug_report", False))
        bench_e2e_config = arguments.get("bench_e2e_config")
        if bench_e2e_config is not None:
            if mode != "benchmark-e2e":
                return (
                    "remote bench_e2e_config is accepted only for "
                    "mode='benchmark-e2e'.")
            if (
                not isinstance(bench_e2e_config, str)
                or not bench_e2e_config.strip()
                or bench_e2e_config != bench_e2e_config.strip()
                or Path(bench_e2e_config).is_absolute()
                or ".." in Path(bench_e2e_config).parts
                or Path(bench_e2e_config).suffix.lower() != ".json"
                or any(
                    ord(ch) < 0x20 or ord(ch) == 0x7f
                    for ch in bench_e2e_config
                )
            ):
                return (
                    "remote bench_e2e_config must be a clean "
                    "repository-relative .json path available in the pushed "
                    "branch/on-box checkout.")
        bench_flags: list[str] = []
        test_flags: list[str] = []
        if mode in CATCH_FILTER_MODES:
            if "debug_report" in arguments or debug_report:
                return ("`debug_report` is bench-only and is not accepted for "
                        f"Catch correctness mode {mode!r}.")
            test_flags, ferr = _unit_test_filter_to_cli_args(bench_filter)
            if ferr is not None:
                return f"Invalid Catch correctness filter for {mode!r}: {ferr}"
        else:
            if (bench_filter or debug_report) and not supports_bench_cli_options(mode):
                return ("`filter`/`debug_report` require a benchmark CLI that accepts "
                        "those options (or Catch correctness focused tags); valid benchmark "
                        f"modes are {', '.join(BENCH_CLI_MODES)}; got '{mode}'.")
            filter_args, ferr = _filter_to_cli_args(bench_filter)
            if ferr is not None:
                return f"Invalid filter: {ferr}"
            if debug_report and not filter_args:
                return ("`debug_report` requires a non-empty `filter` — debug is for "
                        "targeted runs, not for instrumenting an entire benchmark sweep.")
            bench_flags = list(filter_args)
            if debug_report:
                bench_flags.append("--debug")
        payload = {"operation": "build", "mode": mode, "build_only": build_only,
                   "debug": debug, "branch": branch, "sync": sync,
                   "auto_provision": auto_provision,
                   "from_scratch": from_scratch,
                   "bench_flags": bench_flags,
                   "focused_test_args": test_flags,
                   "bench_e2e_config": bench_e2e_config,
                   "preset": preset}
        label = (f"remote build mode={mode} build_only={build_only} debug={debug} "
                 f"(branch={branch}, preset={preset}, from_scratch={from_scratch})")
        if bench_flags:
            label += f" bench_flags={' '.join(bench_flags)}"
        if test_flags:
            label += f" focused_test_args={shlex.join(test_flags)}"
    elif operation == "inspect":
        function_name = arguments.get("function_name")
        if not isinstance(function_name, str) or not function_name:
            return "Error: remote-run inspect requires a non-empty 'function_name'."
        inspect_args = {k: arguments[k] for k in
                        ("function_name", "target", "max_matches", "snippet_lines",
                         "source_basename", "max_llc_bc_files", "llc_on_bc", "debug")
                        if k in arguments}
        payload = {"operation": "inspect", "inspect_args": inspect_args,
                   "branch": branch, "sync": sync, "auto_provision": auto_provision,
                   "from_scratch": from_scratch,
                   "preset": preset}
        label = (f"remote inspect '{function_name}' "
                 f"(branch={branch}, preset={preset}, from_scratch={from_scratch})")
    else:
        return ("Invalid 'operation' for remote-run. Use operation='build' (with a "
                "'mode') or operation='inspect' (with a 'function_name').")

    return _start_or_poll_remote("run", label, payload=payload)
