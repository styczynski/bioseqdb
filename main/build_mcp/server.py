#!/usr/bin/env python3
"""bioseq build MCP server.

Speaks MCP (JSON-RPC 2.0 over stdio) with zero external dependencies — only
the Python stdlib. Exposes two tools:

    start_build(mode)    schedule a build/test/bench run
    status(mode)         report progress, predicted finish, log + report paths

For modes list see MODES dict.

Concurrency rules:
    - At most one run per "lock group" at a time.
    - "tests"  group covers unit+integration and external-api.
    - "bench"  group covers bench and bench-fast (they share the binary and
      the reporting/ output directory, so concurrent runs would race).
    - Different lock groups can run in parallel unless both runs install/restart
      the shared PostgreSQL extension (unit+integration conflicts with bench-e2e).

Architecture:
    - Run with no args → MCP server on stdin/stdout.
    - Run with `--run-build <mode>` → detached build worker (this is what the
      server spawns; the worker is *not* meant to be invoked manually).
    - Build state lives in /tmp/bioseq_build_mcp_state.json.
    - Per-run logs live in /tmp/bioseq_build_mcp_<mode>_<timestamp>.log.
    - Builds are detached via start_new_session=True so they survive Claude
      Code session restarts (a 25-min bench is too long to lose to a closed
      window). The worker self-enforces a configurable hard timeout.

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
import shlex
import shutil
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

# DOC0270 plus test-hook isolation — parallel production/test build families:
#
#   "bench-opt"        — default production/benchmark fast tree. -O3 + NDEBUG,
#                        no -g, no -save-temps. Lives under build/bench-opt/.
#   "test-opt"         — matching isolated unit/integration tree, with guarded
#                        failure hooks and test-only indexes enabled.
#   "bench-opt-debug"  — heavier, slower. Same -O3 + NDEBUG, plus -g (DWARF)
#                        and -save-temps=obj (per-TU .s/.ii/.bc artifacts).
#                        Selected with debug=true on start_build. Required by
#                        the `inspect` tool — it pulls disassembly from the
#                        objdump --line-numbers output (needs DWARF) and the
#                        pre-LTO assembly view (needs the per-TU .s files).
#                        Lives under build/bench-opt-debug/ so it does not
#                        invalidate the default tree's ninja graph.
#   "test-opt-debug"   — matching isolated test debug/inspect tree.
#
# The preset families share the rest of the toolchain (vcpkg, arm64-osx target).
# The build dir is selected per-call from mode + `debug`; state records which
# preset each run used so `status` and `inspect` can find the right artifacts.
# The preset names are overridable via environment so the SAME server.py can
# drive different toolchains depending on where it runs. This is what lets the
# remote-run tool reuse this exact build worker on the x86_64 AWS box: there the
# box-side server.py is invoked with BIOSEQ_BUILD_PRESET=aws-x64-linux, so the
# worker configures/builds build/aws-x64-linux/ (the Linux/libc++/x64-linux-triplet
# tree the verify.sh provisioning already populated) instead of the macOS/arm64
# bench-opt tree. On a normal local checkout the env vars are unset and these
# fall back to the everyday DOC0270 presets. The build dir always derives from the
# preset name (every CMakePresets entry uses binaryDir ${sourceDir}/build/${presetName}),
# so overriding the preset name is sufficient — no separate dir override needed.
DEFAULT_BUILD_PRESET = os.environ.get("BIOSEQ_BUILD_PRESET", "bench-opt")


def _default_debug_preset(production_preset: str) -> str:
    mapped = {
        "bench-opt": "bench-opt-debug",
        "aws-x64-linux": "aws-x64-linux-debug",
        "aws-x64-linux-gcc161": "aws-x64-linux-gcc161-debug",
    }.get(production_preset)
    if mapped is not None:
        return mapped
    if production_preset.endswith("-debug"):
        return production_preset
    return f"{production_preset}-debug"


DEBUG_BUILD_PRESET = os.environ.get(
    "BIOSEQ_BUILD_PRESET_DEBUG",
    _default_debug_preset(DEFAULT_BUILD_PRESET),
)


def _default_test_preset(production_preset: str) -> str:
    mapped = {
        "bench-opt": "test-opt",
        "bench-opt-debug": "test-opt-debug",
        "aws-x64-linux": "test-aws-x64-linux",
        "aws-x64-linux-debug": "test-aws-x64-linux-debug",
        "aws-x64-linux-gcc161": "test-aws-x64-linux-gcc161",
        "aws-x64-linux-gcc161-debug":
            "test-aws-x64-linux-gcc161-debug",
    }.get(production_preset)
    if mapped is not None:
        return mapped
    if production_preset.startswith("test-"):
        return production_preset
    # Custom production presets follow the same fail-closed naming contract.
    # If test-<preset> is not declared, configure fails instead of silently
    # compiling guarded tests against the production tree.
    return f"test-{production_preset}"


TEST_BUILD_PRESET = os.environ.get(
    "BIOSEQ_TEST_BUILD_PRESET",
    _default_test_preset(DEFAULT_BUILD_PRESET),
)
TEST_DEBUG_BUILD_PRESET = os.environ.get(
    "BIOSEQ_TEST_BUILD_PRESET_DEBUG",
    _default_test_preset(DEBUG_BUILD_PRESET),
)
EXTERNAL_API_BUILD_PRESET = os.environ.get(
    "BIOSEQ_EXTERNAL_API_BUILD_PRESET", "aws-x64-linux-gcc161")
DEFAULT_BUILD_DIR = PROJECT_ROOT / "build" / DEFAULT_BUILD_PRESET
DEBUG_BUILD_DIR = PROJECT_ROOT / "build" / DEBUG_BUILD_PRESET
REPORTING_DIR = PROJECT_ROOT / "reporting"


def preset_for(debug: bool) -> str:
    return DEBUG_BUILD_PRESET if debug else DEFAULT_BUILD_PRESET


def build_dir_for(debug: bool) -> Path:
    return DEBUG_BUILD_DIR if debug else DEFAULT_BUILD_DIR


def bin_dir_for(debug: bool) -> Path:
    return build_dir_for(debug) / "bioseq"


def preset_for_mode(mode: str, debug: bool) -> str:
    # A mode may pin an explicit preset (e.g. external-api → GCC16 toolchain);
    # an override wins outright and is not split into debug/production families.
    mode_details = MODES.get(mode)
    if mode_details is not None and mode_details.preset_override:
        return mode_details.preset_override
    # unit+integration builds against the isolated test preset family so the
    # guarded failure hooks / test-only indexes never leak into a production
    # (benchmark) tree.
    if mode == "unit+integration":
        selected = TEST_DEBUG_BUILD_PRESET if debug else TEST_BUILD_PRESET
        production = preset_for(debug)
        if selected == production:
            raise RuntimeError(
                "unit+integration requires an isolated test preset; "
                f"selected test preset equals production preset {production!r}. "
                "Set BIOSEQ_TEST_BUILD_PRESET[_DEBUG] to a distinct preset "
                "whose BIOSEQ_TEST_INDEXES cache value is ON.")
        return selected
    return preset_for(debug)


def build_dir_for_mode(mode: str, debug: bool) -> Path:
    return PROJECT_ROOT / "build" / preset_for_mode(mode, debug)


def bin_dir_for_mode(mode: str, debug: bool) -> Path:
    return build_dir_for_mode(mode, debug) / "bioseq"


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
    build edges in the historical 35-min hard-timeout window). Budgeting ~3 GB per
    concurrent heavy compile keeps cold builds making forward progress without
    paging. On this 8-core / 16 GB machine this yields -j 5."""
    cpus = os.cpu_count() or 4
    try:
        mem_gb = (os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")) / (1024 ** 3)
    except (ValueError, OSError, AttributeError):
        mem_gb = 16.0
    return max(2, min(cpus, int(mem_gb // 3)))


CMAKE_PARALLEL_JOBS = _safe_parallel_jobs()
HARD_TIMEOUT_MIN = float(os.environ.get("BIOSEQ_BUILD_HARD_TIMEOUT_MIN", "240.0"))
HARD_TIMEOUT_SEC = HARD_TIMEOUT_MIN * 60.0
AWS_HEAVY_JOBS = "4"
AWS_GCC_TYPED_DISPATCH_OPT_LEVEL = "O1"
AWS_GCC_TYPED_DISPATCH_JOBS = "1"
AWS_GCC161_CXX_FLAGS = (
    "-O3 -DNDEBUG -fno-omit-frame-pointer -fpermissive -fabi-version=0 "
    "-Wno-expose-global-module-tu-local -Wno-external-tu-local "
    "-Wno-template-names-tu-local -Wno-global-module -Wno-template-body"
)
AWS_LIBCXX_VCPKG_TRIPLET = "x64-linux-libcxx"
AWS_LEGACY_VCPKG_TRIPLET = "x64-linux"
AWS_LIBCXX_OVERLAY_TRIPLETS = PROJECT_ROOT / "cmake" / "triplets"
AWS_LIBCXX_OVERLAY_PORTS = PROJECT_ROOT / "cmake" / "ports"
AWS_LIBCXX_FIND_CACHE_UNSET_PATTERNS = (
    "OPENSSL*",
    "_OPENSSL*",
    "OpenSSL*",
    "PostgreSQL*",
    "FIND_PACKAGE_MESSAGE_DETAILS_OpenSSL",
    "FIND_PACKAGE_MESSAGE_DETAILS_PostgreSQL",
)


def _cmake_cache_value(build_dir: Path, key: str) -> str | None:
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return None
    prefix = f"{key}:"
    try:
        with cache.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith(prefix):
                    return line.split("=", 1)[1].strip()
    except OSError:
        return None
    return None


def _cmake_cache_bool(build_dir: Path, key: str) -> bool | None:
    value = _cmake_cache_value(build_dir, key)
    if value is None:
        return None
    normalized = value.strip().upper()
    if normalized in {"1", "ON", "TRUE", "YES", "Y"}:
        return True
    if normalized in {"0", "OFF", "FALSE", "NO", "N", "IGNORE", "NOTFOUND"}:
        return False
    return None


def _cmake_cache_contains(build_dir: Path, needle: str) -> bool:
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return False
    try:
        return needle in cache.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False


def _ensure_local_toolchain_environment(
        env: dict[str, str], active_build_dir: Path) -> None:
    """Make a fresh local preset configurable from a detached MCP worker.

    MCP servers are commonly launched without the interactive shell's
    ``VCPKG_ROOT`` even though an existing build tree was configured with it.
    Reuse that recorded toolchain location first, then fall back to the usual
    ``~/vcpkg`` checkout.  AWS presets provision their own environment and do
    not use this local repair.
    """
    if env.get("VCPKG_ROOT"):
        return

    candidates: list[Path] = []
    for build_dir in (active_build_dir, DEFAULT_BUILD_DIR):
        configured_toolchain = _cmake_cache_value(
            build_dir, "CMAKE_TOOLCHAIN_FILE")
        if configured_toolchain:
            toolchain = Path(configured_toolchain).expanduser()
            if toolchain.name == "vcpkg.cmake" and len(toolchain.parents) >= 3:
                candidates.append(toolchain.parents[2])
    candidates.append(Path.home() / "vcpkg")

    for candidate in candidates:
        if (candidate / "scripts" / "buildsystems" / "vcpkg.cmake").is_file():
            env["VCPKG_ROOT"] = str(candidate)
            return


def _clear_cmake_configure_cache(build_dir: Path, log_handle, reason: str) -> None:
    log_handle.write(f"# CMake cache repair: clearing configure cache ({reason}).\n")
    log_handle.flush()
    for path in (build_dir / "CMakeCache.txt", build_dir / "CMakeFiles"):
        try:
            if path.is_dir() and not path.is_symlink():
                shutil.rmtree(path)
            elif path.exists() or path.is_symlink():
                path.unlink()
        except OSError as e:
            raise RuntimeError(f"failed to remove stale CMake cache path {path}: {e}") from e

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
    grouped together; unit+integration and external-api share the "tests"
    group. Runtime modes that install/restart the shared PostgreSQL instance
    also conflict across lock groups, while build-only runs remain independent.
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
    at run time (see `bin_dir_for_mode(mode, debug)`). This lets the same MODE
    definition drive isolated production and test preset families, including
    their debug/inspect variants.
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
    """Optional dedicated CMake preset instead of the server-wide preset."""
    preset_override: Optional[str] = None
    """Cache entries that must be set before this mode's targets exist."""
    required_cache_variables: Optional[Dict[str, str]] = None

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
    "external-api": ModeDefinition(
        lock_group="tests",
        expected_duration_min=8.0,
        build_only_expected_duration_min=5.0,
        label="External API integration tests",
        docs="Build and run generated external API clients without sequence or PostgreSQL targets",
        is_verb_plular=True,
        targets=["bioseq_external_integration_tests_exec"],
        commands=[
            ["bioseq_external_integration_tests_exec",
             "--reporter", "console::out=-::colour-mode=ansi"],
        ],
        preset_override=EXTERNAL_API_BUILD_PRESET,
        required_cache_variables={"BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "ON"},
    ),
    "unit+integration": ModeDefinition(
        lock_group="tests",
        expected_duration_min=120.0,
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


def _is_aws_clang_preset(preset: str) -> bool:
    normalized = preset.removeprefix("test-")
    return (
        normalized.startswith("aws-x64-linux")
        and "gcc161" not in normalized
    )


def _is_aws_gcc_preset(preset: str) -> bool:
    normalized = preset.removeprefix("test-")
    return normalized.startswith("aws-x64-linux-gcc161")


def _is_aws_preset(preset: str) -> bool:
    return _is_aws_clang_preset(preset) or _is_aws_gcc_preset(preset)


def _is_debug_preset(preset: str) -> bool:
    return preset.removeprefix("test-").endswith("-debug")


def _cmake_install_cmd(active_build_dir: Path, active_preset: str) -> list[str]:
    cmake = shutil.which("cmake") or "cmake"
    cmd = [cmake, "--install", str(active_build_dir), "--component", "bioseq"]
    if _is_aws_preset(active_preset):
        return ["sudo", "-n", *cmd]
    return cmd


def _prepare_aws_postgresql_test_database(active_preset: str, log_handle,
                                          deadline: float) -> None:
    """Make the AWS PostgreSQL instance match integration-test defaults."""
    if not _is_aws_preset(active_preset):
        return

    log_handle.write(
        "\n# Preparing AWS PostgreSQL test auth/database "
        "(postgres/postgres, database bioseq) ...\n")
    log_handle.flush()

    psql = shutil.which("psql") or "psql"
    rc = _stream_subprocess(
        ["sudo", "-n", "-u", "postgres", psql,
         "-v", "ON_ERROR_STOP=1",
         "--dbname", "postgres",
         "--command", "ALTER ROLE postgres WITH LOGIN PASSWORD 'postgres';"],
        log_handle,
        deadline,
    )
    if rc != 0:
        raise RuntimeError(
            f"PostgreSQL test role setup exited with code {rc}")

    create_db_script = (
        "set -euo pipefail\n"
        "if ! psql -Atq --dbname=postgres "
        "--command \"SELECT 1 FROM pg_database WHERE datname = 'bioseq';\" "
        "| grep -qx 1; then\n"
        "  createdb --owner=postgres bioseq\n"
        "fi\n"
    )
    rc = _stream_subprocess(
        ["sudo", "-n", "-u", "postgres", "bash", "-lc", create_db_script],
        log_handle,
        deadline,
    )
    if rc != 0:
        raise RuntimeError(
            f"PostgreSQL test database setup exited with code {rc}")


def _run_postgresql_restart_cmd(restart_cmd: list[str], log_handle) -> None:
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
            msg = (
                f"PostgreSQL restart exited {result.returncode}; refusing to "
                "run with a potentially stale bioseq.dylib")
            _log(f"ERROR: {msg}")
            raise RuntimeError(msg)
        _log("PostgreSQL restarted — new dylib is now live")
    except subprocess.TimeoutExpired as exc:
        msg = "PostgreSQL restart timed out after 90s; refusing to run with a potentially stale bioseq.dylib"
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg) from exc


def _restart_postgresql(active_build_dir: Path, active_preset: str,
                        log_handle, deadline: float) -> None:
    """Restart the PostgreSQL postmaster after installing a new extension dylib.

    On macOS, fork()d backend processes inherit the postmaster's dyld image
    list.  When the postmaster has an older bioseq.dylib already mapped,
    dlopen("bioseq.dylib") in every fork'd child returns the old cached handle
    — dlsym() then fails to find functions added in the new build.  A
    postmaster restart evicts the stale mapping; the first CREATE EXTENSION
    after restart loads the freshly installed dylib.
    """
    if sys.platform != "darwin" and _is_aws_preset(active_preset):
        systemctl = shutil.which("systemctl")
        if systemctl:
            _log("Restarting PostgreSQL via systemd service to evict stale dylib cache ...")
            _run_postgresql_restart_cmd(
                ["sudo", "-n", systemctl, "restart", "postgresql"],
                log_handle,
            )
            return

    cmake_cache = active_build_dir / "CMakeCache.txt"
    pg_config: str | None = None
    if cmake_cache.exists():
        for line in cmake_cache.read_text(errors="replace").splitlines():
            if line.startswith("PostgreSQL_CONFIG:FILEPATH="):
                pg_config = line.split("=", 1)[1].strip()
                break
    if not pg_config:
        msg = "PostgreSQL_CONFIG not in CMakeCache.txt; cannot restart PG after installing extension"
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg)

    try:
        res = subprocess.run([pg_config, "--bindir"],
                             capture_output=True, text=True, timeout=10)
        if res.returncode != 0:
            raise RuntimeError(
                f"pg_config --bindir exited {res.returncode}: {res.stderr.strip()}")
        pg_bindir = res.stdout.strip()
    except Exception as exc:
        msg = f"pg_config --bindir failed ({exc}); cannot restart PG after installing extension"
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg) from exc

    pg_ctl = os.path.join(pg_bindir, "pg_ctl")
    psql   = os.path.join(pg_bindir, "psql")
    if not os.path.isfile(pg_ctl):
        msg = f"pg_ctl not found at {pg_ctl}; cannot restart PG after installing extension"
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg)
    if not os.path.isfile(psql):
        msg = f"psql not found at {psql}; cannot locate PG data_directory for restart"
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg)

    try:
        res = subprocess.run([psql, "-t", "-c", "SHOW data_directory;", "postgres"],
                             capture_output=True, text=True, timeout=10)
        if res.returncode != 0:
            raise RuntimeError(
                f"psql SHOW data_directory exited {res.returncode}: {res.stderr.strip()}")
        data_dir = res.stdout.strip()
    except Exception as exc:
        msg = f"could not query data_directory ({exc}); cannot restart PG after installing extension"
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg) from exc

    if not data_dir or not os.path.isdir(data_dir):
        msg = f"data_directory '{data_dir}' is not a directory; cannot restart PG after installing extension"
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg)

    # On macOS use arch -arm64 so pg_ctl (universal binary) runs natively;
    # Rosetta and dyld interact badly with arm64-only extension dylibs.
    if sys.platform == "darwin":
        restart_cmd = ["arch", "-arm64", pg_ctl, "-D", data_dir, "restart"]
    elif _is_aws_preset(active_preset):
        systemctl = shutil.which("systemctl")
        if systemctl:
            restart_cmd = ["sudo", "-n", systemctl, "restart", "postgresql"]
        else:
            restart_cmd = ["sudo", "-n", "-u", "postgres",
                           pg_ctl, "-D", data_dir, "restart"]
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
    _run_postgresql_restart_cmd(restart_cmd, log_handle)


def _pg_config_from_cache(active_build_dir: Path) -> str:
    cmake_cache = active_build_dir / "CMakeCache.txt"
    if cmake_cache.exists():
        for line in cmake_cache.read_text(errors="replace").splitlines():
            if line.startswith("PostgreSQL_CONFIG:FILEPATH="):
                return line.split("=", 1)[1].strip()
    raise RuntimeError("PostgreSQL_CONFIG not in CMakeCache.txt")


def _postgresql_e2e_restart_shell_command(active_build_dir: Path,
                                          active_preset: str) -> str:
    """Build the shell command passed to bioseq_bench_e2e.

    The benchmark executes BIOSEQ_E2E_PG_RESTART_CMD with std::system() before
    every just_index timed sample. Redirect pg_ctl output to a file: the
    benchmark itself is run under the MCP worker's stdout/stderr pipe, and a
    restarted postmaster can otherwise inherit that pipe and keep it open.
    """
    pg_config = _pg_config_from_cache(active_build_dir)
    res = subprocess.run([pg_config, "--bindir"],
                         capture_output=True, text=True, timeout=10)
    if res.returncode != 0:
        raise RuntimeError(
            f"pg_config --bindir exited {res.returncode}: {res.stderr.strip()}")
    pg_bindir = res.stdout.strip()
    pg_ctl = os.path.join(pg_bindir, "pg_ctl")
    psql = os.path.join(pg_bindir, "psql")
    if not os.path.isfile(pg_ctl):
        raise RuntimeError(f"pg_ctl not found at {pg_ctl}")
    if not os.path.isfile(psql):
        raise RuntimeError(f"psql not found at {psql}")

    res = subprocess.run([psql, "-t", "-c", "SHOW data_directory;", "postgres"],
                         capture_output=True, text=True, timeout=10)
    if res.returncode != 0:
        raise RuntimeError(
            f"psql SHOW data_directory exited {res.returncode}: {res.stderr.strip()}")
    data_dir = res.stdout.strip()
    if not data_dir or not os.path.isdir(data_dir):
        raise RuntimeError(f"data_directory '{data_dir}' is not a directory")

    if sys.platform == "darwin":
        argv = ["arch", "-arm64", pg_ctl, "-D", data_dir, "restart", "-m", "fast", "-w"]
    elif _is_aws_preset(active_preset) and shutil.which("systemctl"):
        argv = ["sudo", "-n", shutil.which("systemctl"), "restart", "postgresql"]
    elif _is_aws_preset(active_preset):
        argv = ["sudo", "-n", "-u", "postgres", pg_ctl, "-D", data_dir,
                "restart", "-m", "fast", "-w"]
    else:
        argv = [pg_ctl, "-D", data_dir, "restart", "-m", "fast", "-w"]

    return (
        " ".join(shlex.quote(x) for x in argv)
        + " > /tmp/bioseq_e2e_pg_restart.log 2>&1"
    )


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
                      debug: bool = False, bench_flags=None) -> int:
    """Worker entry point — runs detached from the MCP server.

    Reads the slot the server reserved in state, walks configure → build →
    run, updating `step`/`status` as it goes. The hard timeout is enforced
    by `_stream_subprocess` against `started_at + HARD_TIMEOUT_SEC`.

    Unit/integration mode resolves to the isolated test preset family; benchmark
    modes resolve to the production preset family; a mode with `preset_override`
    (e.g. external-api → GCC 16) pins its own preset outright. `debug=True`
    selects that family's debug/inspect tree with per-TU `.s`/`.bc` and DWARF
    artefacts; `debug=False` selects its lean everyday tree.

    `bench_flags` is a list of extra argv tokens (e.g. ['--filter','tags=gpu',
    '--debug']) appended only to bench-mode commands (§5.4).
    """
    bench_flags = bench_flags or []
    state = _load_state()
    entry = state.get(mode)
    if not entry:
        _log(f"worker for {mode}: no state entry, exiting")
        return 1

    started_at = float(entry["started_at"])
    log_path = Path(entry["log_file"])
    deadline = started_at + HARD_TIMEOUT_SEC

    active_preset = preset_for_mode(mode, debug)
    active_build_dir = build_dir_for_mode(mode, debug)
    active_bin_dir = bin_dir_for_mode(mode, debug)

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
    if not _is_aws_preset(active_preset):
        _ensure_local_toolchain_environment(env, active_build_dir)
    if _is_aws_clang_preset(active_preset):
        env["CMAKE_NAMED_ENV_TYPE"] = "AWS_MACHINE_X64"
    elif _is_aws_gcc_preset(active_preset):
        env["CMAKE_NAMED_ENV_TYPE"] = "AWS_MACHINE_X64_GCC16"
    if _is_aws_preset(active_preset):
        env.setdefault("BIOSEQ_TEST_PGUSER", "postgres")
        env.setdefault("BIOSEQ_TEST_PGPASSWORD", "postgres")
        env.setdefault("BIOSEQ_TEST_PGHOST", "localhost")
        env.setdefault("BIOSEQ_TEST_PGDATABASE", "bioseq")
        env.setdefault("BIOSEQ_TEST_PGPORT", "5432")

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
        required_cache = mode_details.required_cache_variables or {}
        required_cache_args = [
            f"-D{name}={value}" for name, value in required_cache.items()
        ]
        build_ninja = active_build_dir / "build.ninja"
        expected_test_indexes = mode == "unit+integration"
        if not build_ninja.exists():
            if (active_build_dir / "CMakeCache.txt").exists():
                _clear_cmake_configure_cache(
                    active_build_dir,
                    log_handle,
                    "previous configure did not produce build.ninja",
                )
            rc = _stream_subprocess(
                ["cmake", "--preset", active_preset, *required_cache_args],
                log_handle, deadline, cwd=PROJECT_ROOT, env=env,
            )
            if rc != 0:
                raise RuntimeError(f"cmake configure exited with code {rc}")
        else:
            cached_test_indexes = _cmake_cache_bool(
                active_build_dir, "BIOSEQ_TEST_INDEXES")
            if cached_test_indexes != expected_test_indexes:
                expected_text = "ON" if expected_test_indexes else "OFF"
                observed_text = (
                    "missing/invalid"
                    if cached_test_indexes is None
                    else ("ON" if cached_test_indexes else "OFF")
                )
                log_handle.write(
                    "# BIOSEQ_TEST_INDEXES cache invariant mismatch: "
                    f"expected={expected_text} observed={observed_text}; "
                    f"reconfiguring with preset {active_preset}.\n")
                log_handle.flush()
                rc = _stream_subprocess(
                    ["cmake", "--preset", active_preset],
                    log_handle, deadline, cwd=PROJECT_ROOT, env=env,
                )
                if rc != 0:
                    raise RuntimeError(
                        "CMake hook-isolation repair exited with code "
                        f"{rc}")
            else:
                log_handle.write(
                    f"# build tree already configured ({build_ninja}); "
                    "BIOSEQ_TEST_INDEXES invariant is correct; skipping "
                    "redundant `cmake --preset` to avoid a full C++-module "
                    "rebuild (Ninja auto-reconfigures if CMake inputs changed).\n")
                log_handle.flush()

        cache_repairs = {
            name: value
            for name, value in required_cache.items()
            if _cmake_cache_value(active_build_dir, name) != value
        }
        if cache_repairs:
            repair_description = ", ".join(
                f"{name}={value}" for name, value in cache_repairs.items())
            log_handle.write(
                f"# mode cache enable: setting {repair_description}.\n")
            log_handle.flush()
            rc = _stream_subprocess(
                ["cmake", "--preset", active_preset,
                 *(f"-D{name}={value}" for name, value in cache_repairs.items())],
                log_handle, deadline, cwd=PROJECT_ROOT, env=env,
            )
            if rc != 0:
                raise RuntimeError(
                    f"mode cache enable exited with code {rc}")

        if _is_aws_preset(active_preset):
            if (
                _is_aws_clang_preset(active_preset)
                and _cmake_cache_contains(
                    active_build_dir,
                    f"/vcpkg_installed/{AWS_LEGACY_VCPKG_TRIPLET}/",
                )
            ):
                _clear_cmake_configure_cache(
                    active_build_dir,
                    log_handle,
                    f"legacy {AWS_LEGACY_VCPKG_TRIPLET} package paths remain "
                    f"after switching to {AWS_LIBCXX_VCPKG_TRIPLET}",
                )
            current_generator = _cmake_cache_value(active_build_dir, "CMAKE_GENERATOR")
            if current_generator and current_generator != "Ninja":
                _clear_cmake_configure_cache(
                    active_build_dir,
                    log_handle,
                    f"unexpected generator {current_generator!r}; AWS presets require Ninja",
                )
            current_heavy_jobs = _cmake_cache_value(active_build_dir, "BIOSEQ_HEAVY_JOBS")
            repair_args = [
                f"-DBIOSEQ_HEAVY_JOBS={AWS_HEAVY_JOBS}",
            ]
            repair_unset_args = []
            repair_reasons = []
            if current_heavy_jobs != AWS_HEAVY_JOBS:
                repair_reasons.append(
                    f"BIOSEQ_HEAVY_JOBS={AWS_HEAVY_JOBS} "
                    f"(was {current_heavy_jobs or 'unset'})")
            if _is_aws_clang_preset(active_preset):
                current_triplet = _cmake_cache_value(active_build_dir, "VCPKG_TARGET_TRIPLET")
                current_overlay = _cmake_cache_value(active_build_dir, "VCPKG_OVERLAY_TRIPLETS")
                current_overlay_ports = _cmake_cache_value(active_build_dir, "VCPKG_OVERLAY_PORTS")
                current_openssl_details = (
                    _cmake_cache_value(active_build_dir, "FIND_PACKAGE_MESSAGE_DETAILS_OpenSSL")
                    or "")
                overlay_str = str(AWS_LIBCXX_OVERLAY_TRIPLETS)
                overlay_ports_str = str(AWS_LIBCXX_OVERLAY_PORTS)
                repair_args += [
                    f"-DVCPKG_TARGET_TRIPLET={AWS_LIBCXX_VCPKG_TRIPLET}",
                    f"-DVCPKG_OVERLAY_TRIPLETS={overlay_str}",
                    f"-DVCPKG_OVERLAY_PORTS={overlay_ports_str}",
                ]
                if current_triplet != AWS_LIBCXX_VCPKG_TRIPLET:
                    repair_reasons.append(
                        f"VCPKG_TARGET_TRIPLET={AWS_LIBCXX_VCPKG_TRIPLET} "
                        f"(was {current_triplet or 'unset'})")
                    repair_unset_args.extend(
                        f"-U{pattern}" for pattern in AWS_LIBCXX_FIND_CACHE_UNSET_PATTERNS)
                if current_overlay != overlay_str:
                    repair_reasons.append(
                        f"VCPKG_OVERLAY_TRIPLETS={overlay_str} "
                        f"(was {current_overlay or 'unset'})")
                if current_overlay_ports != overlay_ports_str:
                    repair_reasons.append(
                        f"VCPKG_OVERLAY_PORTS={overlay_ports_str} "
                        f"(was {current_overlay_ports or 'unset'})")
                if (
                    f"/vcpkg_installed/{AWS_LEGACY_VCPKG_TRIPLET}/" in current_openssl_details
                    and f"/vcpkg_installed/{AWS_LIBCXX_VCPKG_TRIPLET}/"
                    not in current_openssl_details
                ):
                    repair_reasons.append(
                        "clearing stale OpenSSL/PostgreSQL find-package cache "
                        f"from {AWS_LEGACY_VCPKG_TRIPLET}")
                    repair_unset_args.extend(
                        f"-U{pattern}" for pattern in AWS_LIBCXX_FIND_CACHE_UNSET_PATTERNS)
            elif _is_aws_gcc_preset(active_preset):
                expected_gcc_cxx_flags = AWS_GCC161_CXX_FLAGS
                if _is_debug_preset(active_preset):
                    expected_gcc_cxx_flags += " -save-temps=obj -g"
                current_cxx_flags = _cmake_cache_value(active_build_dir, "CMAKE_CXX_FLAGS")
                current_typed_dispatch_opt = _cmake_cache_value(
                    active_build_dir,
                    "BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL",
                )
                current_typed_dispatch_jobs = _cmake_cache_value(
                    active_build_dir,
                    "BIOSEQ_GCC_TYPED_DISPATCH_JOBS",
                )
                repair_args.append(
                    f"-DCMAKE_CXX_FLAGS={expected_gcc_cxx_flags}")
                repair_args.append(
                    "-DBIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL="
                    f"{AWS_GCC_TYPED_DISPATCH_OPT_LEVEL}")
                repair_args.append(
                    "-DBIOSEQ_GCC_TYPED_DISPATCH_JOBS="
                    f"{AWS_GCC_TYPED_DISPATCH_JOBS}")
                if current_cxx_flags != expected_gcc_cxx_flags:
                    repair_reasons.append(
                        f"CMAKE_CXX_FLAGS={expected_gcc_cxx_flags!r} "
                        f"(was {current_cxx_flags or 'unset'})")
                if current_typed_dispatch_opt != AWS_GCC_TYPED_DISPATCH_OPT_LEVEL:
                    repair_reasons.append(
                        "BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL="
                        f"{AWS_GCC_TYPED_DISPATCH_OPT_LEVEL} "
                        f"(was {current_typed_dispatch_opt or 'unset'})")
                if current_typed_dispatch_jobs != AWS_GCC_TYPED_DISPATCH_JOBS:
                    repair_reasons.append(
                        "BIOSEQ_GCC_TYPED_DISPATCH_JOBS="
                        f"{AWS_GCC_TYPED_DISPATCH_JOBS} "
                        f"(was {current_typed_dispatch_jobs or 'unset'})")
            if repair_reasons:
                log_handle.write("# AWS cache repair: setting "
                                 + "; ".join(repair_reasons) + ".\n")
                log_handle.flush()
                rc = _stream_subprocess(
                    ["cmake", "--preset", active_preset,
                     *dict.fromkeys(repair_unset_args), *repair_args],
                    log_handle, deadline, cwd=PROJECT_ROOT, env=env,
                )
                if rc != 0:
                    raise RuntimeError(
                        f"AWS cache repair exited with code {rc}")

        configured_test_indexes = _cmake_cache_bool(
            active_build_dir, "BIOSEQ_TEST_INDEXES")
        if configured_test_indexes != expected_test_indexes:
            expected_text = "ON" if expected_test_indexes else "OFF"
            observed_text = (
                "missing/invalid"
                if configured_test_indexes is None
                else ("ON" if configured_test_indexes else "OFF")
            )
            raise RuntimeError(
                "BIOSEQ_TEST_INDEXES cache invariant failed closed: "
                f"mode={mode} preset={active_preset} expected={expected_text} "
                f"observed={observed_text}. Test modes require an isolated "
                "ON preset; benchmark modes require an explicit OFF preset.")
        verified_test_indexes = "ON" if configured_test_indexes else "OFF"
        log_handle.write(
            "# verified BIOSEQ_TEST_INDEXES="
            f"{verified_test_indexes} preset={active_preset}.\n")
        log_handle.flush()

        _update_entry(mode, step="building", build_only=build_only,
                      debug=debug,
                      bioseq_test_indexes=verified_test_indexes)
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
                    _cmake_install_cmd(active_build_dir, active_preset),
                    log_handle, deadline, cwd=PROJECT_ROOT, env=env,
                )
                if rc != 0:
                    raise RuntimeError(
                        "extension install (cmake --install --component bioseq) "
                        f"exited with code {rc}")
                _restart_postgresql(active_build_dir, active_preset,
                                    log_handle, deadline)
                _prepare_aws_postgresql_test_database(
                    active_preset, log_handle, deadline)

            _update_entry(mode, step="running")
            if mode == "bench-e2e":
                env.setdefault(
                    "BIOSEQ_E2E_PG_RESTART_CMD",
                    _postgresql_e2e_restart_shell_command(active_build_dir, active_preset),
                )
                log_handle.write(
                    "# BIOSEQ_E2E_PG_RESTART_CMD="
                    f"{env['BIOSEQ_E2E_PG_RESTART_CMD']}\n")
                log_handle.flush()
            for cmd in mode_details.commands:
                # MODES.commands store binary BASENAMES; resolve against the
                # active build's bin dir (DOC0270). Args after the first slot
                # are passed through verbatim.
                full_cmd = [str(active_bin_dir / cmd[0]), *cmd[1:]]
                if mode_details.is_bench_mode and bench_flags:
                    full_cmd += bench_flags          # e.g. --filter tags=gpu --debug
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


def _running_in_same_group(
        state: dict, mode: str, build_only: bool) -> tuple[str | None, dict | None]:
    """Find any currently-running run that conflicts with `mode`.

    Returns (running_mode, entry) or (None, None). Conflicts are decided by
    lock groups, plus the shared PostgreSQL install/restart resource. bench and
    bench-fast share a group; runtime unit+integration and runtime bench-e2e
    also conflict because both replace and restart the same extension.
    """
    mode_details = MODES[mode]
    for other_mode in VALID_MODES:
        entry = _refresh(state, other_mode)
        if not entry:
            continue
        if entry.get("status") != "running":
            continue
        other_details = MODES[other_mode]
        same_lock_group = other_details.lock_group == mode_details.lock_group
        shared_postgresql_runtime = (
            not build_only
            and not bool(entry.get("build_only", False))
            and mode_details.install_extension
            and other_details.install_extension
        )
        if same_lock_group or shared_postgresql_runtime:
            return other_mode, entry
    return None, None


# (object key in the MCP `filter`, bench CLI key). Note plural->singular for
# benchmarks: the MCP surface is `benchmarks` (matching the prompt's
# FilterConditions.benchmarks) but the bench CLI key is the singular `benchmark`.
_FILTER_AXES = (("tags", "tags"), ("benchmarks", "benchmark"), ("cases", "cases"))


def _filter_to_cli_args(bench_filter):
    """Translate the structured `filter` object into bench CLI tokens.

    Returns (extra_argv, error). extra_argv is a flat list like
    ['--filter','tags=a,b','--filter','benchmark=k']. error is a human string or
    None. Validates value shape (non-empty arrays / strings, no ',' or '=').
    """
    if not bench_filter:
        return [], None
    if not isinstance(bench_filter, dict):
        return [], "filter must be an object"
    extra: list = []
    for obj_key, cli_key in _FILTER_AXES:
        if obj_key not in bench_filter:
            continue
        values = bench_filter[obj_key]
        if not isinstance(values, list) or not values:
            return [], f"filter.{obj_key} must be a non-empty array"
        for v in values:
            if not isinstance(v, str) or not v:
                return [], f"filter.{obj_key} entries must be non-empty strings"
            if "," in v or "=" in v:
                return [], f"filter.{obj_key} value '{v}' may not contain ',' or '='"
        extra += ["--filter", f"{cli_key}=" + ",".join(values)]
    return extra, None


def tool_start_build(mode: str, build_only: bool = False,
                     debug: bool = False, bench_filter=None,
                     debug_report: bool = False) -> str:
    if mode not in VALID_MODES:
        return (f"Invalid mode '{mode}'. Valid modes are: "
                f"{', '.join(VALID_MODES)}.")

    mode_details = MODES[mode]
    try:
        selected_preset = preset_for_mode(mode, debug)
    except RuntimeError as exc:
        return f"Build preset isolation error: {exc}"

    # Bench-only knobs: reject on non-bench modes, validate + translate the
    # filter, and enforce debug-requires-filter — all BEFORE spawning the
    # long-running detached worker (fast feedback).
    if (bench_filter or debug_report) and not mode_details.is_bench_mode:
        return (f"`filter`/`debug_report` are only valid for bench modes "
                f"(bench, bench-fast, bench-io, bench-e2e); got '{mode}'.")
    filter_args, ferr = _filter_to_cli_args(bench_filter)
    if ferr is not None:
        return f"Invalid filter: {ferr}"
    if debug_report and not filter_args:
        return ("`debug_report` requires a non-empty `filter` — debug is for targeted "
                "runs, not for instrumenting an entire benchmark sweep.")

    # Encode the bench flags appended to the bench binary by the worker.
    bench_flags = list(filter_args)
    if debug_report:
        bench_flags.append("--debug")

    with _global_lock():
        state = _load_state()
        running_mode, running_entry = _running_in_same_group(
            state, mode, build_only)
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
             "--debug", ("true" if debug else "false"),
             "--bench-flags", json.dumps(bench_flags)],
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
            "build_only": build_only,
            "preset": selected_preset,
            "build_dir": str(PROJECT_ROOT / "build" / selected_preset),
            "bench_flags": bench_flags,
        }
        _save_state(state)
        _log(f"started worker pid={proc.pid} mode={mode} debug={debug} "
             f"bench_flags={bench_flags} log={log_path}")

    return (f"{mode_details.label} {mode_details.verb_is} scheduled "
            f"(preset={selected_preset})"
            + (f" with bench flags {' '.join(bench_flags)}" if bench_flags else "")
            + f". Predicted finish in {_format_minutes((mode_details.build_only_expected_duration_min if build_only else  mode_details.expected_duration_min), plural_int=True)}.")


def _run_build_inline(mode: str, build_only: bool = False,
                      debug: bool = False, bench_flags=None) -> int:
    """Reserve the per-mode state slot and run the build worker INLINE
    (synchronously, in this very process) instead of detaching it.

    `tool_start_build` reserves the slot and then spawns a *detached* worker;
    the local MCP server needs that because a 25-min bench must outlive a closed
    Claude Code window. The remote path is the mirror image: the remote-run tool
    invokes `server.py --run-build-sync ...` over SSH on the AWS box and wants the
    build to run in the foreground of that SSH command so its combined output
    streams straight back over the pipe and the SSH exit status equals the build
    verdict (0 ok / 1 our-code-fails-as-expected / 124 timeout). Detaching there
    would make the SSH command return instantly with no result.

    Reserving the slot first means `_run_build_worker` (which requires a
    pre-existing entry) finds its state, and the same-host lock-group guard still
    prevents two conflicting builds racing on the box. pid is this process's own
    pid so `status`/`_refresh` reconcile correctly if we die mid-build.

    Returns the worker's exit code, or a small non-worker code on a pre-flight
    refusal (2 = invalid mode, 3 = a conflicting build is already running here).
    """
    if mode not in VALID_MODES:
        _log(f"--run-build-sync: invalid mode '{mode}'")
        return 2
    try:
        selected_preset = preset_for_mode(mode, debug)
    except RuntimeError as exc:
        _log(f"--run-build-sync: build preset isolation error: {exc}")
        return 2

    with _global_lock():
        state = _load_state()
        running_mode, running_entry = _running_in_same_group(
            state, mode, build_only)
        if running_entry:
            _log(f"--run-build-sync: a conflicting build ({running_mode}) is "
                 f"already running on this host (pid={running_entry.get('pid')}); "
                 f"refusing to start {mode}")
            return 3

        started_at = time.time()
        timestamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(started_at))
        log_path = LOG_DIR / (
            f"bioseq_build_mcp_{mode.replace('+', '_')}_"
            f"{'debug_' if debug else ''}{timestamp}.log"
        )
        state[mode] = {
            "mode": mode,
            "status": "running",
            "step": "spawning",
            "started_at": started_at,
            "completed_at": None,
            "pid": os.getpid(),
            "log_file": str(log_path),
            "exit_code": None,
            "report_file": None,
            "notes": None,
            "debug": debug,
            "build_only": build_only,
            "preset": selected_preset,
            "build_dir": str(PROJECT_ROOT / "build" / selected_preset),
            "bench_flags": list(bench_flags or []),
        }
        _save_state(state)
        _log(f"--run-build-sync: running {mode} inline (pid={os.getpid()}, "
             f"preset={selected_preset}, build_only={build_only}, "
             f"bench_flags={bench_flags or []}, log={log_path})")

    return _run_build_worker(mode, build_only=build_only, debug=debug,
                             bench_flags=bench_flags)


def tool_inspect(arguments: dict) -> str:
    """Implement the `inspect` tool. Returns a single string (a JSON
    document) — the MCP tools/call envelope wraps it in a content item.

    All errors come back as JSON `{"error": "...", ...}` rather than
    raising, so the AI agent always gets a structured result.

    The DOC0270 split means the inspect tool needs DWARF and per-TU temps.
    Benchmark targets resolve against the production preset family; unit and
    integration executables resolve against the isolated test preset family so
    guarded symbols are never inspected from a hook-free production binary.
    We auto-detect the matching debug tree and accept `debug=true|false` as an
    explicit override.
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

    # Resolve which preset family owns the requested binary. `debug` is an
    # explicit override; without it, prefer that family's debug tree if built.
    test_targets = {
        "bioseq_aligner_tests_exec",
        "bioseq_aligner_int_tests_exec",
    }
    inspect_mode = "unit+integration" if target in test_targets else "bench"
    debug_raw = arguments.get("debug")
    try:
        if debug_raw is None:
            debug_chosen = (
                build_dir_for_mode(inspect_mode, True) / "build.ninja"
            ).exists()
        else:
            debug_chosen = parse_bool("debug", debug_raw)
        inspect_build_dir = build_dir_for_mode(inspect_mode, debug_chosen)
        inspect_preset = preset_for_mode(inspect_mode, debug_chosen)
    except RuntimeError as e:
        return json.dumps({"error": str(e)}, indent=2)

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
    out["build_metadata"]["preset_used"] = inspect_preset
    out["build_metadata"]["test_indexes_expected"] = (
        inspect_mode == "unit+integration")
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
    selected_preset = entry.get(
        "preset", preset_for_mode(mode, debug_used))
    preset_suffix = f" preset={selected_preset}"

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
            msg += (f". This was a debug build (preset={selected_preset}); "
                    "DWARF + per-TU .s/.bc artefacts are available under "
                    f"{entry.get('build_dir', str(build_dir_for_mode(mode, True)))} for the "
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

    # DO NOT EDIT THIS!
    return "Error: subagent is disabled for now. Use native subagents!"

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


# =========================================================================== #
# remote operations — AWS benchmark machine (main/infra)
# =========================================================================== #
# Remote tools drive the disposable x86_64 Ubuntu benchmark box defined under
# main/infra/ (OpenTofu + provisioner.py). See main/infra/README.md.
#
# MUTEX: mutating/long remote operations share ONE global slot
# (REMOTE_STATE_FILE), so at most one remote operation runs at a time across all
# Claude Code sessions. This is the same anti-footgun guard the local builds have,
# widened to the remote: it stops an agent from firing `up` ten times and stacking
# ten paid EC2 instances, or mutating the box's git tree while a remote build is
# mid-flight. remote-ssh / remote-status-ps / remote-debug are deliberately
# OUTSIDE the mutex — read-only escape hatches for diagnostics while a long
# remote-run holds the slot.
#
# Long operations (provision, build) detach into a worker (`--run-remote <op_id>`)
# and return immediately; you poll by re-calling any remote tool (a *running* slot
# reports progress instead of starting anything). Short operations (status, output,
# git, …) usually finish inside the in-handler grace-wait and return their output
# inline on the first call. Either way the worker streams everything to a log file
# whose path is always returned, so the full output is one Read away. When an
# operation finishes, the NEXT remote tool call returns its result once (and frees
# the slot); call again to start the next operation. This "deliver-once" rule makes
# polling a finished long build safe — re-calling never silently launches a rebuild.

INFRA_DIR = PROJECT_ROOT / "infra"
PROVISIONER = INFRA_DIR / "provisioner.py"

# Layout on the box (set by main/infra/testing_infra/scripts/{fetch_repo,verify}.sh):
# the repo is cloned to ~/bioseqdb and built with the aws-x64-linux CMake preset
# (x64-linux vcpkg triplet, clang/libc++ 22), so the box-side build worker must be
# told to use that preset family instead of the local macOS/arm64 defaults. We
# export production, test, and both debug preset names when invoking the box-side
# server so unit/integration never reuses the production benchmark tree.
REMOTE_REPO_PATH = "~/bioseqdb"
REMOTE_BUILD_PRESET = "aws-x64-linux"
REMOTE_BUILD_PRESETS = {
    "aws-x64-linux",
    "aws-x64-linux-gcc161",
}

REMOTE_STATE_FILE = Path("/tmp/bioseq_remote_mcp_state.json")
REMOTE_LOCK_FILE = Path("/tmp/bioseq_remote_mcp.lock")

# On-box provisioner.py PROVISION_STATUS markers that mean "toolchain is ready to
# build" (kept in sync with provisioner.py GOOD_STATUSES).
REMOTE_GOOD_STATUSES = {"TOOLCHAIN_READY", "VERIFY_OK_FAILS_ON_OUR_CODE",
                        "VERIFY_BUILD_SUCCEEDED"}

# Per-operation hard ceilings (seconds) enforced inside the detached worker.
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
# In-handler grace-wait (seconds): how long the tool call blocks waiting for the
# op to finish before returning "in progress". Quick ops get a long-enough grace
# to return inline; long ops get a short one and are polled by re-calling.
REMOTE_OP_GRACE = {
    "status": 30.0, "output": 30.0, "logs": 30.0, "git": 30.0,
    "up": 6.0, "down": 8.0, "snapshot": 8.0,
    "verify": 8.0, "wait": 8.0, "run": 6.0,
}
REMOTE_TERMINAL_STATUSES = {"completed", "failed", "killed_timeout", "aborted"}
REMOTE_MANAGE_OPS = {"up", "down", "status", "logs", "verify", "wait", "output"}
# How much of the captured log to inline in a tool response (full log via path).
REMOTE_LOG_INLINE_CAP = 6000
REMOTE_DEBUG_INLINE_CAP = 14000


# --------------------------- remote state I/O ------------------------------

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
    REMOTE_STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
    tmp = REMOTE_STATE_FILE.with_suffix(".json.tmp")
    with tmp.open("w") as f:
        json.dump(state, f, indent=2, sort_keys=True)
    tmp.replace(REMOTE_STATE_FILE)


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


# --------------------------- remote subprocess plumbing --------------------

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


def _tail_path(path_text: str | None, lines: int, cap: int = REMOTE_DEBUG_INLINE_CAP) -> str:
    if not path_text:
        return "(no log path)"
    path = Path(path_text)
    try:
        data = path.read_text(errors="replace")
    except OSError as e:
        return f"(cannot read {path}: {e})"
    tail = "\n".join(data.splitlines()[-max(1, lines):])
    if len(tail) > cap:
        tail = "…(truncated)…\n" + tail[-cap:]
    return tail or "(empty log)"


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
                             cwd=str(PROJECT_ROOT), text=True,
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


# --------------------------- remote-run driver -----------------------------

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
    server_dir = f"{REMOTE_REPO_PATH}/main/build_mcp"
    if operation == "build":
        mode = payload["mode"]
        build_only = bool(payload.get("build_only", False))
        debug = bool(payload.get("debug", False))
        bench_flags = payload.get("bench_flags") or []   # parity with local path
        remote_cmd = (
            f"cd {server_dir} && {env_prefix}python3 server.py "
            f"--run-build-sync {shlex.quote(mode)} "
            f"--build-only {'true' if build_only else 'false'} "
            f"--debug {'true' if debug else 'false'} "
            f"--bench-flags {shlex.quote(json.dumps(bench_flags))}"
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
    our = [ln for ln in err_lines if "/main/bioseq/" in ln]
    third = [ln for ln in err_lines
             if any(s in ln for s in ("vcpkg_installed", "/opt/vcpkg", "/vendor/",
                                      "/usr/lib/llvm", "/usr/include/", "/opt/llvm"))
             and "/main/bioseq/" not in ln]
    neon = ("NEON" in text) or ("dispatch/dispatch.h" in text)
    if third:
        return (f"UNEXPECTED third-party/toolchain failure — {len(third)} error "
                f"line(s) outside our code. This is NOT the expected gap; "
                f"investigate. First: {third[0].strip()[:200]}")
    if our:
        tag = "the expected ::NEON / macOS-ism gap" if neon else "our own code"
        return (f"expected_our_code_failure — configure succeeded and every "
                f"third-party dependency compiled; the only errors are in {tag} "
                f"({len(our)} line(s) under /main/bioseq/). This is exactly the "
                f"VERIFY_OK_FAILS_ON_OUR_CODE outcome: the remote build pipeline is "
                f"healthy and the failure is the known x86_64 SIMD port gap. "
                f"First: {our[0].strip()[:200]}")
    return ("failed_unclassified — non-zero exit but no 'error:' lines matched by "
            "path. Read the full log to see what happened.")


# --------------------------- remote worker ---------------------------------

def _run_remote_worker(op_id: str) -> int:
    """Detached worker entry point (invoked via `--run-remote <op_id>`). Waits for
    the slot it owns (matched by op_id, to ignore any stale prior state), runs the
    recorded command/payload streaming to the log, and records the verdict."""
    state = {}
    for _ in range(100):  # ~10s — tolerate the benign spawn→write race
        state = _load_remote_state()
        if state.get("op_id") == op_id and state.get("status") == "running":
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


# --------------------------- remote presentation ---------------------------

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
        server_path = str(Path(__file__).resolve())
        proc = subprocess.Popen(
            [sys.executable, server_path, "--run-remote", op_id],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, start_new_session=True, close_fds=True)
        _save_remote_state({
            "op_id": op_id, "kind": kind, "label": label,
            "status": "running", "step": "spawning",
            "started_at": started_at, "completed_at": None, "pid": proc.pid,
            "log_file": str(log_path), "exit_code": None, "notes": None,
            "classification": None, "delivered": False,
            "timeout_sec": timeout_sec, "argv": argv, "payload": payload,
        })
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


# --------------------------- remote tool implementations -------------------

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
    include_ssh = parse_bool("include_ssh", arguments.get("include_ssh", False))
    include_ssm = parse_bool("include_ssm", arguments.get("include_ssm", False))
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
df -h / ~/bioseqdb/main/build/aws-x64-linux 2>/dev/null || true
echo
echo "## build processes"
ps -eo pid,ppid,stat,ni,pcpu,pmem,rss,vsz,etime,args --cols 220 \
  | { read -r header; echo "$header"; grep -E '[c]make|[n]inja|[c]lang(\+\+)?|[g]\+\+|[g]cc|[x]gcc|[c]c1(plus)?|[l]d\.lld|[c]ollect2|/opt/[g]cc-build|postgresql_[b]ioseq|[b]ioseq_(bench|io_bench|aligner)|dispatch_processor\.cppm' | tail -n 80 || true; } \
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
        from_scratch = parse_bool("from_scratch", arguments.get("from_scratch", False))
        if from_scratch:
            argv += ["--from-scratch"]
        if arguments.get("volume_size"):
            argv += ["--volume-size", str(int(arguments["volume_size"]))]
        if parse_bool("no_verify", arguments.get("no_verify", False)):
            argv += ["--no-verify"]
        if parse_bool("wait", arguments.get("wait", True)):
            argv += ["--wait"]
        argv += ["--timeout", str(int(arguments.get("timeout", 120)))]
        if parse_bool("yes", arguments.get("yes", False)):
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
            f"{REMOTE_REPO_PATH}/main/infra/testing_infra/scripts/verify.sh")
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
    if parse_bool("no_wait", arguments.get("no_wait", False)):
        argv += ["--no-wait"]
    if parse_bool("no_reboot", arguments.get("no_reboot", False)):
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
    sync = parse_bool("sync", arguments.get("sync", True))
    auto_provision = parse_bool("auto_provision", arguments.get("auto_provision", True))
    from_scratch = parse_bool("from_scratch", arguments.get("from_scratch", False))
    preset = arguments.get("preset", REMOTE_BUILD_PRESET)
    if preset not in REMOTE_BUILD_PRESETS:
        return (f"Invalid preset '{preset}'. remote-run presets: "
                f"{', '.join(sorted(REMOTE_BUILD_PRESETS))}.")

    if operation == "build":
        mode = arguments.get("mode")
        if mode not in VALID_MODES:
            return (f"Invalid mode '{mode}'. remote-run build modes: "
                    f"{', '.join(sorted(VALID_MODES))}.")
        build_only = parse_bool("build_only", arguments.get("build_only", False))
        debug = parse_bool("debug", arguments.get("debug", False))
        bench_filter = arguments.get("filter")
        debug_report = parse_bool("debug_report",
                                  arguments.get("debug_report", False))
        mode_details = MODES[mode]
        if (bench_filter or debug_report) and not mode_details.is_bench_mode:
            return (f"`filter`/`debug_report` are only valid for bench modes "
                    f"(bench, bench-fast, bench-io, bench-e2e); got '{mode}'.")
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
                   "bench_flags": bench_flags, "preset": preset}
        label = (f"remote build mode={mode} build_only={build_only} debug={debug} "
                 f"(branch={branch}, preset={preset}, from_scratch={from_scratch})")
        if bench_flags:
            label += f" bench_flags={' '.join(bench_flags)}"
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


# --------------------------- MCP protocol layer ----------------------------

PROTOCOL_VERSION = "2024-11-05"
SERVER_INFO = {"name": "bioseq-build-mcp", "version": "1.1.0"}

TOOL_DEFINITIONS = [
    {
        "name": "start_build",
        "description": (
            "Schedule a build/test/benchmark run. The build runs detached so "
            "it survives MCP server restarts. If a run of the same lock group "
            "is already in progress (non-SQL benchmarks may run with tests; "
            "unit+integration and bench-e2e serialize because both install "
            "and restart the shared PostgreSQL extension) "
            "returns the current status instead of starting a duplicate. "
            f"{get_modes_short_documentation()} "
            f"\nNote: If you want to iterate quickly then use build_only parameter."
            f"\n\nBuild tree selection (`debug` parameter, DOC0270):"
            f"\n  - debug=false (DEFAULT): uses the mode's fast preset "
            f"(`test-opt` for unit+integration, `bench-opt` for benchmarks). "
            f"-O3 + NDEBUG only, no debug info, no "
            f"per-TU temps. This is the fast everyday build — preferred for "
            f"every normal iteration, including running tests and benches."
            f"\n  - debug=true: uses the corresponding isolated debug preset "
            f"(`test-opt-debug` or `bench-opt-debug`). Same -O3 + NDEBUG, "
            f"plus -g (DWARF "
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
                       "Select the mode's isolated debug build tree "
                       "(test-opt-debug for unit+integration, "
                       "bench-opt-debug for benchmarks). Adds -g + "
                       "-save-temps=obj "
                       "so the `inspect` tool can extract per-function "
                       "disassembly and pre-LTO assembly listings. ~2x slower "
                       "and uses ~3x more memory per TU than the default "
                       "fast build — only set this when you actually need "
                       "the inspect artefacts. Default false (uses the "
                       "corresponding fast preset)."
                   ),
                   "default": False,
                },
                "filter": {
                    "type": "object",
                    "additionalProperties": False,
                    "description": (
                        "Bench-only. Subset selection for bench/bench-fast/bench-io/bench-e2e, "
                        "translated to the bench binary's repeatable --filter flag. Each present "
                        "array is an OR-set; the three arrays AND together. Omit an array to leave "
                        "that axis unconstrained. Values may not contain ',' or '='."
                    ),
                    "properties": {
                        "tags":       {"type": "array", "items": {"type": "string", "minLength": 1}, "minItems": 1,
                                       "description": "Run only benchmarks whose `tags` intersect this set."},
                        "cases":      {"type": "array", "items": {"type": "string", "minLength": 1}, "minItems": 1,
                                       "description": "Within selected benchmarks, run only these matrix.over cases (algorithm names)."},
                        "benchmarks": {"type": "array", "items": {"type": "string", "minLength": 1}, "minItems": 1,
                                       "description": "Run only these benchmark keys. (Maps to the bench CLI's singular `benchmark=`.)"}
                    }
                },
                "debug_report": {
                    "type": "boolean",
                    "default": False,
                    "description": (
                        "Bench-only. Pass --debug to the bench binary: stream a crash-resilient "
                        "report_debug_<N>.jsonl (one JSON event per case) next to report_<N>.json. "
                        "Requires a non-empty `filter` (debug is for targeted runs). NOTE: this is "
                        "NOT the build-tree `debug` flag above."
                    )
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
            "DOC0270 — build-tree selection: fast production/test trees do "
            "NOT carry per-TU .s/.bc files or DWARF. Call `start_build` for "
            "the requested target's mode with debug=true first to produce "
            "the matching isolated debug tree. If you pass "
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
            "fresh debug build is ~25-100s (one llc per relevant .bc, "
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
                        "Which target binary to inspect. Test executables "
                        "resolve under test-opt[-debug]; benchmark executables "
                        "resolve under bench-opt[-debug].",
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
                        "Force the inspect tool to read from the requested "
                        "target's matching tree (DOC0270). Test executables "
                        "use test-opt[-debug]; benchmark executables use "
                        "bench-opt[-debug]. true selects the debug tree, "
                        "false the fast tree. Omit to auto-detect: "
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

# --------------------------- remote tools (AWS benchmark machine) -----------
# Tools that drive the disposable x86_64 Ubuntu benchmark box under
# main/infra/ (OpenTofu + provisioner.py). See main/infra/README.md.
_REMOTE_MUTEX_NOTE = (
    "\n\nMUTEX & POLLING: every remote tool EXCEPT remote-ssh shares ONE global "
    "lock — at most one remote operation runs at a time (so you can't, e.g., fire "
    "`up` ten times and stack ten paid EC2 instances, or change the box's branch "
    "mid-build). Long operations (provision/build) run DETACHED and return "
    "immediately; poll by re-calling ANY remote tool (a running slot reports "
    "progress and will NOT start a second op). When an op finishes, the NEXT remote "
    "tool call returns its result once and frees the slot — so polling a finished "
    "build never silently launches a rebuild. The full output is always streamed to "
    "a log file whose path is returned (open it with Read). Use remote-ssh (which is "
    "NOT under the mutex) to inspect the box while a long op holds the slot."
)
_FROM_SCRATCH_WARNING = (
    "DANGER: default false. In normal circumstances agents should NEVER set "
    "from_scratch=true. It bypasses the cached AMI/snapshot path and starts a "
    "new EC2 box from the base Ubuntu image, losing the startup/build-cache "
    "benefit. Use true only when the USER has explicitly confirmed a clean "
    "from-scratch rebuild."
)
_REMOTE_RUN_COMMON_PROPS = {
    "branch": {
        "type": "string",
        "description": (
            "Branch to build. remote-run builds the latest PUSHED commit: it "
            "git-fetches and checks out origin/<branch> on the box before building "
            "(see `sync`). Defaults to your current local branch. The branch MUST be "
            "pushed to origin, and must contain the build_mcp updates that provide "
            "the box-side --run-build-sync entry point."
        ),
    },
    "sync": {
        "type": "boolean",
        "default": True,
        "description": (
            "Before building, fetch origin and `git checkout -B <branch> "
            "origin/<branch>` on the box so it builds the latest pushed commit "
            "(force-resets the box's working branch — the box is disposable). Set "
            "false to build whatever is already checked out on the box (e.g. when "
            "you arranged it via remote-git, or to avoid clobbering a manual state)."
        ),
    },
    "auto_provision": {
        "type": "boolean",
        "default": True,
        "description": (
            "If no box is currently running, provision one (provisioner up --wait) "
            "before building. This launches a PAID EC2 instance. Set false to fail "
            "fast instead when no box exists."
        ),
    },
    "from_scratch": {
        "type": "boolean",
        "default": False,
        "description": (
            _FROM_SCRATCH_WARNING + " When false, provisioner.py up automatically "
            "uses the newest cached bioseq AMI in the target region if one exists."
        ),
    },
    "preset": {
        "type": "string",
        "enum": sorted(REMOTE_BUILD_PRESETS),
        "default": REMOTE_BUILD_PRESET,
        "description": (
            "Remote CMake preset exported as BIOSEQ_BUILD_PRESET for the box-side "
            "build worker. The default is the Clang/libc++ AWS preset; use "
            "aws-x64-linux-gcc161 for the separate GCC 16.1 build tree."
        ),
    },
}
TOOL_DEFINITIONS += [
    {
        "name": "remote-status",
        "description": (
            "Show the AWS benchmark box status — wraps `provisioner.py status`: "
            "Terraform outputs (instance id / public ip / region), the live EC2 "
            "state, a count of running benchmark instances (cost guardrail), and the "
            "on-box PROVISION_STATUS marker (VERIFY_OK_FAILS_ON_OUR_CODE = ready). "
            "Equivalent to remote-manage with operation='status'." + _REMOTE_MUTEX_NOTE
        ),
        "inputSchema": {"type": "object", "properties": {}, "required": []},
    },
    {
        "name": "remote-debug",
        "description": (
            "Read-only diagnostics for the AWS benchmark box and the local remote "
            "MCP worker. Unlike remote-status/remote-manage/remote-run, this tool "
            "does NOT acquire the remote mutex and can be called while a long build "
            "owns the slot. It reports local worker process state, the worker log "
            "tail, Terraform outputs, EC2 instance/status-check data, and recent "
            "CloudWatch CPU via AWS APIs. Set include_ssh=true to also attempt a "
            "short bounded read-only SSH process probe; by default it avoids SSH so "
            "it still works when SSH banner exchange is slow during saturated "
            "compiles."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "log_lines": {
                    "type": "integer", "minimum": 1, "maximum": 500, "default": 80,
                    "description": "Number of local remote-worker log lines to include.",
                },
                "cloudwatch_minutes": {
                    "type": "integer", "minimum": 5, "maximum": 360, "default": 120,
                    "description": "Lookback window for AWS/EC2 CPUUtilization datapoints.",
                },
                "include_ssh": {
                    "type": "boolean", "default": False,
                    "description": "Also attempt a bounded read-only SSH ps/free/df probe.",
                },
                "include_ssm": {
                    "type": "boolean", "default": False,
                    "description": (
                        "Also attempt a read-only AWS Systems Manager RunCommand "
                        "ps/free/df probe when the SSM agent is Online."
                    ),
                },
                "ssh_timeout": {
                    "type": "integer", "minimum": 5, "maximum": 120, "default": 20,
                    "description": "Subprocess timeout for the optional SSH probe.",
                },
                "ssm_timeout": {
                    "type": "integer", "minimum": 10, "maximum": 120, "default": 30,
                    "description": "Timeout for the optional SSM read-only probe.",
                },
            },
            "required": [],
        },
    },
    {
        "name": "remote-status-ps",
        "description": (
            "Run a concise read-only SSH status snapshot on the AWS box without "
            "acquiring the remote mutex. It prints date, host/provision status, "
            "repo commit, load, `swapon --show`, `free -h`, root/build disk "
            "usage, and the current cmake/ninja/clang/gcc/linker/bioseq process list. "
            "Use it while a long remote-run owns the slot to inspect memory, CPU, "
            "swap, and active build processes."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "timeout": {
                    "type": "integer", "minimum": 5, "maximum": REMOTE_OP_TIMEOUTS["ssh"],
                    "default": 300,
                    "description": "Subprocess timeout in seconds for the SSH snapshot.",
                },
            },
            "required": [],
        },
    },
    {
        "name": "remote-manage",
        "description": (
            "Manage the AWS benchmark box — wraps `provisioner.py <operation>` for "
            "operation in up/down/status/logs/verify/wait/output:\n"
            "  up     — create/update the box (OpenTofu apply). Provisions a PAID "
            "instance. Defaults: uses the newest cached bioseq AMI/snapshot in the "
            "target region when available, branch=your current local branch, "
            "region=the existing stack's region, wait=true (block until on-box "
            "provisioning is terminal). Refuses if a box is already running unless "
            "yes=true. " + _FROM_SCRATCH_WARNING + "\n"
            "  down   — create a cached AMI/snapshot of the current root volume, "
            "then destroy the box and managed resources (always non-interactive).\n"
            "  status — same as remote-status.\n"
            "  logs   — tail /var/log/bioseq-provision.log (last `lines`).\n"
            "  verify — re-run verify.sh on the box (cmake configure + build; "
            "classify failures into the PROVISION_STATUS verdict).\n"
            "  wait   — block until on-box provisioning reaches a terminal status.\n"
            "  output — print Terraform outputs as JSON." + _REMOTE_MUTEX_NOTE
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "operation": {
                    "type": "string",
                    "enum": sorted(REMOTE_MANAGE_OPS),
                    "description": "Which provisioner.py command to run.",
                },
                "branch": {"type": "string",
                           "description": "[up] branch to clone (default: your current local branch)."},
                "region": {"type": "string",
                           "description": "[up/status/down] AWS region (default: the existing stack's region)."},
                "instance_type": {"type": "string",
                                  "description": "[up] EC2 instance type (default m4.4xlarge = 16 vCPU/64 GiB)."},
                "ami": {"type": "string",
                        "description": ("[up] override AMI id (default: newest cached "
                                        "bioseq AMI if available, otherwise latest "
                                        "Ubuntu 24.04 x86_64).")},
                "from_scratch": {
                    "type": "boolean",
                    "default": False,
                    "description": (
                        "[up] " + _FROM_SCRATCH_WARNING + " When false, `up` "
                        "uses the newest cached bioseq AMI if one exists."
                    ),
                },
                "volume_size": {"type": "integer", "minimum": 1,
                                "description": "[up] root volume GiB (default 300)."},
                "no_verify": {"type": "boolean", "default": False,
                              "description": "[up] skip the on-box build verification."},
                "wait": {"type": "boolean", "default": True,
                         "description": "[up] block until on-box provisioning finishes (default true)."},
                "timeout": {"type": "integer", "minimum": 1, "default": 120,
                            "description": "[up/wait] timeout in minutes (default 120)."},
                "yes": {"type": "boolean", "default": False,
                        "description": "[up] override the running-instance guardrail and apply anyway."},
                "lines": {"type": "integer", "minimum": 1, "default": 200,
                          "description": "[logs] number of trailing log lines to show (default 200)."},
            },
            "required": ["operation"],
        },
    },
    {
        "name": "remote-ssh",
        "description": (
            "Run a shell command on the AWS box and return its output (wraps "
            "`provisioner.py ssh`, non-interactive). This is the ONLY remote tool "
            "NOT under the remote mutex — use it as the read-only escape hatch to "
            "inspect the box (e.g. `cat /opt/bioseq/PROVISION_STATUS`, `git -C "
            "~/bioseqdb log -1`, `nvidia-smi`, `free -h`) while a long remote-run "
            "holds the slot. Synchronous and bounded by the configured remote SSH "
            "timeout (currently long enough to tolerate saturated compiles). For "
            "long-running work use remote-run / remote-manage, not this; for "
            "non-SSH diagnostics use remote-debug."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "command": {
                    "type": "string",
                    "description": ("Shell command to run on the box via `bash -lc`. "
                                    "The repo is at ~/bioseqdb."),
                },
            },
            "required": ["command"],
        },
    },
    {
        "name": "remote-snapshot",
        "description": (
            "Create a reusable cached AWS AMI from the current benchmark box root "
            "volume. This captures installed dependencies (clang/LLVM, GCC, CMake, "
            "Ninja, PostgreSQL, vcpkg) plus the existing repo/build directory so "
            "future remote provisioning can start from preexisting artifacts instead "
            "of rebuilding from scratch. Prefer running this after expensive setup or "
            "before teardown; `remote-manage` operation='down' already snapshots "
            "automatically before destroying the EC2 instance. Normal `remote-run`/"
            "`remote-manage up` calls then use the newest cached AMI by default. "
            "Avoid from_scratch=true unless the USER explicitly confirmed a clean "
            "base-image rebuild. By default AWS may reboot the instance briefly to "
            "make the image filesystem-consistent; no EC2 instance is destroyed by "
            "this tool." + _REMOTE_MUTEX_NOTE
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "region": {
                    "type": "string",
                    "description": "AWS region of the current stack (default: Terraform outputs/env).",
                },
                "name": {
                    "type": "string",
                    "description": "Optional AMI name (default bioseq-bench-aws-cache-<UTC timestamp>).",
                },
                "description": {
                    "type": "string",
                    "description": "Optional AMI description.",
                },
                "timeout": {
                    "type": "integer",
                    "minimum": 1,
                    "default": 120,
                    "description": "Wait timeout in minutes for AMI availability.",
                },
                "no_wait": {
                    "type": "boolean",
                    "default": False,
                    "description": "Return after create-image is requested instead of waiting for AMI availability.",
                },
                "no_reboot": {
                    "type": "boolean",
                    "default": False,
                    "description": (
                        "Ask AWS not to reboot the instance before imaging. Faster, "
                        "but the filesystem is only crash-consistent; leave false for "
                        "normal cache snapshots."
                    ),
                },
            },
            "required": [],
        },
    },
    {
        "name": "remote-run",
        "description": (
            "Run a build/test/benchmark or an inspect on the AWS box — the remote "
            "twin of start_build + inspect. Ensures a ready box exists (provisioning "
            "+ waiting if needed, using the newest cached bioseq AMI/snapshot by "
            "default), git-syncs the box to the requested branch's latest PUSHED "
            "commit (unless sync=false), then runs the operation on the box using "
            "the box-side server.py with the aws-x64-linux preset. " + _FROM_SCRATCH_WARNING + "\n\n"
            "Two operation shapes (oneOf):\n"
            "  operation='build' — runs one start_build mode (external-api, "
            "unit+integration, bench, bench-fast, bench-io, bench-e2e) with the same build_only/debug/"
            "filter/debug_report semantics as the local start_build tool.\n"
            "  operation='inspect' — runs the inspect tool on a box-side binary, "
            "same parameters as the local inspect tool.\n\n"
            "IMPORTANT CAVEATS:\n"
            "  - The box is x86_64 Linux. Build/test/bench modes expect the "
            "provisioned PostgreSQL + bioseq DB to be ready on the box; failures in "
            "our code should now be treated as real regressions, not as an expected "
            "::NEON/macOS dispatch.h gap.\n"
            "  - remote inspect needs a successfully-built binary on the box, so it "
            "won't return useful disassembly until the build succeeds." + _REMOTE_MUTEX_NOTE
        ),
        "inputSchema": {
            "type": "object",
            "oneOf": [
                {
                    "title": "build",
                    "type": "object",
                    "required": ["operation", "mode"],
                    "properties": {
                        "operation": {"const": "build",
                                      "description": "Run a build/test/benchmark mode."},
                        "mode": {
                            "type": "string",
                            "enum": sorted(VALID_MODES),
                            "description": ("Which run to build/execute — same modes "
                                            "as the local start_build tool."),
                        },
                        "build_only": {
                            "type": "boolean", "default": False,
                            "description": ("Only compile the targets; do not run the "
                                            "tests/benches. The reliable path on the "
                                            "box today (the build stops at the ::NEON "
                                            "gap regardless)."),
                        },
                        "debug": {
                            "type": "boolean", "default": False,
                            "description": ("Use the debug build tree (adds -g + "
                                            "-save-temps for inspect). Rarely needed "
                                            "on the box."),
                        },
                        "filter": {
                            "type": "object",
                            "additionalProperties": False,
                            "description": (
                                "Bench-only. Subset selection for bench/bench-fast/"
                                "bench-io/bench-e2e, translated to the bench binary's "
                                "repeatable --filter flag. Each present array is an "
                                "OR-set; the three arrays AND together. Omit an array "
                                "to leave that axis unconstrained. Values may not "
                                "contain ',' or '='."
                            ),
                            "properties": {
                                "tags": {
                                    "type": "array",
                                    "items": {"type": "string", "minLength": 1},
                                    "minItems": 1,
                                    "description": "Run only benchmarks whose `tags` intersect this set.",
                                },
                                "cases": {
                                    "type": "array",
                                    "items": {"type": "string", "minLength": 1},
                                    "minItems": 1,
                                    "description": "Within selected benchmarks, run only these matrix.over cases (algorithm names).",
                                },
                                "benchmarks": {
                                    "type": "array",
                                    "items": {"type": "string", "minLength": 1},
                                    "minItems": 1,
                                    "description": "Run only these benchmark keys. (Maps to the bench CLI's singular `benchmark=`.)",
                                },
                            },
                        },
                        "debug_report": {
                            "type": "boolean",
                            "default": False,
                            "description": (
                                "Bench-only. Pass --debug to the bench binary and "
                                "stream report_debug_<N>.jsonl next to report_<N>.json. "
                                "Requires a non-empty filter."
                            ),
                        },
                        **_REMOTE_RUN_COMMON_PROPS,
                    },
                },
                {
                    "title": "inspect",
                    "type": "object",
                    "required": ["operation", "function_name"],
                    "properties": {
                        "operation": {"const": "inspect",
                                      "description": "Inspect a function in a box-side binary."},
                        "function_name": {
                            "type": "string",
                            "description": ("Substring matched against demangled C++ "
                                            "names (prefix-anchored recommended), same "
                                            "as the local inspect tool."),
                        },
                        "target": {
                            "type": "string",
                            "enum": ["bioseq_bench", "bioseq_io_bench",
                                     "bioseq_aligner_tests_exec",
                                     "bioseq_aligner_int_tests_exec"],
                            "default": "bioseq_bench",
                            "description": "Which target binary to inspect on the box.",
                        },
                        "max_matches": {"type": "integer", "minimum": 1, "default": 4,
                                        "description": "Cap on returned symbols."},
                        "snippet_lines": {"type": "integer", "minimum": 1, "default": 200,
                                          "description": "Lines of asm/disasm to inline."},
                        "source_basename": {"type": "string",
                                            "description": "Source basename for the inline-site sweep."},
                        "max_llc_bc_files": {"type": "integer", "minimum": 0, "default": 4,
                                             "description": "Cap on .bc files llc-compiled for the pre-LTO view."},
                        "llc_on_bc": {"type": "boolean", "default": True,
                                      "description": "Run the llc-on-.bc fallback when no .s is present."},
                        "debug": {"type": "boolean",
                                  "description": "Force the debug/fast build tree (omit to auto-detect)."},
                        **_REMOTE_RUN_COMMON_PROPS,
                    },
                },
            ],
        },
    },
    {
        "name": "remote-git",
        "description": (
            "Proxy a git command to the box's repo (~/bioseqdb), under the remote "
            "mutex. Pass just the git subcommand and args in `command` — e.g. "
            "{\"command\": \"pull\"}, {\"command\": \"checkout my-branch\"}, "
            "{\"command\": \"fetch origin\"}, {\"command\": \"merge origin/x -X "
            "theirs\"}, {\"command\": \"reset --hard origin/main\"}. A leading 'git ' "
            "is tolerated. For read-only git inspection while another remote op is "
            "running, use remote-ssh instead (it is not under the mutex)." + _REMOTE_MUTEX_NOTE
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "command": {
                    "type": "string",
                    "description": ("git subcommand + args to run in ~/bioseqdb on the "
                                    "box, e.g. 'checkout research/aws-machine' or "
                                    "'merge origin/feature -X theirs'."),
                },
            },
            "required": ["command"],
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
        bench_filter = args.get("filter")                       # dict | None
        debug_report: bool = parse_bool("debug_report", args.get("debug_report", False))
        text = tool_start_build(mode, build_only=build_only, debug=debug,
                                bench_filter=bench_filter, debug_report=debug_report)
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
    elif name == "remote-status":
        text = tool_remote_status()
    elif name == "remote-debug":
        text = tool_remote_debug(args)
    elif name == "remote-status-ps":
        text = tool_remote_status_ps(args)
    elif name == "remote-manage":
        text = tool_remote_manage(args)
    elif name == "remote-ssh":
        text = tool_remote_ssh(args.get("command", ""))
    elif name == "remote-snapshot":
        text = tool_remote_snapshot(args)
    elif name == "remote-run":
        text = tool_remote_run(args)
    elif name == "remote-git":
        text = tool_remote_git(args.get("command", ""))
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
        "--run-build-sync",
        choices=VALID_MODES,
        default=None,
        help=(
            "(internal) reserve the state slot and run the build worker INLINE "
            "(synchronous, not detached) for the given mode. Used by the "
            "remote-run tool over SSH so the build verdict is the SSH exit "
            "status and output streams back live. Honours --build-only/--debug "
            "and the production/test BIOSEQ_*_BUILD_PRESET variables."
        ),
    )
    parser.add_argument(
        "--run-inspect",
        default=None,
        metavar="JSON",
        help=(
            "(internal) run the inspect tool with the given JSON argument object "
            "and print the JSON result to stdout. Used by remote-run over SSH to "
            "inspect a binary on the box. Honours the production/test "
            "BIOSEQ_*_BUILD_PRESET variables."
        ),
    )
    parser.add_argument(
        "--run-remote",
        default=None,
        metavar="OP_ID",
        help=(
            "(internal) run as a detached remote-operation worker for the given "
            "op_id; reads the command/payload from the remote state file and drives "
            "provisioner.py or a remote build/inspect over SSH"
        ),
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
            "(internal) if 'true' then use the selected mode's debug preset "
            "(test family for unit/integration, production family for "
            "benchmarks), adding -g + -save-temps=obj. Required for the "
            "inspect tool. Default 'false' uses that mode's lean preset."
        ),
    )
    parser.add_argument(
        "--bench-flags",
        default="[]",
        metavar="JSON",
        help=(
            "(internal) JSON array of extra argv flags appended to the bench "
            "binary (e.g. [\"--filter\",\"tags=gpu\",\"--debug\"]). Bench modes only."
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
    if args.run_remote is not None:
        return _run_remote_worker(args.run_remote)
    if args.run_build is not None:
        build_only: bool = parse_bool("build_only", args.build_only)
        debug: bool = parse_bool("debug", args.debug)
        try:
            bench_flags = json.loads(args.bench_flags)
            if not isinstance(bench_flags, list) or not all(isinstance(x, str) for x in bench_flags):
                raise ValueError("must be a JSON array of strings")
        except (ValueError, json.JSONDecodeError) as e:
            _log(f"--bench-flags invalid: {e}")
            return 2
        return _run_build_worker(args.run_build, build_only=build_only,
                                 debug=debug, bench_flags=bench_flags)
    if args.run_build_sync is not None:
        build_only: bool = parse_bool("build_only", args.build_only)
        debug: bool = parse_bool("debug", args.debug)
        try:
            bench_flags = json.loads(args.bench_flags)
            if not isinstance(bench_flags, list) or not all(isinstance(x, str) for x in bench_flags):
                raise ValueError("must be a JSON array of strings")
        except (ValueError, json.JSONDecodeError) as e:
            _log(f"--bench-flags invalid: {e}")
            return 2
        return _run_build_inline(args.run_build_sync, build_only=build_only,
                                 debug=debug, bench_flags=bench_flags)
    if args.run_inspect is not None:
        try:
            inspect_args = json.loads(args.run_inspect)
            if not isinstance(inspect_args, dict):
                raise ValueError("inspect JSON argument must be a JSON object")
        except (ValueError, json.JSONDecodeError) as e:
            print(json.dumps({"error": f"invalid --run-inspect JSON: {e}"}))
            return 2
        print(tool_inspect(inspect_args))
        return 0

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
