#!/usr/bin/env python3
"""bioseq build MCP server.

Speaks MCP (JSON-RPC 2.0 over stdio) with zero external dependencies — only
the Python stdlib. Exposes two tools:

    start_build(mode)    schedule a build/test/bench run
    status(mode)         report progress, predicted finish, log + report paths

Modes:
    unit+integration   build & run bioseq_aligner_tests_exec + bioseq_aligner_int_tests_exec
    bench              build & run bioseq_bench
    bench-fast         build & run bioseq_bench --fast

Concurrency rules:
    - At most one run per "lock group" at a time.
    - "tests"  group covers unit+integration.
    - "bench"  group covers bench and bench-fast (they share the binary and
      the reporting/ output directory, so concurrent runs would race).
    - Different lock groups can run in parallel
      (e.g. unit+integration alongside bench).

Architecture:
    - Run with no args → MCP server on stdin/stdout.
    - Run with `--run-build <mode>` → detached build worker (this is what the
      server spawns; the worker is *not* meant to be invoked manually).
    - Build state lives in /tmp/bioseq_build_mcp_state.json.
    - Per-run logs live in /tmp/bioseq_build_mcp_<mode>_<timestamp>.log.
    - Builds are detached via start_new_session=True so they survive Claude
      Code session restarts (a 25-min bench is too long to lose to a closed
      window). The worker self-enforces a 35-min hard timeout.

The MCP protocol is implemented from scratch (initialize, tools/list,
tools/call) — it's ~30 lines of JSON-RPC, so pulling in the official `mcp`
package would mean fighting `pip install --externally-managed` for no
benefit. Logging goes to stderr; stdout is reserved for protocol output.
"""

from __future__ import annotations

import argparse
import contextlib
import errno
import fcntl
import json
import os
import select
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

# --------------------------- configuration ---------------------------------

PROJECT_ROOT = Path(__file__).resolve().parent.parent  # main/
BUILD_PRESET = "bench-opt"
BUILD_DIR = PROJECT_ROOT / "build" / BUILD_PRESET
REPORTING_DIR = PROJECT_ROOT / "reporting"

STATE_FILE = Path("/tmp/bioseq_build_mcp_state.json")
LOCK_FILE = Path("/tmp/bioseq_build_mcp.lock")
LOG_DIR = Path("/tmp")

CMAKE_PARALLEL_JOBS = 32
HARD_TIMEOUT_MIN = 35.0
HARD_TIMEOUT_SEC = HARD_TIMEOUT_MIN * 60.0

VALID_MODES = ("unit+integration", "bench", "bench-fast")

# Two runs with the same lock group cannot run in parallel. bench and
# bench-fast share the same binary + reporting/ directory, so they're
# grouped together; unit+integration is its own group.
LOCK_GROUPS = {
    "unit+integration": "tests",
    "bench": "bench",
    "bench-fast": "bench",
}

# Per-mode predicted wall clock (minutes). Used for ETAs in messages and
# kept slightly under the hard timeout so a healthy run never reports a
# negative ETA before being killed.
EXPECTED_DURATION_MIN = {
    "unit+integration": 10.0,
    "bench": 25.0,
    "bench-fast": 20.0,
}

# Subject-verb pairs so message strings read naturally:
#   "Benchmark is already scheduled" (singular)
#   "Unit/integration tests are already scheduled" (plural)
LABELS = {
    "unit+integration": {"label": "Unit/integration tests", "is": "are", "was": "were"},
    "bench":            {"label": "Benchmark",              "is": "is",  "was": "was"},
    "bench-fast":       {"label": "Benchmark (--fast)",     "is": "is",  "was": "was"},
}


def _log(msg: str) -> None:
    """Server-side log line. Always stderr — stdout is JSON-RPC only."""
    print(f"[bioseq-build-mcp] {msg}", file=sys.stderr, flush=True)


# --------------------------- state file I/O --------------------------------

@contextlib.contextmanager
def _global_lock():
    """Cross-process advisory lock around state-file mutations.

    MCP servers are spawned per Claude Code session, so two sessions can
    race on start_build for the same mode. The lock makes the read-check-
    spawn-write critical section atomic across processes.
    """
    LOCK_FILE.touch()
    fd = os.open(str(LOCK_FILE), os.O_RDWR)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        yield
    finally:
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        finally:
            os.close(fd)


def _load_state() -> dict:
    if not STATE_FILE.exists():
        return {}
    try:
        with STATE_FILE.open("r") as f:
            data = json.load(f)
        if not isinstance(data, dict):
            return {}
        return data
    except (OSError, json.JSONDecodeError):
        return {}


def _save_state(state: dict) -> None:
    STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
    tmp = STATE_FILE.with_suffix(".json.tmp")
    with tmp.open("w") as f:
        json.dump(state, f, indent=2, sort_keys=True)
    tmp.replace(STATE_FILE)


def _update_entry(mode: str, **fields) -> None:
    """Merge `fields` into the per-mode state entry. Caller is responsible
    for any locking — workers do their own atomic writes via tmp+rename
    and only mutate their own mode's slot, which is already reserved by
    the parent server under the global lock."""
    state = _load_state()
    entry = dict(state.get(mode) or {})
    entry.update(fields)
    state[mode] = entry
    _save_state(state)


# --------------------------- pid liveness ----------------------------------

def _is_pid_alive(pid) -> bool:
    if not isinstance(pid, int) or pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        # Process exists but we don't own it — extremely unlikely in this
        # workflow, but treat as alive to be safe.
        return True
    except OSError as e:
        if e.errno == errno.ESRCH:
            return False
        return True
    return True


def _refresh(state: dict, mode: str) -> dict | None:
    """Reconcile the recorded status of one mode with reality.

    A "running" entry whose pid is dead means the worker was killed without
    getting a chance to write its terminal status (OOM, SIGKILL from outside,
    machine reboot, etc.) — promote it to "aborted" so the slot is free for
    a fresh start_build call.
    """
    entry = state.get(mode)
    if not entry:
        return None
    if entry.get("status") == "running" and not _is_pid_alive(entry.get("pid")):
        entry = dict(entry)
        entry["status"] = "aborted"
        entry["step"] = "aborted"
        if not entry.get("completed_at"):
            entry["completed_at"] = time.time()
        state[mode] = entry
        _save_state(state)
    return entry


# --------------------------- mode → cmake plan -----------------------------

def _targets_for_mode(mode: str) -> list[str]:
    if mode == "unit+integration":
        return ["bioseq_aligner_tests_exec", "bioseq_aligner_int_tests_exec"]
    if mode == "bench":
        return ["bioseq_bench"]
    if mode == "bench-fast":
        return ["bioseq_bench"]
    raise ValueError(f"unknown mode: {mode}")


def _run_commands_for_mode(mode: str) -> list[list[str]]:
    bin_dir = BUILD_DIR / "bioseq"
    if mode == "unit+integration":
        return [
            [str(bin_dir / "bioseq_aligner_tests_exec"),
             "--reporter", "console::out=-::colour-mode=ansi"],
            [str(bin_dir / "bioseq_aligner_int_tests_exec"),
             "--reporter", "console::out=-::colour-mode=ansi"],
        ]
    if mode == "bench":
        return [[str(bin_dir / "bioseq_bench"),
                 "--reporter", "console::out=-::colour-mode=ansi"]]
    if mode == "bench-fast":
        return [[str(bin_dir / "bioseq_bench"),
                 "--reporter", "console::out=-::colour-mode=ansi", "--fast"]]
    raise ValueError(f"unknown mode: {mode}")


# --------------------------- subprocess streaming --------------------------

def _kill_pgroup(pid: int) -> None:
    """SIGTERM then SIGKILL the entire process group rooted at pid.

    Cmake spawns ninja which spawns clang++; killing only the cmake pid
    leaves the compile jobs running. start_new_session=True puts each
    Popen in its own session/group so killpg reaches the whole tree.
    """
    try:
        pgid = os.getpgid(pid)
    except (ProcessLookupError, OSError):
        return
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(pgid, sig)
        except (ProcessLookupError, OSError):
            return
        # Give children a moment to exit before escalating.
        for _ in range(20):
            time.sleep(0.1)
            try:
                os.killpg(pgid, 0)
            except (ProcessLookupError, OSError):
                return


def _stream_subprocess(cmd: list[str], log_handle, deadline: float,
                       cwd: Path | None = None,
                       env: dict | None = None) -> int:
    """Run cmd, tee combined stdout/stderr to log_handle and stderr, enforce
    deadline. Returns the process return code on clean exit. Raises
    TimeoutError if the deadline is hit.

    The subprocess runs in its own session so the whole tree can be killed
    with killpg(); cmake/ninja/clang++ do not all live under cmake's pid.
    """
    pretty = " ".join(cmd)
    header = f"\n+ {pretty}\n"
    log_handle.write(header)
    log_handle.flush()
    sys.stderr.write(header)
    sys.stderr.flush()

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=str(cwd) if cwd else None,
        env=env,
        bufsize=0,
        start_new_session=True,
    )

    fd = proc.stdout.fileno()
    try:
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                _kill_pgroup(proc.pid)
                raise TimeoutError(f"hard timeout reached during: {pretty}")

            ready, _, _ = select.select([fd], [], [], min(remaining, 1.0))
            if ready:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    chunk = b""
                if not chunk:
                    break
                text = chunk.decode("utf-8", errors="replace")
                log_handle.write(text)
                log_handle.flush()
                sys.stderr.write(text)
                sys.stderr.flush()
                continue
            if proc.poll() is not None:
                # Drain whatever is buffered after the process exited.
                try:
                    while True:
                        chunk = os.read(fd, 65536)
                        if not chunk:
                            break
                        text = chunk.decode("utf-8", errors="replace")
                        log_handle.write(text)
                        log_handle.flush()
                        sys.stderr.write(text)
                        sys.stderr.flush()
                except OSError:
                    pass
                break
        return proc.wait()
    finally:
        if proc.poll() is None:
            _kill_pgroup(proc.pid)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass


# --------------------------- build worker ----------------------------------

def _report_index(filename: str) -> int:
    # filenames look like report_build_<N>.json
    try:
        return int(filename.removeprefix("report_build_").removesuffix(".json"))
    except (ValueError, AttributeError):
        return -1


def _newest_report(snapshot_paths: set[Path]) -> Path | None:
    """Return the highest-numbered report file produced after `snapshot_paths`
    was taken (i.e. one not in the snapshot)."""
    if not REPORTING_DIR.exists():
        return None
    current = set(REPORTING_DIR.glob("report_build_*.json"))
    new = current - snapshot_paths
    if not new:
        return None
    return max(new, key=lambda p: _report_index(p.name))


def _run_build_worker(mode: str) -> int:
    """Worker entry point — runs detached from the MCP server.

    Reads the slot the server reserved in state, walks configure → build →
    run, updating `step`/`status` as it goes. The hard timeout is enforced
    by `_stream_subprocess` against `started_at + HARD_TIMEOUT_SEC`.
    """
    state = _load_state()
    entry = state.get(mode)
    if not entry:
        _log(f"worker for {mode}: no state entry, exiting")
        return 1

    started_at = float(entry["started_at"])
    log_path = Path(entry["log_file"])
    deadline = started_at + HARD_TIMEOUT_SEC

    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_handle = log_path.open("w", buffering=1)
    log_handle.write(
        f"# bioseq build worker — mode={mode} pid={os.getpid()} "
        f"started={time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(started_at))}\n"
        f"# project_root={PROJECT_ROOT}\n"
        f"# build_dir={BUILD_DIR}\n"
        f"# preset={BUILD_PRESET}\n"
        f"# hard_timeout_min={HARD_TIMEOUT_MIN}\n"
    )
    log_handle.flush()

    pre_reports: set[Path] = set()
    if "bench" in mode and REPORTING_DIR.exists():
        pre_reports = set(REPORTING_DIR.glob("report_build_*.json"))

    env = os.environ.copy()
    env["CMAKE_EXPORT_COMPILE_COMMANDS"] = "1"

    try:
        _update_entry(mode, step="configuring")
        rc = _stream_subprocess(
            ["cmake", "--preset", BUILD_PRESET],
            log_handle, deadline, cwd=PROJECT_ROOT, env=env,
        )
        if rc != 0:
            raise RuntimeError(f"cmake configure exited with code {rc}")

        _update_entry(mode, step="building")
        for tgt in _targets_for_mode(mode):
            rc = _stream_subprocess(
                ["cmake", "--build", str(BUILD_DIR), "--target", tgt,
                 "--parallel", str(CMAKE_PARALLEL_JOBS)],
                log_handle, deadline, cwd=PROJECT_ROOT, env=env,
            )
            if rc != 0:
                raise RuntimeError(f"build of target '{tgt}' exited with code {rc}")

        # Maintain the build/compile_commands.json symlink — clangd / IDEs
        # probe build/ by default. Only matters for human operators, but
        # cheap to keep working.
        cc = BUILD_DIR / "compile_commands.json"
        if cc.exists():
            link = BUILD_DIR.parent / "compile_commands.json"
            try:
                if link.is_symlink() or link.exists():
                    link.unlink()
                link.symlink_to(Path(BUILD_PRESET) / "compile_commands.json")
            except OSError:
                pass

        _update_entry(mode, step="running")
        for cmd in _run_commands_for_mode(mode):
            rc = _stream_subprocess(
                cmd, log_handle, deadline, cwd=BUILD_DIR, env=env,
            )
            if rc != 0:
                # Bench exits non-zero when regression rules are violated
                # (exit code 2) — we still want to surface the report path,
                # so this is a "completed with violations" outcome rather
                # than a worker failure. The console log already explains
                # the violations.
                if "bench" in mode and rc == 2:
                    _log(f"bench reported regression violations (exit {rc}); "
                         "treating as completed-with-violations")
                    break
                raise RuntimeError(f"run failed with exit code {rc}: {' '.join(cmd)}")

        report_path = _newest_report(pre_reports) if "bench" in mode else None
        completed_at = time.time()
        _update_entry(
            mode,
            status="completed",
            step="completed",
            completed_at=completed_at,
            exit_code=0,
            report_file=str(report_path) if report_path else None,
        )
        log_handle.write(
            f"\n# worker exiting cleanly at "
            f"{time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(completed_at))}\n"
        )
        return 0

    except TimeoutError as e:
        completed_at = time.time()
        _update_entry(
            mode,
            status="killed_timeout",
            step="killed_timeout",
            completed_at=completed_at,
            exit_code=None,
            notes=str(e),
        )
        log_handle.write(f"\n# killed by 35-min hard timeout: {e}\n")
        return 124

    except Exception as e:
        completed_at = time.time()
        _update_entry(
            mode,
            status="failed",
            step="failed",
            completed_at=completed_at,
            exit_code=None,
            notes=str(e),
        )
        log_handle.write(f"\n# worker failed: {e}\n")
        return 1

    finally:
        log_handle.close()


# --------------------------- tool implementations --------------------------

def _format_minutes(minutes: float, *, plural_int: bool = False) -> str:
    """Render an ETA / duration in the format the spec expects.

    `plural_int=True` → "20 minutes" form (used by the first-start message
    and completed-run total times).
    Otherwise → "5.23 min" form (used by the already-running predicted-finish
    message).
    """
    if plural_int:
        return f"{round(minutes)} minutes"
    return f"{minutes:.2f} min"


def _running_in_same_group(state: dict, mode: str) -> tuple[str | None, dict | None]:
    """Find any currently-running run that conflicts with `mode`.

    Returns (running_mode, entry) or (None, None). Conflicts are decided by
    LOCK_GROUPS — bench and bench-fast share a group so starting bench while
    bench-fast is running is rejected.
    """
    target_group = LOCK_GROUPS[mode]
    for other_mode in VALID_MODES:
        entry = _refresh(state, other_mode)
        if not entry:
            continue
        if entry.get("status") != "running":
            continue
        if LOCK_GROUPS.get(other_mode) == target_group:
            return other_mode, entry
    return None, None


def tool_start_build(mode: str) -> str:
    if mode not in VALID_MODES:
        return (f"Invalid mode '{mode}'. Valid modes are: "
                f"{', '.join(VALID_MODES)}.")

    with _global_lock():
        state = _load_state()
        running_mode, running_entry = _running_in_same_group(state, mode)
        if running_entry:
            elapsed_min = (time.time() - running_entry["started_at"]) / 60.0
            eta_min = max(
                0.0,
                EXPECTED_DURATION_MIN[running_mode] - elapsed_min,
            )
            label = LABELS[running_mode]
            step = running_entry.get("step", "running")
            return (f"{label['label']} {label['is']} already scheduled. "
                    f"Current step: {step}. "
                    f"Predicted finish in {_format_minutes(eta_min)}")

        # Reserve the slot atomically before spawning the worker so a
        # concurrent start_build observes "running" immediately and
        # doesn't race past the same check.
        started_at = time.time()
        timestamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(started_at))
        log_path = LOG_DIR / (
            f"bioseq_build_mcp_{mode.replace('+', '_')}_{timestamp}.log"
        )

        server_path = Path(__file__).resolve()
        proc = subprocess.Popen(
            [sys.executable, str(server_path), "--run-build", mode],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            close_fds=True,
        )

        state[mode] = {
            "mode": mode,
            "status": "running",
            "step": "spawning",
            "started_at": started_at,
            "completed_at": None,
            "pid": proc.pid,
            "log_file": str(log_path),
            "exit_code": None,
            "report_file": None,
            "notes": None,
        }
        _save_state(state)
        _log(f"started worker pid={proc.pid} mode={mode} log={log_path}")

    label = LABELS[mode]
    expected = EXPECTED_DURATION_MIN[mode]
    return (f"{label['label']} {label['was']} scheduled. "
            f"Predicted finish in {_format_minutes(expected, plural_int=True)}.")


def tool_status(mode: str) -> str:
    if mode not in VALID_MODES:
        return (f"Invalid mode '{mode}'. Valid modes are: "
                f"{', '.join(VALID_MODES)}.")

    state = _load_state()
    entry = _refresh(state, mode)
    label = LABELS[mode]

    if not entry:
        return (f"{label['label']} {label['was']} never scheduled. "
                f"Use start_build to start a run.")

    log_path = entry.get("log_file") or "(no log file)"
    status = entry.get("status")

    if status == "running":
        elapsed_min = (time.time() - entry["started_at"]) / 60.0
        eta_min = max(0.0, EXPECTED_DURATION_MIN[mode] - elapsed_min)
        step = entry.get("step", "running")
        return (f"{label['label']} {label['is']} already scheduled. "
                f"Current step: {step}. "
                f"Predicted finish in {_format_minutes(eta_min)}. "
                f"Log: {log_path}")

    completed_at = entry.get("completed_at") or time.time()
    duration_min = (completed_at - entry["started_at"]) / 60.0

    if status == "completed":
        msg = (f"{label['label']} {label['was']} already completed "
               f"(total time: {_format_minutes(duration_min, plural_int=True)}). "
               f"Log: {log_path}")
        if entry.get("report_file"):
            msg += f". Report: {entry['report_file']}"
        return msg

    if status == "killed_timeout":
        return ("Last run was forcefully killed due to timeout. "
                f"Logs for inspection are available here: {log_path}")

    if status == "failed":
        notes = entry.get("notes") or "(no notes)"
        return (f"{label['label']} run failed after "
                f"{_format_minutes(duration_min)}: {notes}. "
                f"Log: {log_path}")

    if status == "aborted":
        return (f"{label['label']} run was aborted (worker process died "
                f"unexpectedly after {_format_minutes(duration_min)}). "
                f"Log: {log_path}")

    return f"{label['label']} status: {status}. Log: {log_path}"


# --------------------------- MCP protocol layer ----------------------------

PROTOCOL_VERSION = "2024-11-05"
SERVER_INFO = {"name": "bioseq-build-mcp", "version": "1.0.0"}

TOOL_DEFINITIONS = [
    {
        "name": "start_build",
        "description": (
            "Schedule a build/test/benchmark run. The build runs detached so "
            "it survives MCP server restarts. If a run of the same lock group "
            "is already in progress (bench and bench-fast share a group), "
            "returns the current status instead of starting a duplicate. "
            "Modes: 'unit+integration' (~10 min), 'bench' (~25 min), "
            "'bench-fast' (~20 min). Hard timeout: 35 min."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "mode": {
                    "type": "string",
                    "enum": list(VALID_MODES),
                    "description": "Which run to start.",
                },
            },
            "required": ["mode"],
        },
    },
    {
        "name": "status",
        "description": (
            "Report the status of a previously scheduled run for `mode`. "
            "While running, returns current step + predicted finish. After "
            "completion, returns total duration, log file path, and (for "
            "bench modes) the path to the new report_build_<N>.json under "
            "main/reporting/."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "mode": {
                    "type": "string",
                    "enum": list(VALID_MODES),
                    "description": "Which run to query.",
                },
            },
            "required": ["mode"],
        },
    },
]


def _write_message(msg: dict) -> None:
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def _read_message() -> dict | None:
    line = sys.stdin.readline()
    if not line:
        return None
    line = line.strip()
    if not line:
        return None
    try:
        return json.loads(line)
    except json.JSONDecodeError as e:
        _log(f"malformed JSON-RPC line dropped: {e}")
        return {"_dropped": True}


def _make_response(req_id, *, result=None, error=None) -> dict:
    msg = {"jsonrpc": "2.0", "id": req_id}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result
    return msg


def _handle_initialize(params: dict) -> dict:
    return {
        "protocolVersion": PROTOCOL_VERSION,
        "capabilities": {"tools": {}},
        "serverInfo": SERVER_INFO,
    }


def _handle_tools_list(params: dict) -> dict:
    return {"tools": TOOL_DEFINITIONS}


def _handle_tools_call(params: dict) -> dict:
    name = params.get("name")
    args = params.get("arguments") or {}
    mode = args.get("mode", "")
    if name == "start_build":
        text = tool_start_build(mode)
    elif name == "status":
        text = tool_status(mode)
    else:
        return {
            "content": [{"type": "text", "text": f"unknown tool: {name}"}],
            "isError": True,
        }
    return {"content": [{"type": "text", "text": text}]}


HANDLERS = {
    "initialize": _handle_initialize,
    "tools/list": _handle_tools_list,
    "tools/call": _handle_tools_call,
}


def _serve() -> None:
    _log(f"starting MCP server (pid={os.getpid()})")
    while True:
        msg = _read_message()
        if msg is None:
            _log("stdin closed; exiting")
            return
        if msg.get("_dropped"):
            continue
        if "id" not in msg:
            # Notification — no response is expected. We still log so users
            # can confirm the client handshake reached us.
            _log(f"notification: {msg.get('method')}")
            continue
        method = msg.get("method")
        params = msg.get("params") or {}
        handler = HANDLERS.get(method)
        if handler is None:
            _write_message(_make_response(
                msg["id"],
                error={"code": -32601, "message": f"method not found: {method}"},
            ))
            continue
        try:
            result = handler(params)
            _write_message(_make_response(msg["id"], result=result))
        except Exception as e:
            _log(f"handler {method} raised: {e}")
            _write_message(_make_response(
                msg["id"],
                error={"code": -32000, "message": f"{type(e).__name__}: {e}"},
            ))


# --------------------------- entry point -----------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--run-build",
        choices=VALID_MODES,
        default=None,
        help="(internal) run as a detached build worker for the given mode",
    )
    args = parser.parse_args()

    if args.run_build is not None:
        return _run_build_worker(args.run_build)

    _serve()
    return 0


if __name__ == "__main__":
    sys.exit(main())
