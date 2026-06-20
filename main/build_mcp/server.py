#!/usr/bin/env python3
"""bioseq build MCP server.

Speaks MCP (JSON-RPC 2.0 over stdio) with zero external dependencies — only
the Python stdlib. Exposes two tools:

    start_build(mode)    schedule a build/test/bench run
    status(mode)         report progress, predicted finish, log + report paths

For modes list see MODES dict.

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

from typing import List, Dict, Set, Optional, Any

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
from dataclasses import dataclass
from pathlib import Path

# inspect tool — see inspect_tool.py docstring for design.
# (Named `inspect_tool` rather than `inspect` to avoid shadowing the
# stdlib `inspect` module when this script's directory ends up on
# sys.path[0].)
from inspect_tool import (
    run_inspect as _run_inspect_impl,
    MATCHES_WARN_THRESHOLD,
)

# Lazy provisioning for the document tools (npm qmd + poetry jsonschema). Imported here only
# to define functions; ensure_doc_env() is called on first doc-tool use, never at import and
# never by the detached --run-build worker (which is stdlib-only).
import bootstrap  # noqa: E402

# --------------------------- configuration ---------------------------------

PROJECT_ROOT = Path(__file__).resolve().parent.parent  # main/

# DOC0270 — two parallel build trees:
#
#   "bench-opt"        — default, fast. -O3 + NDEBUG, no -g, no -save-temps.
#                        Used by every normal start_build call. Lives under
#                        build/bench-opt/.
#   "bench-opt-debug"  — heavier, slower. Same -O3 + NDEBUG, plus -g (DWARF)
#                        and -save-temps=obj (per-TU .s/.ii/.bc artifacts).
#                        Selected with debug=true on start_build. Required by
#                        the `inspect` tool — it pulls disassembly from the
#                        objdump --line-numbers output (needs DWARF) and the
#                        pre-LTO assembly view (needs the per-TU .s files).
#                        Lives under build/bench-opt-debug/ so it does not
#                        invalidate the default tree's ninja graph.
#
# Both presets share the rest of the toolchain (vcpkg, arm64-osx target).
# The build dir is selected per-call from `debug`; the state file records
# which mode each run used so `status` and `inspect` can find the right
# artifacts.
DEFAULT_BUILD_PRESET = "bench-opt"
DEBUG_BUILD_PRESET = "bench-opt-debug"
DEFAULT_BUILD_DIR = PROJECT_ROOT / "build" / DEFAULT_BUILD_PRESET
DEBUG_BUILD_DIR = PROJECT_ROOT / "build" / DEBUG_BUILD_PRESET
REPORTING_DIR = PROJECT_ROOT / "reporting"


def preset_for(debug: bool) -> str:
    return DEBUG_BUILD_PRESET if debug else DEFAULT_BUILD_PRESET


def build_dir_for(debug: bool) -> Path:
    return DEBUG_BUILD_DIR if debug else DEFAULT_BUILD_DIR


def bin_dir_for(debug: bool) -> Path:
    return build_dir_for(debug) / "bioseq"


# Backwards-compatible aliases — the old module-level constants are still
# read by callers that haven't been updated to thread `debug` through. They
# always point at the default (non-debug) build tree.
BUILD_PRESET = DEFAULT_BUILD_PRESET
BUILD_DIR = DEFAULT_BUILD_DIR
BIN_DIR = BUILD_DIR / "bioseq"

STATE_FILE = Path("/tmp/bioseq_build_mcp_state.json")
LOCK_FILE = Path("/tmp/bioseq_build_mcp.lock")
LOG_DIR = Path("/tmp")

def _safe_parallel_jobs() -> int:
    """Cap build parallelism by BOTH core count and RAM.

    The heavy C++26 template TUs (pipeline/pipe.cppm, scheduler/dispatch_buffer.cppm,
    storage/sequence_storage_manager.cppm, minimap_port.cppm, ksw2_batched.cppm)
    each peak around ~3-4 GB RSS at -O3 + thin-LTO. The previous hardcoded
    -j 32 launched dozens of them at once; on a 16 GB box that swap-thrashed
    to a near-total stall (a cold postgresql_bioseq build completed only 2
    build edges in the full 35-min hard-timeout window). Budgeting ~3 GB per
    concurrent heavy compile keeps cold builds making forward progress without
    paging. On this 8-core / 16 GB machine this yields -j 5."""
    cpus = os.cpu_count() or 4
    try:
        mem_gb = (os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")) / (1024 ** 3)
    except (ValueError, OSError, AttributeError):
        mem_gb = 16.0
    return max(2, min(cpus, int(mem_gb // 3)))


CMAKE_PARALLEL_JOBS = _safe_parallel_jobs()
HARD_TIMEOUT_MIN = 90.0
HARD_TIMEOUT_SEC = HARD_TIMEOUT_MIN * 60.0

# Prompt template injected into every `subagent` call.
# Placeholders replaced at call time via str.replace (not f-string so the JSON
# braces in the response schema don't need escaping):
#   {requested_outputs_section}  — injected list of named outputs or empty string
#   {parent_prompt}              — the caller's task prompt
_SUBAGENT_PROMPT_TEMPLATE = (
    "# Context\n"
    "You are subagent spawned from main AI agent. Your task will be stated in prompt "
    "(from parent AI agent) below.\n\n"
    "CRITICAL RULES — you must follow these unconditionally:\n"
    "- This is a NON-INTERACTIVE session. You must NEVER ask questions or request clarification.\n"
    "- You must complete ALL work described in the prompt in a single run. No deferring, no "
    "'I will do this later', no partial work. Leaving any part of the task unfinished is a "
    "CRITICAL FAILURE.\n"
    "- If anything is ambiguous, make a reasonable decision and proceed. Document your decision "
    "in task_status.\n\n"
    "Always follow this workflow no matter what the prompt underneath says:\n"
    "1. Read main/DOCS/CURRENT/GUIDELINES/0321_BIOSEQ_CODE_AND_WORKFLOW_GUIDELINES.md in full\n"
    "2. Read prompt content and do ALL tasks specified there in full. Before finishing, spawn a "
    "subagent that will review your response (this is to ensure the parent AI agent gets the "
    "highest quality answer possible). Iterate this review subagent until saturation (no further "
    "feedback). The review subagent must also follow the non-interactive, no-deferral rules.\n"
    "3. Return response to the parent AI agent in the JSON format. The response format should "
    "follow this pattern:\n"
    "{\n"
    '    "task_status": "Detailed information about completion of this task",\n'
    '    "bugs": "Any bugs you found that needs to be flagged",\n'
    '    "risks": "Any risks you found that needs to be flagged",\n'
    '    "changed_files": [],\n'
    '    "review_tldr": [],\n'
    '    "output": []\n'
    "}\n"
    "The `output` array must contain one string entry per requested output (see below), "
    "in the same order they were listed. Each entry is a free-form string — prose, code, "
    "bullet points, whatever best answers that specific request.\n"
    "{requested_outputs_section}\n"
    "# Prompt\n"
    "PROMPT FROM PARENT AI AGENT PASTED AS-IS BELOW:\n"
    "{parent_prompt}"
)

def _build_requested_outputs_section(requested_outputs: list[str]) -> str:
    """Render the requested_outputs list into the prompt section that tells the
    subagent exactly what to put in each slot of the `output` array."""
    if not requested_outputs:
        return (
            "No specific outputs were requested by the parent agent. "
            "Leave the `output` array empty ([]).\n\n"
        )
    lines = [
        "\n# Requested Outputs\n"
        "The parent agent wants the following items in the `output` array, "
        "one element per item, in this exact order:\n"
    ]
    for i, item in enumerate(requested_outputs, 1):
        lines.append(f"{i}. {item}")
    lines.append("\n")
    return "\n".join(lines)


# --------------------------- subagent state --------------------------------
# Mirrors the build-worker pattern: one detached process, one state file, one
# lock file.  At most one subagent runs at a time (no lock groups needed).

SUBAGENT_STATE_FILE = Path("/tmp/bioseq_subagent_mcp_state.json")
SUBAGENT_LOCK_FILE  = Path("/tmp/bioseq_subagent_mcp.lock")
# Hard ceiling applied inside the worker regardless of the caller's timeout_sec.
SUBAGENT_HARD_TIMEOUT_SEC = 90 * 60.0


@contextlib.contextmanager
def _subagent_global_lock():
    SUBAGENT_LOCK_FILE.touch()
    fd = os.open(str(SUBAGENT_LOCK_FILE), os.O_RDWR)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        yield
    finally:
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        finally:
            os.close(fd)


def _load_subagent_state() -> dict:
    if not SUBAGENT_STATE_FILE.exists():
        return {}
    try:
        with SUBAGENT_STATE_FILE.open("r") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def _save_subagent_state(state: dict) -> None:
    SUBAGENT_STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
    tmp = SUBAGENT_STATE_FILE.with_suffix(".json.tmp")
    with tmp.open("w") as f:
        json.dump(state, f, indent=2, sort_keys=True)
    tmp.replace(SUBAGENT_STATE_FILE)


def _update_subagent_entry(**fields) -> None:
    state = _load_subagent_state()
    state.update(fields)
    _save_subagent_state(state)


def _refresh_subagent(state: dict) -> dict:
    """Promote a 'running' entry whose worker pid is dead to 'aborted'."""
    if state.get("status") == "running" and not _is_pid_alive(state.get("pid")):
        state = dict(state)
        state["status"] = "aborted"
        state["step"] = "aborted"
        if not state.get("completed_at"):
            state["completed_at"] = time.time()
        _save_subagent_state(state)
    return state


@dataclass
class ModeDefinition:
    """
    Two runs with the same lock group cannot run in parallel. bench and
    bench-fast share the same binary + reporting/ directory, so they're
    grouped together; unit+integration is its own group.
    """
    lock_group: str
    """
    Per-mode predicted wall clock (minutes). Used for ETAs in messages and
    kept slightly under the hard timeout so a healthy run never reports a
    negative ETA before being killed.
    """
    expected_duration_min: float
    """Same as expected_duration_min but estimates execution time only for compilation"""
    build_only_expected_duration_min: float
    """Human-readable name of this mode"""
    label: str
    """Adjusts verbs so messages read naturally"""
    is_verb_plular: bool
    """Targets we need to build via CMake"""
    targets: List[str]
    """
    Commands we want to run. First element of each command is the binary
    basename — resolved against the active build tree's `bioseq/` directory
    at run time (see `bin_dir_for(debug)`). This lets the same MODE definition
    drive both the fast (build/bench-opt/) and the debug (build/bench-opt-debug/)
    binaries; the worker decides which by reading the run's `debug` flag.
    """
    commands: List[List[str]]
    """Extra documentation"""
    docs: str
    """This applies only to benchmarks. Defines subdirectory within REPORTING_DIR where benchmark report is saved"""
    reporting_subdir_name: Optional[str] = None
    """This applies only to benchmark. Is this benchmark target?"""
    is_bench_mode: bool = False
    """If true, install the freshly-built postgresql_bioseq extension (dylib +
    .control + .sql) into the running PostgreSQL before executing the run
    commands. Integration tests connect to that server and DROP/CREATE EXTENSION
    from the installed files, so without this they would silently exercise a
    stale, previously-installed extension. Requires 'postgresql_bioseq' to be in
    `targets` so the dylib exists at install time."""
    install_extension: bool = False

    @property
    def verb_is(self) -> str:
        return "are" if self.is_verb_plular else "is"

    @property
    def verb_was(self) -> str:
        return "were" if self.is_verb_plular else "was"

    @property
    def reporting_dir(self) -> str:
        if not self.is_bench_mode or not self.reporting_subdir_name:
            return REPORTING_DIR / "__does_not_exist__"
        return REPORTING_DIR / self.reporting_subdir_name

MODES: Dict[str, ModeDefinition] = {
    "unit+integration": ModeDefinition(
        lock_group="tests",
        expected_duration_min=22.0,
        build_only_expected_duration_min=10.0,
        label="Unit/integration tests",
        docs="Run all bioseq unit and integration tests",
        is_verb_plular=True,
        # postgresql_bioseq MUST be built first: the integration tests connect to
        # the running PostgreSQL and CREATE EXTENSION from the *installed* files,
        # so the extension dylib + SQL must be (re)built and installed before the
        # int-test client runs. Without this the int tests exercise whatever
        # stale extension happens to be installed in the server.
        targets=["postgresql_bioseq",
                 "bioseq_aligner_tests_exec", "bioseq_aligner_int_tests_exec"],
        install_extension=True,
        commands=[
            ["bioseq_aligner_tests_exec",
             "--reporter", "console::out=-::colour-mode=ansi"],
            ["bioseq_aligner_int_tests_exec",
             "--reporter", "console::out=-::colour-mode=ansi"],
        ],
    ),
    "bench": ModeDefinition(
        lock_group="bench",
        expected_duration_min=25.0,
         build_only_expected_duration_min=1.5,
        label="Benchmark for bioseq",
        docs="Run primary bioseq benchmark",
        is_verb_plular=False,
        targets=["bioseq_bench"],
        commands=[["bioseq_bench"]],
        reporting_subdir_name="bioseq_bench",
        is_bench_mode=True,
    ),
    "bench-fast": ModeDefinition(
        lock_group="bench",
        expected_duration_min=20.0,
        build_only_expected_duration_min=1.5,
        label="Benchmark for bioseq (--fast)",
        docs="Run primary bioseq benchmark in fast mode for faster iteration",
        is_verb_plular=False,
        targets=["bioseq_bench"],
        commands=[["bioseq_bench", "--fast"]],
        reporting_subdir_name="bioseq_bench",
        is_bench_mode=True,
    ),
    "bench-io": ModeDefinition(
        lock_group="bench",
        expected_duration_min=8.0,
        build_only_expected_duration_min=1.5,
        label="Benchmark for bioseq IO module",
        docs="Run benchmark for IO module residing in io/ directory. Note this is separate benchmark dedicated only to IO interfaces",
        is_verb_plular=False,
        targets=["bioseq_io_bench"],
        commands=[["bioseq_io_bench"]],
        reporting_subdir_name="bioseq_io_bench",
        is_bench_mode=True,
    ),
    "bench-e2e": ModeDefinition(
        lock_group="bench",
        expected_duration_min=45.0,
        build_only_expected_duration_min=3.0,
        label="End-to-end SQL benchmark (bioseq.load + mmsearch vs minimap2)",
        docs=("DOC0286 Part 2 — end-to-end SQL-driven benchmark. Drives postgres "
              "over libpq (integration_tests/common.cppm): bioseq.load + SELECT "
              "mmsearch vs minimap2 over the 2/6 Gbp x dna4/dna5 x {no_index, "
              "just_index} matrix. Like the integration tests it installs the "
              "freshly-built extension first (the bench's bioseq.load/mmsearch "
              "must hit current code, not a stale installed copy)."),
        is_verb_plular=False,
        # postgresql_bioseq MUST be built + installed first: the bench connects to
        # the running PostgreSQL and CREATE EXTENSIONs from the *installed* files.
        targets=["postgresql_bioseq", "bioseq_bench_e2e"],
        install_extension=True,
        commands=[["bioseq_bench_e2e"]],
        reporting_subdir_name="bioseq_bench_e2e",
        is_bench_mode=True,
    ),
}
VALID_MODES: Set[str] = set(MODES.keys())

def get_modes_short_documentation() -> str:
    modes_list_str = ", ".join([f"'{mode}' (~{int(MODES[mode].expected_duration_min)} min and ~{int(MODES[mode].build_only_expected_duration_min)} min for build-only)" for mode in VALID_MODES])
    return f"Modes: {modes_list_str}";

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


def _restart_postgresql(active_build_dir: Path, log_handle, deadline: float) -> None:
    """Restart the PostgreSQL postmaster after installing a new extension dylib.

    On macOS, fork()d backend processes inherit the postmaster's dyld image
    list.  When the postmaster has an older bioseq.dylib already mapped,
    dlopen("bioseq.dylib") in every fork'd child returns the old cached handle
    — dlsym() then fails to find functions added in the new build.  A
    postmaster restart evicts the stale mapping; the first CREATE EXTENSION
    after restart loads the freshly installed dylib.
    """
    cmake_cache = active_build_dir / "CMakeCache.txt"
    pg_config: str | None = None
    if cmake_cache.exists():
        for line in cmake_cache.read_text(errors="replace").splitlines():
            if line.startswith("PostgreSQL_CONFIG:FILEPATH="):
                pg_config = line.split("=", 1)[1].strip()
                break
    if not pg_config:
        _log("WARNING: PostgreSQL_CONFIG not in CMakeCache.txt; skipping PG restart")
        return

    try:
        res = subprocess.run([pg_config, "--bindir"],
                             capture_output=True, text=True, timeout=10)
        pg_bindir = res.stdout.strip()
    except Exception as exc:
        _log(f"WARNING: pg_config --bindir failed ({exc}); skipping PG restart")
        return

    pg_ctl = os.path.join(pg_bindir, "pg_ctl")
    psql   = os.path.join(pg_bindir, "psql")
    if not os.path.isfile(pg_ctl):
        _log(f"WARNING: pg_ctl not found at {pg_ctl}; skipping PG restart")
        return

    try:
        res = subprocess.run([psql, "-t", "-c", "SHOW data_directory;", "postgres"],
                             capture_output=True, text=True, timeout=10)
        data_dir = res.stdout.strip()
    except Exception as exc:
        _log(f"WARNING: could not query data_directory ({exc}); skipping PG restart")
        return

    if not data_dir or not os.path.isdir(data_dir):
        _log(f"WARNING: data_directory '{data_dir}' is not a directory; skipping PG restart")
        return

    # On macOS use arch -arm64 so pg_ctl (universal binary) runs natively;
    # Rosetta and dyld interact badly with arm64-only extension dylibs.
    if sys.platform == "darwin":
        restart_cmd = ["arch", "-arm64", pg_ctl, "-D", data_dir, "restart"]
    else:
        restart_cmd = [pg_ctl, "-D", data_dir, "restart"]

    _log(f"Restarting PostgreSQL (data_dir={data_dir}) to evict stale dylib cache ...")
    # DON'T use _stream_subprocess here: that function blocks until the pipe
    # reaches EOF.  pg_ctl restart forks the new postmaster, which inherits
    # pg_ctl's stdout pipe and keeps it open indefinitely, causing a hang.
    # Writing stdout directly to log_handle (a file) avoids the pipe: Python
    # then calls proc.wait() which returns as soon as pg_ctl itself exits
    # (after the postmaster signals readiness), regardless of what the
    # postmaster does with the file handle afterwards.
    pretty = " ".join(restart_cmd)
    log_handle.write(f"\n+ {pretty}\n")
    log_handle.flush()
    sys.stderr.write(f"\n+ {pretty}\n")
    sys.stderr.flush()
    try:
        result = subprocess.run(
            restart_cmd,
            stdout=log_handle,
            stderr=subprocess.STDOUT,
            timeout=90,
        )
        log_handle.flush()
        if result.returncode != 0:
            _log(f"WARNING: pg_ctl restart exited {result.returncode}; tests may load a stale dylib")
        else:
            _log("PostgreSQL restarted — new dylib is now live")
    except subprocess.TimeoutExpired:
        _log("WARNING: pg_ctl restart timed out after 90s; tests may load a stale dylib")


# --------------------------- build worker ----------------------------------

def _report_index(filename: str) -> int:
    # filenames look like report_build_<N>.json
    try:
        return int(filename.removeprefix("report_build_").removesuffix(".json"))
    except (ValueError, AttributeError):
        return -1


def _newest_report(snapshot_paths: set[Path], mode: str) -> Path | None:
    """Return the highest-numbered report file produced after `snapshot_paths`
    was taken (i.e. one not in the snapshot)."""
    mode_details = MODES[mode]
    if not mode_details.reporting_dir.exists():
        return None
    current = set(mode_details.reporting_dir.glob("report_build_*.json"))
    new = current - snapshot_paths
    if not new:
        return None
    return max(new, key=lambda p: _report_index(p.name))


def _run_build_worker(mode: str, build_only: bool = False,
                      debug: bool = False) -> int:
    """Worker entry point — runs detached from the MCP server.

    Reads the slot the server reserved in state, walks configure → build →
    run, updating `step`/`status` as it goes. The hard timeout is enforced
    by `_stream_subprocess` against `started_at + HARD_TIMEOUT_SEC`.

    `debug=True` routes the build into `build/bench-opt-debug/` using the
    `bench-opt-debug` preset — the per-TU `.s`/`.bc` + DWARF artefacts the
    `inspect` tool consumes. `debug=False` (default) uses the lean
    `bench-opt` preset that DOC0270 split out for everyday builds.
    """
    state = _load_state()
    entry = state.get(mode)
    if not entry:
        _log(f"worker for {mode}: no state entry, exiting")
        return 1

    started_at = float(entry["started_at"])
    log_path = Path(entry["log_file"])
    deadline = started_at + HARD_TIMEOUT_SEC

    active_preset = preset_for(debug)
    active_build_dir = build_dir_for(debug)
    active_bin_dir = bin_dir_for(debug)

    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_handle = log_path.open("w", buffering=1)
    log_handle.write(
        f"# bioseq build worker — mode={mode} pid={os.getpid()} "
        f"started={time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(started_at))}\n"
        f"# project_root={PROJECT_ROOT}\n"
        f"# build_dir={active_build_dir}\n"
        f"# preset={active_preset}\n"
        f"# debug={debug}\n"
        f"# hard_timeout_min={HARD_TIMEOUT_MIN}\n"
    )
    log_handle.flush()

    mode_details = MODES[mode]

    pre_reports: set[Path] = set()
    if mode_details.is_bench_mode and mode_details.reporting_dir.exists():
        pre_reports = set(mode_details.reporting_dir.glob("report_build_*.json"))

    env = os.environ.copy()
    env["CMAKE_EXPORT_COMPILE_COMMANDS"] = "1"

    try:
        _update_entry(mode, step="configuring", build_only=build_only,
                      debug=debug, preset=active_preset,
                      build_dir=str(active_build_dir))
        # Only run the (expensive) configure when the tree isn't configured yet.
        # Re-running `cmake --preset` on an already-configured C++-modules tree
        # regenerates the Ninja dyndep/modmap graph and invalidates EVERY module
        # TU across all targets (~219 edges here vs ~34 for a true incremental),
        # turning every run into a full rebuild that cannot finish inside the
        # hard timeout. Ninja still auto-reconfigures via its RERUN_CMAKE rule
        # when CMakeLists / cache / vcpkg manifest actually change, so a later
        # `cmake --build` still picks up genuine configuration changes.
        if not (active_build_dir / "build.ninja").exists():
            rc = _stream_subprocess(
                ["cmake", "--preset", active_preset],
                log_handle, deadline, cwd=PROJECT_ROOT, env=env,
            )
            if rc != 0:
                raise RuntimeError(f"cmake configure exited with code {rc}")
        else:
            log_handle.write(
                f"# build tree already configured ({active_build_dir / 'build.ninja'}); "
                "skipping redundant `cmake --preset` to avoid a full C++-module "
                "rebuild (Ninja auto-reconfigures if CMake inputs changed).\n")
            log_handle.flush()

        _update_entry(mode, step="building", build_only=build_only,
                      debug=debug)
        for tgt in mode_details.targets:
            rc = _stream_subprocess(
                ["cmake", "--build", str(active_build_dir), "--target", tgt,
                 "--parallel", str(CMAKE_PARALLEL_JOBS)],
                log_handle, deadline, cwd=PROJECT_ROOT, env=env,
            )
            if rc != 0:
                raise RuntimeError(f"build of target '{tgt}' exited with code {rc}")

        # Maintain the build/compile_commands.json symlink — clangd / IDEs
        # probe build/ by default. Only matters for human operators, but
        # cheap to keep working. Symlink follows the active preset so the
        # debug build can also be the source of compile_commands when the
        # operator is iterating against -g + -save-temps.
        cc = active_build_dir / "compile_commands.json"
        if cc.exists():
            link = active_build_dir.parent / "compile_commands.json"
            try:
                if link.is_symlink() or link.exists():
                    link.unlink()
                link.symlink_to(Path(active_preset) / "compile_commands.json")
            except OSError:
                pass

        if build_only:
            completed_at = time.time()
            _update_entry(
                mode,
                status="completed",
                step="completed",
                completed_at=completed_at,
                exit_code=0,
                report_file=None,
                build_only=True,
                debug=debug,
            )
        else:
            # Install the freshly-built extension into the running PostgreSQL
            # so integration tests' CREATE EXTENSION picks up current code, not
            # a stale previously-installed copy.  After install, restart the
            # postmaster: on macOS, fork()d backends inherit the postmaster's
            # dyld image list, so dlopen() returns the *old* cached handle for
            # any dylib the postmaster already had loaded.  A postmaster restart
            # clears that cache so the next CREATE EXTENSION loads the new dylib.
            if mode_details.install_extension:
                _update_entry(mode, step="installing extension")
                rc = _stream_subprocess(
                    ["cmake", "--install", str(active_build_dir),
                     "--component", "bioseq"],
                    log_handle, deadline, cwd=PROJECT_ROOT, env=env,
                )
                if rc != 0:
                    raise RuntimeError(
                        "extension install (cmake --install --component bioseq) "
                        f"exited with code {rc}")
                _restart_postgresql(active_build_dir, log_handle, deadline)

            _update_entry(mode, step="running")
            for cmd in mode_details.commands:
                # MODES.commands store binary BASENAMES; resolve against the
                # active build's bin dir (DOC0270). Args after the first slot
                # are passed through verbatim.
                full_cmd = [str(active_bin_dir / cmd[0]), *cmd[1:]]
                rc = _stream_subprocess(
                    full_cmd, log_handle, deadline,
                    cwd=active_build_dir, env=env,
                )
                if rc != 0:
                    # Bench exits non-zero when regression rules are violated
                    # (exit code 2) — we still want to surface the report path,
                    # so this is a "completed with violations" outcome rather
                    # than a worker failure. The console log already explains
                    # the violations.
                    if mode_details.is_bench_mode and rc == 2:
                        _log(f"bench reported regression violations (exit {rc}); "
                            "treating as completed-with-violations")
                        break
                    raise RuntimeError(f"run failed with exit code {rc}: {' '.join(cmd)}")
            report_path = _newest_report(pre_reports, mode) if mode_details.is_bench_mode else None
            completed_at = time.time()
            _update_entry(
                mode,
                status="completed",
                step="completed",
                completed_at=completed_at,
                exit_code=0,
                report_file=str(report_path) if report_path else None,
                build_only=False,
                debug=debug,
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
            build_only=build_only,
            debug=debug,
        )
        log_handle.write(f"\n# killed by {HARD_TIMEOUT_MIN}-min hard timeout: {e}\n")
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
            build_only=build_only,
            debug=debug,
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
    mode_details = MODES[mode]
    for other_mode in VALID_MODES:
        entry = _refresh(state, other_mode)
        if not entry:
            continue
        if entry.get("status") != "running":
            continue
        if MODES[other_mode].lock_group == mode_details.lock_group:
            return other_mode, entry
    return None, None


def tool_start_build(mode: str, build_only: bool = False,
                     debug: bool = False) -> str:
    if mode not in VALID_MODES:
        return (f"Invalid mode '{mode}'. Valid modes are: "
                f"{', '.join(VALID_MODES)}.")

    mode_details = MODES[mode]

    with _global_lock():
        state = _load_state()
        running_mode, running_entry = _running_in_same_group(state, mode)
        if running_entry:
            elapsed_min = (time.time() - running_entry["started_at"]) / 60.0
            eta_min = max(
                0.0,
                (mode_details.build_only_expected_duration_min if build_only else  mode_details.expected_duration_min) - elapsed_min,
            )
            step = running_entry.get("step", "running")
            return (f"{mode_details.label} {mode_details.verb_is} already scheduled. "
                    f"Current step: {step}. "
                    f"Predicted finish in {_format_minutes(eta_min)}")

        # Reserve the slot atomically before spawning the worker so a
        # concurrent start_build observes "running" immediately and
        # doesn't race past the same check.
        started_at = time.time()
        timestamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(started_at))
        log_path = LOG_DIR / (
            f"bioseq_build_mcp_{mode.replace('+', '_')}_"
            f"{'debug_' if debug else ''}{timestamp}.log"
        )

        server_path = Path(__file__).resolve()
        proc = subprocess.Popen(
            [sys.executable, str(server_path),
             "--run-build", mode,
             "--build-only", ("true" if build_only else "false"),
             "--debug", ("true" if debug else "false")],
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
            "debug": debug,
            "preset": preset_for(debug),
            "build_dir": str(build_dir_for(debug)),
        }
        _save_state(state)
        _log(f"started worker pid={proc.pid} mode={mode} debug={debug} "
             f"log={log_path}")

    return (f"{mode_details.label} {mode_details.verb_is} scheduled "
            f"(preset={preset_for(debug)}). "
            f"Predicted finish in {_format_minutes((mode_details.build_only_expected_duration_min if build_only else  mode_details.expected_duration_min), plural_int=True)}.")


def tool_inspect(arguments: dict) -> str:
    """Implement the `inspect` tool. Returns a single string (a JSON
    document) — the MCP tools/call envelope wraps it in a content item.

    All errors come back as JSON `{"error": "...", ...}` rather than
    raising, so the AI agent always gets a structured result.

    The DOC0270 split means the inspect tool needs DWARF and per-TU temps,
    which only exist in the `build/bench-opt-debug/` tree. We auto-detect:
    if the debug tree has a build.ninja we use it; otherwise we fall back
    to `build/bench-opt/` (which may produce an "no per-TU `.s` files"
    warning if the binary was built without -save-temps). The caller can
    force one or the other via `debug=true|false`.
    """
    function_name = arguments.get("function_name")
    if not isinstance(function_name, str) or not function_name:
        return json.dumps(
            {"error": "function_name is required and must be a non-empty string"},
            indent=2,
        )
    target = arguments.get("target", "bioseq_bench")
    if not isinstance(target, str):
        return json.dumps(
            {"error": f"target must be a string, got {type(target).__name__}"},
            indent=2,
        )

    def _int_arg(name: str, default: int) -> int:
        v = arguments.get(name, default)
        if isinstance(v, bool):  # bool is a subclass of int in Python
            return default
        if isinstance(v, int):
            return v
        if isinstance(v, str) and v.strip().lstrip("-").isdigit():
            return int(v)
        return default

    max_matches = _int_arg("max_matches", 4)
    snippet_lines = _int_arg("snippet_lines", 200)
    max_llc_bc_files = _int_arg("max_llc_bc_files", 4)
    source_basename = arguments.get("source_basename")
    if source_basename is not None and not isinstance(source_basename, str):
        source_basename = None
    try:
        llc_on_bc = parse_bool("llc_on_bc", arguments.get("llc_on_bc", True))
    except RuntimeError as e:
        # parse_bool raises on a malformed string — convert to the JSON
        # error body shape the AI agent expects (per plan §6 error
        # contract) instead of bubbling up as a JSON-RPC -32000.
        return json.dumps({"error": str(e)}, indent=2)

    # Resolve which build tree to inspect. `debug` is an explicit override;
    # without it, prefer the debug tree if it's been built, otherwise fall
    # back to the fast tree. The fast tree does NOT carry per-TU .s/.bc
    # artefacts (DOC0270 dropped -save-temps from bench-opt), so inspecting
    # the fast tree omits the pre-LTO listing and may surface as a
    # top-level warning in the inspect output.
    debug_raw = arguments.get("debug")
    if debug_raw is None:
        debug_chosen = (DEBUG_BUILD_DIR / "build.ninja").exists()
    else:
        try:
            debug_chosen = parse_bool("debug", debug_raw)
        except RuntimeError as e:
            return json.dumps({"error": str(e)}, indent=2)
    inspect_build_dir = build_dir_for(debug_chosen)

    out = _run_inspect_impl(
        function_name=function_name,
        target=target,
        build_dir=inspect_build_dir,
        max_matches=max_matches,
        snippet_lines=snippet_lines,
        source_basename=source_basename,
        llc_on_bc=llc_on_bc,
        max_llc_bc_files=max_llc_bc_files,
    )
    # Surface which tree was inspected so the AI agent can act on
    # "no .s files" results by re-running with debug=true.
    out.setdefault("build_metadata", {})["debug_tree_used"] = debug_chosen
    out["build_metadata"]["preset_used"] = preset_for(debug_chosen)
    return json.dumps(out, indent=2)


def tool_status(mode: str) -> str:
    if mode not in MODES:
        return (f"Invalid mode '{mode}'. Valid modes are: "
                f"{', '.join(VALID_MODES)}.")

    state = _load_state()
    entry = _refresh(state, mode)
    mode_details = MODES[mode]

    if not entry:
        return (f"{mode_details.label} {mode_details.verb_was} never scheduled. "
                f"Use start_build to start a run.")

    log_path = entry.get("log_file") or "(no log file)"
    status = entry.get("status")
    build_only: bool = entry.get("build_only", False)
    debug_used: bool = bool(entry.get("debug", False))
    preset_suffix = (
        f" preset={entry.get('preset', preset_for(debug_used))}"
        if debug_used else ""
    )

    if status == "running":
        elapsed_min = (time.time() - entry["started_at"]) / 60.0
        eta_min = max(0.0, (mode_details.build_only_expected_duration_min if build_only else  mode_details.expected_duration_min) - elapsed_min)
        step = entry.get("step", "running")
        return (f"{mode_details.label} {mode_details.verb_is} already scheduled"
                f"{preset_suffix}. "
                f"Current step: {step}. "
                f"Predicted finish in {_format_minutes(eta_min)}. "
                f"Log: {log_path}")

    completed_at = entry.get("completed_at") or time.time()
    duration_min = (completed_at - entry["started_at"]) / 60.0

    if status == "completed":
        msg = (f"{mode_details.label} {mode_details.verb_was} already completed"
               f"{preset_suffix} "
               f"(total time: {_format_minutes(duration_min, plural_int=True)}). "
               f"Log: {log_path}")
        if entry.get("report_file"):
            msg += f". Report: {entry['report_file']}"
        if entry.get("build_only", False):
            msg += f". This was build-only job so targets were built, but not run (for faster iteration)."
        if debug_used:
            msg += (". This was a debug build (preset=bench-opt-debug); "
                    "DWARF + per-TU .s/.bc artefacts are available under "
                    f"{entry.get('build_dir', str(DEBUG_BUILD_DIR))} for the "
                    "`inspect` tool.")
        return msg

    if status == "killed_timeout":
        return ("Last run was forcefully killed due to timeout. "
                f"Logs for inspection are available here: {log_path}")

    if status == "failed":
        notes = entry.get("notes") or "(no notes)"
        return (f"{mode_details.label} run failed after "
                f"{_format_minutes(duration_min)}: {notes}. "
                f"Log: {log_path}")

    if status == "aborted":
        return (f"{mode_details.label} run was aborted (worker process died "
                f"unexpectedly after {_format_minutes(duration_min)}). "
                f"Log: {log_path}")

    return f"{mode_details.label} status: {status}. Log: {log_path}"


# --------------------------- subagent worker ----------------------------------

def _run_subagent_worker() -> int:
    """Detached worker entry point — invoked via --run-subagent.

    Reads the full prompt and config from SUBAGENT_STATE_FILE (written by
    tool_subagent before spawning this process), runs codex exec, streams
    its JSONL output to the log file, and captures the final message via
    --output-last-message.  Updates state on every significant transition so
    subagent_status always reflects reality.
    """
    state = _load_subagent_state()
    if not state:
        _log("subagent worker: no state entry, exiting")
        return 1

    started_at = float(state["started_at"])
    log_path    = Path(state["log_file"])
    result_path = Path(state["result_file"])
    timeout_sec = float(state.get("timeout_sec", 3600))
    deadline    = started_at + min(timeout_sec, SUBAGENT_HARD_TIMEOUT_SEC)

    prompt            = state.get("prompt", "")
    requested_outputs = state.get("requested_outputs") or []

    outputs_section = _build_requested_outputs_section(requested_outputs)
    full_prompt = (
        _SUBAGENT_PROMPT_TEMPLATE
        .replace("{requested_outputs_section}", outputs_section)
        .replace("{parent_prompt}", prompt)
    )

    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_handle = log_path.open("w", buffering=1)
    log_handle.write(
        f"# bioseq subagent worker — pid={os.getpid()} "
        f"started={time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(started_at))}\n"
        f"# project_root={PROJECT_ROOT}\n"
        f"# timeout_sec={timeout_sec} (hard ceiling {SUBAGENT_HARD_TIMEOUT_SEC/60:.0f} min)\n"
        f"# result_file={result_path}\n"
    )
    log_handle.flush()

    env = os.environ.copy()

    try:
        _update_subagent_entry(step="running")

        cmd = [
            "codex", "exec",
            "--json",
            "--output-last-message", str(result_path),
            "-C", str(PROJECT_ROOT),
            full_prompt,
        ]
        rc = _stream_subprocess(cmd, log_handle, deadline,
                                cwd=PROJECT_ROOT, env=env)

        completed_at = time.time()
        _update_subagent_entry(
            status="completed" if rc == 0 else "failed",
            step="completed"  if rc == 0 else "failed",
            completed_at=completed_at,
            exit_code=rc,
            notes=None if rc == 0 else f"codex exited with code {rc}",
        )
        log_handle.write(
            f"\n# worker exiting (rc={rc}) at "
            f"{time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(completed_at))}\n"
        )
        return 0 if rc == 0 else 1

    except TimeoutError as e:
        completed_at = time.time()
        _update_subagent_entry(
            status="killed_timeout",
            step="killed_timeout",
            completed_at=completed_at,
            exit_code=None,
            notes=str(e),
        )
        log_handle.write(f"\n# killed by hard timeout: {e}\n")
        return 124

    except Exception as e:
        completed_at = time.time()
        _update_subagent_entry(
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


def tool_subagent(prompt: str, timeout_sec: int = 3600,
                  requested_outputs: Optional[List[str]] = None) -> str:
    """Schedule a Codex cross-model subagent (non-blocking).

    Spawns a detached worker process (identical pattern to start_build) and
    returns immediately.  Use subagent_status to poll for completion and read
    the result.  At most one subagent runs at a time; a second call while one
    is running returns the current status instead of starting a duplicate.
    """
    if not prompt or not prompt.strip():
        return "Error: prompt must be a non-empty string."

    with _subagent_global_lock():
        state = _refresh_subagent(_load_subagent_state())
        if state.get("status") == "running":
            elapsed_min = (time.time() - float(state["started_at"])) / 60.0
            return (
                f"A subagent is already running (pid={state.get('pid')}, "
                f"elapsed={elapsed_min:.1f} min). "
                f"Use subagent_status to check progress. "
                f"Log: {state.get('log_file')}"
            )

        started_at = time.time()
        timestamp  = time.strftime("%Y%m%d-%H%M%S", time.localtime(started_at))
        log_path    = LOG_DIR / f"bioseq_subagent_mcp_{timestamp}.log"
        result_path = LOG_DIR / f"bioseq_subagent_mcp_{timestamp}.result.json"

        server_path = Path(__file__).resolve()
        proc = subprocess.Popen(
            [sys.executable, str(server_path), "--run-subagent"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            close_fds=True,
        )

        _save_subagent_state({
            "status":            "running",
            "step":              "spawning",
            "started_at":        started_at,
            "completed_at":      None,
            "pid":               proc.pid,
            "log_file":          str(log_path),
            "result_file":       str(result_path),
            "exit_code":         None,
            "notes":             None,
            "timeout_sec":       timeout_sec,
            "prompt":            prompt,
            "requested_outputs": requested_outputs or [],
            "prompt_preview":    prompt[:200],
        })
        _log(f"started subagent worker pid={proc.pid} log={log_path}")

    return (
        f"Subagent scheduled (pid={proc.pid}). "
        f"Use subagent_status to check progress and retrieve the result. "
        f"Log: {log_path}. Result will be written to: {result_path}"
    )


def tool_subagent_status() -> str:
    """Return the current status of the most recently scheduled subagent."""
    state = _load_subagent_state()
    if not state:
        return "No subagent has been scheduled yet. Use the subagent tool to spawn one."

    state = _refresh_subagent(state)
    status   = state.get("status", "unknown")
    log_path = state.get("log_file", "(no log)")
    result_path = state.get("result_file", "(no result file)")
    preview  = state.get("prompt_preview", "")

    if status == "running":
        elapsed_min = (time.time() - float(state["started_at"])) / 60.0
        eta_min     = max(0.0, float(state.get("timeout_sec", 3600)) / 60.0 - elapsed_min)
        step        = state.get("step", "running")
        return (
            f"Subagent is running (pid={state.get('pid')}, "
            f"elapsed={elapsed_min:.1f} min, ETA ≤{eta_min:.0f} min). "
            f"Step: {step}. Prompt preview: {preview!r}. "
            f"Log: {log_path}"
        )

    started_at   = float(state.get("started_at", time.time()))
    completed_at = float(state.get("completed_at") or time.time())
    duration_min = (completed_at - started_at) / 60.0

    if status == "completed":
        msg = (
            f"Subagent completed in {duration_min:.1f} min. "
            f"Result: {result_path}. Log: {log_path}"
        )
        try:
            result_text = Path(result_path).read_text(encoding="utf-8", errors="replace")
            if len(result_text) <= 8000:
                msg += f"\n\nResult content:\n{result_text}"
            else:
                msg += (f"\n\nResult file is large ({len(result_text)} chars); "
                        f"read {result_path} directly.")
        except OSError:
            msg += "\n\n(result file not found or unreadable)"
        return msg

    if status == "killed_timeout":
        return (
            f"Subagent was killed by the hard timeout after {duration_min:.1f} min. "
            f"Log: {log_path}"
        )

    if status in ("failed", "aborted"):
        notes = state.get("notes") or "(no notes)"
        return (
            f"Subagent {status} after {duration_min:.1f} min: {notes}. "
            f"Log: {log_path}"
        )

    return f"Subagent status: {status}. Log: {log_path}"


# --------------------------- MCP protocol layer ----------------------------

PROTOCOL_VERSION = "2024-11-05"
SERVER_INFO = {"name": "bioseq-build-mcp", "version": "1.1.0"}

TOOL_DEFINITIONS = [
    {
        "name": "start_build",
        "description": (
            "Schedule a build/test/benchmark run. The build runs detached so "
            "it survives MCP server restarts. If a run of the same lock group "
            "is already in progress (can run benchmark together with tests at the same time, but not two benchmarks) "
            "returns the current status instead of starting a duplicate. "
            f"{get_modes_short_documentation()} "
            f"\nNote: If you want to iterate quickly then use build_only parameter."
            f"\n\nBuild tree selection (`debug` parameter, DOC0270):"
            f"\n  - debug=false (DEFAULT): uses the `bench-opt` CMake preset → "
            f"`build/bench-opt/`. -O3 + NDEBUG only, no debug info, no "
            f"per-TU temps. This is the fast everyday build — preferred for "
            f"every normal iteration, including running tests and benches."
            f"\n  - debug=true: uses the `bench-opt-debug` CMake preset → "
            f"`build/bench-opt-debug/`. Same -O3 + NDEBUG, plus -g (DWARF "
            f"symbols) and -save-temps=obj (writes per-TU .s/.ii/.bc beside "
            f"each .o). REQUIRED by the `inspect` tool: it pulls disassembly "
            f"from objdump --line-numbers (needs DWARF) and pre-LTO "
            f"per-TU listings (needs .s files). ~2x slower to compile, "
            f"~2-3x peak RSS per TU, ~3x disk usage versus debug=false. "
            f"Only set this when you intend to call `inspect` afterwards. "
            f"The two build trees are kept on separate disk paths so "
            f"toggling `debug` does not invalidate the other tree's ninja "
            f"graph — switching is cheap once both have been populated once."
            f"\nHard timeout: ~{int(HARD_TIMEOUT_MIN)} min."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "mode": {
                    "type": "string",
                    "enum": list(VALID_MODES),
                    "description": "Which run to start.",
                },
                "build_only": {
                   "type": "boolean",
                   "description": "Do you want only to build the target, but do not actually run any commands (like benchmarks, tests etc.). Useful for faster iteration to check if the code just compiles correctly.",
                   "default": False,
                },
                "debug": {
                   "type": "boolean",
                   "description": (
                       "Select the debug build tree (preset=bench-opt-debug, "
                       "dir=build/bench-opt-debug/). Adds -g + -save-temps=obj "
                       "so the `inspect` tool can extract per-function "
                       "disassembly and pre-LTO assembly listings. ~2x slower "
                       "and uses ~3x more memory per TU than the default "
                       "fast build — only set this when you actually need "
                       "the inspect artefacts. Default false (uses the "
                       "everyday `bench-opt` preset)."
                   ),
                   "default": False,
                }
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
    {
        "name": "inspect",
        "description": (
            "Inspect a function in the latest build. Returns a "
            "JSON document with: demangled symbols matching the substring; "
            "the linked-binary disassembly per match (post-LTO ground "
            "truth); the per-TU compiler-emitted .s listing block (pre-LTO "
            "compiler intent, with .loc source-line annotations); an "
            "instruction histogram (total / SIMD / branches / loads / "
            "stores / atomic_loads / atomic_stores / neon_struct_loadstore); "
            "and a sweep of inline sites where the function appears as "
            ".loc directives or DEBUG_VALUE annotations in other TUs' .s "
            "files (useful for [[gnu::always_inline]] functions that have "
            "no standalone symbol). All large outputs are written to "
            "/tmp/bioseq_inspect/ and pointed at via paths so the AI can "
            "Read them on demand.\n\n"
            "DOC0270 — build-tree selection: the default `bench-opt` build "
            "tree does NOT carry per-TU .s/.bc files or DWARF (DOC0270 "
            "dropped -g + -save-temps from that preset to halve per-TU "
            "compile memory). Call `start_build` with debug=true first to "
            "produce a `bench-opt-debug` tree that has both. If you pass "
            "no `debug` argument here, the tool auto-detects: the debug "
            "tree wins when its build.ninja exists, otherwise we fall "
            "back to the fast tree (the response's "
            "`build_metadata.debug_tree_used` field records which one was "
            "used; if it is false and you see 'no per-TU `.s` files' "
            "warnings, schedule a `start_build mode=… debug=true` and "
            "re-run inspect).\n\n"
            "Use this to compare two implementations of the same function "
            "(e.g. before/after a refactor) at the instruction level to "
            "spot vectorisation regressions, register spills, "
            "instruction-count deltas, branch-pattern changes, etc.\n\n"
            "Recommended: pass a fully-qualified namespace prefix as "
            "`function_name` (e.g. "
            "'void bioseq::simd::N_NEON::collect_minimizers_two_stack_nthash_simd@') "
            "rather than the bare function name; bare names like 'hash' "
            "match thousands of unrelated symbols (std::hash, "
            "boost::hash, lambdas, etc.). The `summary.total_matches_before_cap` "
            "field surfaces the un-capped count so you can decide whether "
            "to retry with a tighter prefix; a top-level warning fires "
            f"when the count exceeds {MATCHES_WARN_THRESHOLD}.\n\n"
            "Latency: lazy disk-cached llc fallback. First call after a "
            "fresh bench-opt build is ~25-100s (one llc per relevant .bc, "
            "capped at 4). Subsequent calls reuse the cached .s and "
            "complete in ~6-9s. Cache invalidates on .bc mtime; rebuilds "
            "cleanly reset it.\n\n"
            "Caveats:\n"
            "  - llvm-objdump symbolises every branch target by finding "
            "the closest *named* symbol with a smaller address — even when "
            "the offset is huge (e.g. `bl 0x1004f5ec0 <_write+0x1004f5ec0>` "
            "is NOT a call to _write, just the closest libsystem anchor). "
            "Treat `<symbol+offset>` annotations with suspicion when the "
            "offset is > 0x10000.\n"
            "  - macOS+ThinLTO drops inline-frame source line info, so the "
            "post-LTO disassembly's --line-numbers output is sparse. The "
            "pre-LTO `.s` view (assembly_listing_*) preserves source "
            "mapping via .loc directives.\n"
            "  - At clang -O3 the default -discard-value-names strips "
            "DEBUG_VALUE annotations for templated/inlined code, so "
            "inline_sites[].debug_value_hits is often 0. The .loc "
            "directive count is the primary signal."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "function_name": {
                    "type": "string",
                    "description":
                        "Substring matched against demangled C++ names. "
                        "Examples: "
                        "'void bioseq::simd::N_NEON::collect_minimizers_two_stack_nthash_simd@' "
                        "(prefix-anchored, recommended), or "
                        "'collect_minimizers_two_stack_nthash_simd' "
                        "(broader, may match many lambdas/closures).",
                },
                "target": {
                    "type": "string",
                    "enum": ["bioseq_bench", "bioseq_io_bench",
                             "bioseq_aligner_tests_exec",
                             "bioseq_aligner_int_tests_exec"],
                    "default": "bioseq_bench",
                    "description":
                        "Which target binary to inspect under "
                        "main/build/bench-opt/bioseq/.",
                },
                "max_matches": {
                    "type": "integer",
                    "default": 4,
                    "minimum": 1,
                    "description":
                        "Cap on the number of returned symbols. Largest "
                        "first (by computed size).",
                },
                "snippet_lines": {
                    "type": "integer",
                    "default": 200,
                    "minimum": 1,
                    "description":
                        "How many lines of asm/disasm to inline in the "
                        "JSON. Full output is always written to /tmp and "
                        "the path is included; use Read on the path for "
                        "the rest.",
                },
                "source_basename": {
                    "type": "string",
                    "description":
                        "Basename of the source file (e.g. "
                        "'two_stacks_minimizers.cppm') used to filter "
                        "the inline-site .loc sweep. Defaults to "
                        "'<function_name>.cppm', which is usually wrong "
                        "for namespaced functions — pass it explicitly "
                        "for accurate inline_sites[].",
                },
                "max_llc_bc_files": {
                    "type": "integer",
                    "default": 4,
                    "minimum": 0,
                    "description":
                        "Cap on the number of .bc files we llc-compile "
                        "for the pre-LTO view when no .s is present. Each "
                        "conversion costs ~25s on the bench-main TU. "
                        "Raise to widen the search; set to 0 to disable.",
                },
                "llc_on_bc": {
                    "type": "boolean",
                    "default": True,
                    "description":
                        "Whether to run the llc-on-.bc fallback when no "
                        ".s files are present (clang ThinLTO case). "
                        "Disable for fast/incomplete inspect calls.",
                },
                "debug": {
                    "type": "boolean",
                    "description":
                        "Force the inspect tool to read from a specific "
                        "build tree (DOC0270). true → build/bench-opt-debug/ "
                        "(needs `start_build debug=true` to have populated "
                        "it). false → build/bench-opt/ (the everyday tree; "
                        "no per-TU .s/.bc, no DWARF). Omit to auto-detect: "
                        "prefer the debug tree when available, fall back "
                        "to fast. Set this explicitly only when you need "
                        "to pin the tree (e.g. compare debug vs fast).",
                },
            },
            "required": ["function_name"],
        },
    },
    {
        "name": "subagent",
        "description": (
            "CROSS-MODEL SUBAGENT ONLY — this tool is specifically for delegating work to a "
            "different AI model (Codex). For same-model subtasks use the built-in Agent tool "
            "instead; reserve this tool for hard tasks that genuinely benefit from an independent "
            "model's perspective (e.g. deep refactors, second-opinion code reviews, complex "
            "multi-file edits).\n\n"
            "Schedules a detached Codex subagent (returns immediately — mirrors the start_build "
            "pattern). At most one subagent runs at a time; calling while one is already running "
            "returns the current status instead of starting a duplicate. "
            "Use subagent_status to poll for completion and read the result.\n\n"
            "The subagent: (1) reads the project coding guidelines "
            "(0321_BIOSEQ_CODE_AND_WORKFLOW_GUIDELINES.md), (2) executes the given task in full "
            "(non-interactive, no deferral), (3) self-reviews via a nested sub-subagent until "
            "saturation, and (4) writes a structured JSON result file containing:\n"
            "  task_status  — detailed completion summary\n"
            "  bugs         — bugs surfaced during the task\n"
            "  risks        — risks to flag to the parent agent\n"
            "  changed_files — list of files modified\n"
            "  review_tldr  — key takeaways from the self-review pass\n"
            "  output       — one entry per requested_output item, in order"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "prompt": {
                    "type": "string",
                    "description": (
                        "The task prompt to send to the Codex subagent. Write it as you "
                        "would a detailed ticket: include context, goals, acceptance criteria, "
                        "and any file paths or symbols the subagent should focus on."
                    ),
                },
                "requested_outputs": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": (
                        "Optional list of named outputs you want the subagent to produce "
                        "in the `output` array of its JSON response. Each string is a "
                        "free-form request — e.g. 'review feedback summary', "
                        "'describe the most critical bug found', 'list all changed APIs'. "
                        "The subagent fills one `output` slot per item, in the same order. "
                        "Omit or pass [] to leave `output` empty."
                    ),
                },
                "timeout_sec": {
                    "type": "integer",
                    "default": 3600,
                    "minimum": 60,
                    "maximum": 7200,
                    "description": "Hard timeout in seconds (default 3600 = 60 min).",
                },
            },
            "required": ["prompt"],
        },
    },
    {
        "name": "subagent_status",
        "description": (
            "Report the status of the most recently scheduled subagent. "
            "While running, returns current step, elapsed time, and ETA. "
            "After completion, returns the full result content (if ≤ 8 KB) "
            "or the path to the result file for larger outputs, plus the "
            "log file path for detailed JSONL event inspection."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {},
            "required": [],
        },
    },
]

# --------------------------- document tools (Goal C) -----------------------
# Four QMD-backed document-management tools. Their request bodies are JSON-Schema-validated
# server-side in docs/schemas.py (the inputSchema dicts below mirror those schemas and are the
# AI-facing contract). See main/DOCS/README.md.
TOOL_DEFINITIONS += [
    {
        "name": "doc_create",
        "description": (
            "Create a new project document under main/DOCS/CURRENT/<TYPE>/ with consistent "
            "frontmatter and an auto-allocated DOC id (max existing + 1). The file is "
            "immediately FTS-indexed into QMD (vector embeddings refresh in the background) so "
            "doc_search finds it. Returns the absolute path — open it with Read and write the "
            "body. Routing: bench topics -> BENCHMARK_FRAMEWORK, bugfix -> BUGFIX, else by type. "
            "Topics are validated against main/DOCS/.topics unless allow_new_topic=true."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "type": {
                    "type": "string",
                    "enum": ["guidelines", "human_notes", "tech_spec", "research", "report",
                             "impl_plan", "prompt", "external_doc", "bugfix"],
                    "description": "Document type (decides directory + frontmatter).",
                },
                "title": {"type": "string",
                          "description": "Human title; becomes the filename slug."},
                "topics": {"type": "array", "items": {"type": "string"},
                           "description": "1+ topic tags (closed vocabulary in .topics)."},
                "summary": {"type": "string", "description": "<=400 char one-line summary."},
                "origin": {"type": "string",
                           "enum": ["ai_output", "human_prompt", "external_dump",
                                    "human_notes", "mixed"], "default": "ai_output"},
                "supersedes": {"type": "array", "items": {"type": "string"},
                               "description": "DOC ids this replaces, e.g. [\"DOC0310\"]."},
                "allow_new_topic": {"type": "boolean", "default": False,
                                    "description": "Permit topics not yet in .topics."},
            },
            "required": ["type", "title", "topics"],
        },
    },
    {
        "name": "doc_archive",
        "description": (
            "Archive a document: move it from main/DOCS/CURRENT/ to main/DOCS/ARCHIVE/ "
            "(preserving the <TYPE>/ subdir), set `archived: true` (and status: archived unless "
            "superseded) in its frontmatter, and refresh the QMD index. `doc` accepts a DOC id "
            "(DOC0123 or 123), an exact filename, or a unique path substring."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {"doc": {"type": "string",
                           "description": "DOC id, filename, or unique path fragment."}},
            "required": ["doc"],
        },
    },
    {
        "name": "doc_search",
        "description": (
            "Search project documents via QMD hybrid retrieval (BM25 + vector) + LLM reranking "
            "over a warm local model server. Returns the BEST hit's absolute path + the "
            "most-relevant text fragment, plus, for the other hits, their metadata (path, "
            "doc_id, title, context, score). Searches CURRENT by default; set "
            "include_archived=true to also search ARCHIVE."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string",
                          "description": "Natural-language or keyword query."},
                "limit": {"type": "integer", "default": 10, "minimum": 1, "maximum": 50},
                "min_score": {"type": "number", "default": 0.0, "minimum": 0, "maximum": 1},
                "include_archived": {"type": "boolean", "default": False},
                "intent": {"type": "string",
                           "description": "Optional disambiguation context."},
            },
            "required": ["query"],
        },
    },
    {
        "name": "doc_read",
        "description": (
            "Resolve a document selector (DOC id, filename, or path fragment) to its absolute "
            "on-disk path plus metadata, so you can open it with the Read tool. Does not return "
            "the body itself."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {"doc": {"type": "string",
                           "description": "DOC id, filename, or unique path fragment."}},
            "required": ["doc"],
        },
    },
]


_OUTPUT_FRAMING = "content-length"


def _write_message(msg: dict) -> None:
    body = json.dumps(msg, separators=(",", ":")).encode("utf-8")
    if _OUTPUT_FRAMING == "newline":
        sys.stdout.write(body.decode("utf-8") + "\n")
        sys.stdout.flush()
        return

    sys.stdout.buffer.write(
        f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body
    )
    sys.stdout.buffer.flush()


def _parse_jsonrpc_body(body: bytes, source: str) -> dict:
    try:
        return json.loads(body.decode("utf-8"))
    except json.JSONDecodeError as e:
        _log(f"malformed JSON-RPC {source} dropped: {e}")
        return {"_dropped": True}


def _read_message() -> dict | None:
    """Read either standard MCP Content-Length frames or legacy JSON lines."""
    global _OUTPUT_FRAMING

    stdin = sys.stdin.buffer
    first = stdin.read(1)
    while first in (b"\r", b"\n"):
        first = stdin.read(1)
    if not first:
        return None

    if first == b"{":
        _OUTPUT_FRAMING = "newline"
        line = first + stdin.readline()
        return _parse_jsonrpc_body(line.strip(), "line")

    _OUTPUT_FRAMING = "content-length"
    header_lines = [first + stdin.readline()]
    while True:
        line = stdin.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break
        header_lines.append(line)

    content_length = None
    for line in header_lines:
        decoded = line.decode("ascii", errors="replace").strip()
        name, sep, value = decoded.partition(":")
        if sep and name.lower() == "content-length":
            try:
                content_length = int(value.strip())
            except ValueError:
                _log(f"invalid Content-Length dropped: {value.strip()!r}")
                return {"_dropped": True}
            break

    if content_length is None:
        header = b"".join(header_lines).decode("ascii", errors="replace").strip()
        _log(f"missing Content-Length header dropped: {header!r}")
        return {"_dropped": True}

    body = stdin.read(content_length)
    if len(body) != content_length:
        _log(f"short JSON-RPC body dropped: got {len(body)}, expected {content_length}")
        return None

    return _parse_jsonrpc_body(body, "frame")


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

def parse_bool(parameter_description: str, value: Any) -> bool:
    if value is None:
        return False
    if isinstance(value, str):
        value = value.lower()
        if value not in ['true', 'false', 'y', 'n']:
            raise RuntimeError(f"Value passed as {parameter_description} is not a valid bool. Got string '{value}' which is neither 'true' or 'false'")
        return (value == "true" or value == "y")
    if type(value) == bool:
        return value
    raise RuntimeError(f"Value passed as {parameter_description} is unsupported type '{type(value)}' (with value: '{value}')")

def _handle_tools_call(params: dict) -> dict:
    name = params.get("name")
    args = params.get("arguments") or {}
    if name == "start_build":
        mode = args.get("mode", "")
        build_only: bool = parse_bool("build_only", args.get("build_only", False))
        debug: bool = parse_bool("debug", args.get("debug", False))
        text = tool_start_build(mode, build_only=build_only, debug=debug)
    elif name == "status":
        mode = args.get("mode", "")
        text = tool_status(mode)
    elif name == "inspect":
        text = tool_inspect(args)
    elif name == "subagent":
        prompt_text = args.get("prompt", "")
        timeout_sec = int(args.get("timeout_sec", 3600))
        requested_outputs = args.get("requested_outputs") or []
        if not isinstance(requested_outputs, list):
            requested_outputs = []
        text = tool_subagent(prompt_text, timeout_sec=timeout_sec,
                             requested_outputs=requested_outputs)
    elif name == "subagent_status":
        text = tool_subagent_status()
    elif name in ("doc_create", "doc_archive", "doc_search", "doc_read"):
        # Doc-tool requests are JSON-Schema-validated server-side inside operations
        # (docs/schemas.py). ensure_doc_env() lazily provisions qmd + jsonschema on first use.
        bootstrap.ensure_doc_env()
        from docs import operations
        text = getattr(operations, name)(args)
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
    parser.add_argument(
        "--build-only",
        choices=['true', 'false'],
        default='false',
        help="(internal) if 'true' then build will only compile target and not run any actual executables like benchmarks or tests",
    )
    parser.add_argument(
        "--debug",
        choices=['true', 'false'],
        default='false',
        help=(
            "(internal) if 'true' then build under build/bench-opt-debug/ "
            "with the `bench-opt-debug` CMake preset (adds -g + "
            "-save-temps=obj). Required for the `inspect` tool. Default "
            "'false' uses the lean `bench-opt` preset (DOC0270)."
        ),
    )
    parser.add_argument(
        "--run-subagent",
        action="store_true",
        default=False,
        help=(
            "(internal) run as a detached subagent worker; reads prompt and "
            "config from SUBAGENT_STATE_FILE and invokes codex exec"
        ),
    )
    args = parser.parse_args()
    if args.run_subagent:
        return _run_subagent_worker()
    if args.run_build is not None:
        build_only: bool = parse_bool("build_only", args.build_only)
        debug: bool = parse_bool("debug", args.debug)
        return _run_build_worker(args.run_build, build_only=build_only,
                                 debug=debug)

    # Non-blocking autostart warm: provision the doc env and bring qmd (collections + HTTP
    # server + background index) online without blocking the MCP handshake or the stdlib build
    # tools. This also makes "keep QMD collections up to date periodically" run even in sessions
    # that never call a doc tool. Disable with BIOSEQ_DOCS_AUTOSTART=0.
    if os.environ.get("BIOSEQ_DOCS_AUTOSTART", "1") == "1":
        def _warm() -> None:
            try:
                bootstrap.ensure_doc_env()
                from docs.qmd_manager import get_manager
                get_manager().ensure_ready()
            except Exception as e:
                print(f"[bioseq-build-mcp] autostart warm skipped: {e}",
                      file=sys.stderr, flush=True)
        threading.Thread(target=_warm, daemon=True).start()

    _serve()
    return 0


if __name__ == "__main__":
    sys.exit(main())
