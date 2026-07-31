#!/usr/bin/env python3
"""bioseq build MCP server.

Speaks MCP (JSON-RPC 2.0 over stdio) with zero Python-package dependencies —
only the stdlib. It exposes local and remote build/test/benchmark tools,
binary inspection, project-document operations, subagent orchestration, and a
paper-specific CMake/PDF review pipeline. For build modes, see ``MODES``.

Concurrency rules:
    - At most one run per "lock group" at a time.
    - Runs resolving to the same CMake build directory never overlap, even when
      their modes belong to different lock groups.
    - "tests"  group covers unit+integration and external-api.
    - "cache-tests" covers cache unit and private-cluster integration modes.
    - "bench" covers the core and cache benchmark modes; concurrent reports
      and shared build trees would race.
    - Different lock groups can run in parallel unless both runs install/restart
      the shared PostgreSQL extension (unit+integration conflicts with bench-e2e).

Architecture:
    - Run with no args → MCP server on stdin/stdout.
    - Run with `--run-build <mode>` → detached build worker (this is what the
      server spawns; the worker is *not* meant to be invoked manually).
    - Run with `--run-papers-build` → detached thesis CMake worker.
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

from typing import List, Dict, Set, Optional, Any, Mapping

import argparse
import base64
import contextlib
import errno
import fcntl
import hashlib
import json
import math
import os
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

from postgres_error_safety_promotion import (  # noqa: E402
    PromotionError as PostgresErrorSafetyPromotionError,
    load_review_waiver as _load_postgres_error_safety_review_waiver,
    promote_reviewed_evidence as _promote_safety_reviewed_evidence,
    validate_review_waiver_snapshot as _validate_safety_waiver_snapshot,
)

# --------------------------- configuration ---------------------------------

PROJECT_ROOT = Path(__file__).resolve().parent.parent  # main/
WORKSPACE_ROOT = PROJECT_ROOT.parent
CMAKE_PRESETS_FILE = PROJECT_ROOT / "CMakePresets.json"
_CMAKE_CONFIGURE_STAMP_NAME = ".bioseq-build-mcp-configure.json"
_CMAKE_CONFIGURE_STAMP_KIND = "bioseq-build-mcp-configure-v1"
_CMAKE_HELPER_DIR = PROJECT_ROOT / "cmake"
if str(_CMAKE_HELPER_DIR) not in sys.path:
    sys.path.insert(0, str(_CMAKE_HELPER_DIR))

from sanitizer_runtime import (  # noqa: E402
    SANITIZER_RUNTIME_ENV,
    SanitizerRuntimeError,
    postgres_sanitizer_environment,
    sanitizer_descendant_environment,
)

# DOC0270/DOC0371 — isolated production, test, and cache build families:
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
#   "cache-opt[-debug]" — independent cache-only trees with deterministic
#                         hooks and the cache-local vcpkg manifest.
#   "cache-bioseq-opt[-debug]" — combined provider integration trees that
#                                retain the Bioseq manifest/toolchain.
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
# bench-opt tree. Explicit environment overrides remain authoritative; without
# one, Linux hosts select the existing aws-x64-linux family and non-Linux hosts
# retain the everyday DOC0270 bench-opt family. The build dir always derives from the
# preset name (every CMakePresets entry uses binaryDir ${sourceDir}/build/${presetName}),
# so overriding the preset name is sufficient — no separate dir override needed.
_HOST_DEFAULT_BUILD_PRESET = (
    "aws-x64-linux" if sys.platform.startswith("linux") else "bench-opt"
)
DEFAULT_BUILD_PRESET = os.environ.get(
    "BIOSEQ_BUILD_PRESET", _HOST_DEFAULT_BUILD_PRESET)


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
CACHE_BUILD_PRESET_OVERRIDE = os.environ.get("BIOSEQ_CACHE_BUILD_PRESET")
CACHE_DEBUG_BUILD_PRESET_OVERRIDE = os.environ.get(
    "BIOSEQ_CACHE_BUILD_PRESET_DEBUG")
CACHE_BIOSEQ_BUILD_PRESET_OVERRIDE = os.environ.get(
    "BIOSEQ_CACHE_BIOSEQ_BUILD_PRESET")
CACHE_BIOSEQ_DEBUG_BUILD_PRESET_OVERRIDE = os.environ.get(
    "BIOSEQ_CACHE_BIOSEQ_BUILD_PRESET_DEBUG")


def _external_api_build_preset(
        environment: Mapping[str, str], platform: str,
        default_build_preset: str) -> str:
    host_default = (
        "aws-x64-linux-gcc161"
        if platform.startswith("linux")
        else default_build_preset
    )
    return environment.get("BIOSEQ_EXTERNAL_API_BUILD_PRESET", host_default)


EXTERNAL_API_BUILD_PRESET = _external_api_build_preset(
    os.environ, sys.platform, DEFAULT_BUILD_PRESET)
WEBSITE_BUILD_PRESET = os.environ.get(
    "BIOSEQ_WEBSITE_BUILD_PRESET", "website-aws-x64-linux")
WEBSITE_DEBUG_BUILD_PRESET = os.environ.get(
    "BIOSEQ_WEBSITE_DEBUG_BUILD_PRESET", "website-aws-x64-linux-debug")
POSTGRES_ERROR_SAFETY_MODE = "postgres-error-safety"
POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET = (
    "postgres-error-safety-production")
POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET = (
    "postgres-error-safety-test-indexes")
POSTGRES_ERROR_SAFETY_RUNTIME_PROFILES_TARGET = (
    "bioseq_sql_postgres_error_safety_runtime_profiles")
POSTGRES_ERROR_SAFETY_CANDIDATE_TARGET = (
    "bioseq_sql_postgres_error_safety_candidate")
POSTGRES_ERROR_SAFETY_EVIDENCE_TARGET = (
    "bioseq_sql_postgres_error_safety_evidence")
POSTGRES_ERROR_SAFETY_CANDIDATE_RELATIVE_ROOT = (
    Path("bioseq")
    / "generated"
    / "sql"
    / "safety"
    / "runtime"
    / "candidate"
)
POSTGRES_ERROR_SAFETY_CASE_TEMPLATE = (
    PROJECT_ROOT
    / "codegen"
    / "safety"
    / "fixtures"
    / "doc0388_runtime_cases.json")
POSTGRES_ERROR_SAFETY_PEER_BUILD_DIR_CACHE_KEY = (
    "BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR")
DEFAULT_BUILD_DIR = PROJECT_ROOT / "build" / DEFAULT_BUILD_PRESET
DEBUG_BUILD_DIR = PROJECT_ROOT / "build" / DEBUG_BUILD_PRESET
REPORTING_DIR = PROJECT_ROOT / "reporting"


def _default_cache_preset(core_preset: str, *, combined: bool) -> str:
    """Map a complete core toolchain preset to its inherited cache variant."""
    mapped = {
        ("bench-opt", False): "cache-opt",
        ("bench-opt-debug", False): "cache-opt-debug",
        ("bench-opt", True): "cache-bioseq-opt",
        ("bench-opt-debug", True): "cache-bioseq-opt-debug",
        ("aws-x64-linux", False): "aws-x64-linux-cache",
        ("aws-x64-linux-debug", False): "aws-x64-linux-cache-debug",
        ("aws-x64-linux", True): "aws-x64-linux-cache-bioseq",
        ("aws-x64-linux-debug", True): "aws-x64-linux-cache-bioseq-debug",
        ("aws-x64-linux-gcc161", False): "aws-x64-linux-gcc161-cache",
        ("aws-x64-linux-gcc161-debug", False):
            "aws-x64-linux-gcc161-cache-debug",
        ("aws-x64-linux-gcc161", True):
            "aws-x64-linux-gcc161-cache-bioseq",
        ("aws-x64-linux-gcc161-debug", True):
            "aws-x64-linux-gcc161-cache-bioseq-debug",
    }.get((core_preset, combined))
    if mapped is not None:
        return mapped

    debug_suffix = "-debug" if core_preset.endswith("-debug") else ""
    base = core_preset.removesuffix("-debug")
    family_suffix = "-cache-bioseq" if combined else "-cache"
    return f"{base}{family_suffix}{debug_suffix}"


def preset_for(mode: str, debug: bool) -> str:
    # A mode may pin an explicit preset (e.g. external-api → GCC16 toolchain);
    # an override wins outright and is not split into debug/production families.
    mode_details = MODES.get(mode)
    if mode_details is None:
        raise RuntimeError(f"unknown build mode {mode!r}")
    if mode_details.preset_override:
        return mode_details.preset_override

    family = mode_details.preset_family
    production = DEBUG_BUILD_PRESET if debug else DEFAULT_BUILD_PRESET
    if family == "core":
        return production
    if family == "test":
        selected = TEST_DEBUG_BUILD_PRESET if debug else TEST_BUILD_PRESET
        if selected == production:
            raise RuntimeError(
                f"{mode} requires an isolated test preset; "
                f"selected test preset equals production preset {production!r}. "
                "Set BIOSEQ_TEST_BUILD_PRESET[_DEBUG] to a distinct preset "
                "whose BIOSEQ_TEST_INDEXES cache value is ON.")
        return selected
    if family == "cache":
        override = (CACHE_DEBUG_BUILD_PRESET_OVERRIDE if debug
                    else CACHE_BUILD_PRESET_OVERRIDE)
        return override or _default_cache_preset(
            production, combined=False)
    if family == "cache-bioseq":
        override = (CACHE_BIOSEQ_DEBUG_BUILD_PRESET_OVERRIDE if debug
                    else CACHE_BIOSEQ_BUILD_PRESET_OVERRIDE)
        return override or _default_cache_preset(
            production, combined=True)
    if family == "website":
        return WEBSITE_DEBUG_BUILD_PRESET if debug else WEBSITE_BUILD_PRESET
    raise RuntimeError(
        f"mode {mode!r} has unsupported preset family {family!r}")


def build_dir_for(mode: str, debug: bool) -> Path:
    return PROJECT_ROOT / "build" / preset_for(mode, debug)


def build_dirs_for(mode: str, debug: bool) -> tuple[Path, ...]:
    """Return every CMake tree a mode may configure or build.

    Ordinary modes own one tree. PostgreSQL error-safety qualification owns
    two fixed, isolated profile trees and must serialize against either one.
    """
    if mode == POSTGRES_ERROR_SAFETY_MODE:
        return (
            PROJECT_ROOT / "build" / POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
            PROJECT_ROOT / "build" / POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET,
        )
    return (build_dir_for(mode, debug),)


def bin_dir_for(mode: str, debug: bool) -> Path:
    return build_dir_for(mode, debug) / MODES[mode].binary_subdir


# Backwards-compatible aliases — the old module-level constants are still
# read by callers that haven't been updated to thread `debug` through. They
# always point at the default (non-debug) build tree.
BUILD_PRESET = DEFAULT_BUILD_PRESET
BUILD_DIR = DEFAULT_BUILD_DIR
BIN_DIR = BUILD_DIR / "bioseq"

STATE_FILE = Path("/tmp/bioseq_build_mcp_state.json")
LOCK_FILE = Path("/tmp/bioseq_build_mcp.lock")
SQL_GUARDRAIL_PUBLICATION_LEDGER = Path(
    "/tmp/bioseq_sql_guardrail_publications.json")
LOG_DIR = Path("/tmp")
PAPERS_BUILD_STATE_KEY = "papers-build"
PAPERS_DIR = PROJECT_ROOT / "papers"
PAPERS_BUILD_DIR = PAPERS_DIR / "build"
PAPERS_TARGET = "bioseqdb_thesis"
PAPERS_PDF_PATH = PAPERS_DIR / "pdf" / f"{PAPERS_TARGET}.pdf"
PAPERS_LATEX_LOG_PATH = (
    PAPERS_BUILD_DIR / PAPERS_TARGET / "_assembly" / "main.log")
PAPERS_INSPECT_ROOT = Path("/tmp/bioseq_papers_inspect")
PAPERS_BUILD_HARD_TIMEOUT_SEC = 30 * 60.0
PAPERS_INSPECT_MAX_OVERVIEW_PAGES = 80
PAPERS_INSPECT_MAX_EMBED_BYTES = 8 * 1024 * 1024

# Local machine policy for compiler-only realtime scheduling.  The enablement
# key is read exclusively from the root-owned /etc/environment; an MCP process
# environment can neither enable RT nor raise its priority.  The installed
# launcher contains the same literal priority and drops back to the invoking
# non-root uid/gid before execing either exact LLVM-22 compiler path.
LOCAL_BUILD_ENVIRONMENT_FILE = Path("/etc/environment")
LOCAL_CLANG_RT_PRIORITY_KEY = "BIOSEQ_BUILD_RT_PRIORITY"
LOCAL_CLANG_RT_PRIORITY = 1
LOCAL_CLANG_RT_LAUNCHER = Path(
    "/usr/local/libexec/bioseq-clang-rt-launcher")
LOCAL_LLVM22_CLANG = Path("/usr/lib/llvm-22/bin/clang")
LOCAL_LLVM22_CLANGXX = Path("/usr/lib/llvm-22/bin/clang++")
LOCAL_CLANG_RT_CMAKE_KEYS = (
    "CMAKE_C_COMPILER_LAUNCHER",
    "CMAKE_CXX_COMPILER_LAUNCHER",
)


def _trusted_root_owned_path_error(
        path: Path, *, executable: bool = False) -> str | None:
    """Return why ``path`` is unsafe for a privileged hand-off, if any."""
    try:
        metadata = path.stat()
    except OSError as exc:
        return f"cannot stat {path}: {exc}"
    if not stat.S_ISREG(metadata.st_mode):
        return f"{path} is not a regular file"
    if metadata.st_uid != 0:
        return f"{path} is not owned by root"
    if metadata.st_mode & 0o022:
        return f"{path} is group- or world-writable"
    if executable and not os.access(path, os.X_OK):
        return f"{path} is not executable"
    return None


def _require_trusted_root_owned_path(
        path: Path, *, executable: bool = False) -> None:
    problem = _trusted_root_owned_path_error(path, executable=executable)
    if problem is not None:
        raise RuntimeError(problem)


def _refuse_root_build_identity(context: str) -> str | None:
    """Return a fail-closed diagnostic when a build path runs as root."""
    if os.geteuid() == 0:
        return (
            f"{context} refuses to run as root; build and compiler processes "
            "must retain a non-root uid")
    return None


def _ensure_build_worker_timesharing_policy() -> None:
    """Ensure the worker itself cannot leak an inherited realtime policy.

    Linux scheduling policy is inherited across fork/exec.  A worker launched
    by an accidentally promoted parent therefore demotes itself before it can
    start CMake, Ninja, tests, or benchmarks.  Selecting SCHED_OTHER for one's
    own thread is an unprivileged reduction in scheduling privilege.
    """
    if not sys.platform.startswith("linux"):
        return
    try:
        policy = os.sched_getscheduler(0)
        if policy != os.SCHED_OTHER:
            os.sched_setscheduler(0, os.SCHED_OTHER, os.sched_param(0))
        if (os.sched_getscheduler(0) != os.SCHED_OTHER
                or os.sched_getparam(0).sched_priority != 0):
            raise RuntimeError("worker scheduler did not settle at SCHED_OTHER/0")
    except (AttributeError, OSError) as exc:
        raise RuntimeError(
            "could not enforce SCHED_OTHER/0 for the build worker") from exc


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
CACHE_EXACT2G_FINALIZATION_RESERVE_SEC = 10 * 60.0


def _safe_heavy_jobs() -> int:
    """Throttle the highest-RSS module compiles within the global job cap."""
    override = os.environ.get("BIOSEQ_BUILD_HEAVY_JOBS")
    if override:
        try:
            requested = int(override)
        except ValueError as exc:
            raise RuntimeError(
                "BIOSEQ_BUILD_HEAVY_JOBS must be a positive integer") from exc
        if requested <= 0:
            raise RuntimeError(
                "BIOSEQ_BUILD_HEAVY_JOBS must be a positive integer")
        return requested
    return max(1, min(4, CMAKE_PARALLEL_JOBS // 2))


AWS_HEAVY_JOBS = str(_safe_heavy_jobs())
AWS_DEBUG_HEAVY_JOBS = str(max(1, int(AWS_HEAVY_JOBS) // 2))
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


def _expand_cmake_preset_include(
        value: str,
        *,
        source_dir: Path,
        file_dir: Path,
        environment: Mapping[str, str]) -> Path:
    """Resolve the bounded macro set accepted in project preset includes."""
    expanded = value
    replacements = {
        "${sourceDir}": str(source_dir),
        "${fileDir}": str(file_dir),
        "${dollar}": "$",
        "${pathListSep}": os.pathsep,
    }
    for macro, replacement in replacements.items():
        expanded = expanded.replace(macro, replacement)

    def replace_environment(match: re.Match[str]) -> str:
        return environment.get(match.group(1), "")

    expanded = re.sub(
        r"\$(?:p?env)\{([A-Za-z_][A-Za-z0-9_]*)\}",
        replace_environment,
        expanded,
    )
    if re.search(r"\$\{[^}]+\}|\$(?:p?env)\{[^}]+\}", expanded):
        raise RuntimeError(
            "unsupported dynamic macro in CMake preset include "
            f"{value!r}")
    path = Path(expanded)
    return path if path.is_absolute() else file_dir / path


def _cmake_preset_payload_graph(
        presets_file: Path,
        *,
        environment: Mapping[str, str] | None = None,
) -> tuple[tuple[Path, Path, dict[str, object], bytes], ...]:
    """Load the project/user preset files and their transitive includes."""
    effective_environment = (
        os.environ if environment is None else environment)
    source_dir = presets_file.parent.resolve(strict=False)
    records: list[
        tuple[Path, Path, dict[str, object], bytes]
    ] = []
    visited: set[Path] = set()
    lexical_sources: dict[Path, Path] = {}
    active: list[Path] = []

    def visit(path: Path) -> None:
        lexical = Path(os.path.abspath(path))
        try:
            lexical_metadata = lexical.lstat()
            if (
                stat.S_ISLNK(lexical_metadata.st_mode)
                or not stat.S_ISREG(lexical_metadata.st_mode)
            ):
                raise RuntimeError(
                    "CMake preset source must be a regular non-symlink "
                    f"file: {lexical}")
            canonical = path.resolve(strict=True)
        except OSError as exc:
            raise RuntimeError(
                f"cannot resolve CMake preset include {path}: {exc}") from exc
        previous_lexical = lexical_sources.get(canonical)
        if previous_lexical is not None and previous_lexical != lexical:
            raise RuntimeError(
                "CMake preset graph aliases one file through multiple "
                f"lexical paths: {previous_lexical} and {lexical}")
        if canonical in active:
            cycle = " -> ".join(str(item) for item in (*active, canonical))
            raise RuntimeError(f"CMake preset include cycle: {cycle}")
        if canonical in visited:
            return
        lexical_sources[canonical] = lexical
        try:
            metadata = canonical.stat()
            if not stat.S_ISREG(metadata.st_mode):
                raise RuntimeError(
                    "CMake preset source must resolve to a regular file: "
                    f"{canonical}")
            body = canonical.read_bytes()
            payload = json.loads(body)
        except (OSError, json.JSONDecodeError) as exc:
            raise RuntimeError(
                f"cannot read CMake configure presets from {canonical}: "
                f"{exc}") from exc
        if not isinstance(payload, dict):
            raise RuntimeError(
                f"CMake preset source must be a regular JSON object: "
                f"{canonical}")

        active.append(canonical)
        records.append((lexical, canonical, payload, body))
        raw_includes = payload.get("include", [])
        if isinstance(raw_includes, str):
            includes = [raw_includes]
        elif (
            isinstance(raw_includes, list)
            and all(isinstance(item, str) and item for item in raw_includes)
        ):
            includes = raw_includes
        else:
            raise RuntimeError(
                f"{canonical} contains an invalid CMake preset include list")
        for include in includes:
            visit(_expand_cmake_preset_include(
                include,
                source_dir=source_dir,
                file_dir=lexical.parent,
                environment=effective_environment,
            ))
        active.pop()
        visited.add(canonical)

    visit(presets_file)
    user_presets = presets_file.with_name("CMakeUserPresets.json")
    if presets_file.name == "CMakePresets.json" and user_presets.exists():
        visit(user_presets)
    return tuple(records)


def _resolved_cmake_preset_cache_variables(
        preset: str,
        *,
        presets_file: Path = CMAKE_PRESETS_FILE,
        environment: Mapping[str, str] | None = None) -> dict[str, object]:
    """Resolve configure-preset cache variables with CMake inheritance rules.

    Earlier entries in an ``inherits`` array have priority, the selected
    preset overrides every parent, and an explicit ``null`` remains a
    tombstone so a later parent cannot restore the value.
    """
    presets: dict[str, dict] = {}
    for _, source, payload, _ in _cmake_preset_payload_graph(
            presets_file,
            environment=environment):
        raw_presets = payload.get("configurePresets", [])
        if not isinstance(raw_presets, list):
            raise RuntimeError(
                f"{source} does not contain a configurePresets array")
        for raw_preset in raw_presets:
            if not isinstance(raw_preset, dict):
                raise RuntimeError(
                    f"{source} contains a non-object configure preset")
            name = raw_preset.get("name")
            if not isinstance(name, str) or not name:
                raise RuntimeError(
                    f"{source} contains a configure preset without a name")
            if name in presets:
                raise RuntimeError(
                    f"CMake preset graph contains duplicate configure "
                    f"preset {name!r}")
            presets[name] = raw_preset

    resolved: dict[str, dict[str, object]] = {}

    def resolve(name: str, stack: tuple[str, ...]) -> dict[str, object]:
        cached = resolved.get(name)
        if cached is not None:
            return cached
        if name in stack:
            cycle = " -> ".join((*stack, name))
            raise RuntimeError(
                f"CMake configure preset inheritance cycle: {cycle}")
        raw_preset = presets.get(name)
        if raw_preset is None:
            raise RuntimeError(
                f"CMake configure preset {name!r} is not declared in "
                f"{presets_file}")

        raw_parents = raw_preset.get("inherits", [])
        if isinstance(raw_parents, str):
            parents = [raw_parents]
        elif (
            isinstance(raw_parents, list)
            and all(isinstance(parent, str) and parent
                    for parent in raw_parents)
        ):
            parents = raw_parents
        else:
            raise RuntimeError(
                f"CMake configure preset {name!r} has invalid inherits")

        variables: dict[str, object] = {}
        # Later parents are merged first so the documented earlier-parent
        # priority is preserved by the subsequent updates.
        for parent in reversed(parents):
            variables.update(resolve(parent, (*stack, name)))

        own_variables = raw_preset.get("cacheVariables", {})
        if not isinstance(own_variables, dict):
            raise RuntimeError(
                f"CMake configure preset {name!r} has invalid "
                "cacheVariables")
        for key, value in own_variables.items():
            if not isinstance(key, str) or not key:
                raise RuntimeError(
                    f"CMake configure preset {name!r} has an invalid "
                    "cache-variable name")
            variables[key] = value

        resolved[name] = variables
        return variables

    return dict(resolve(preset, ()))


def _cmake_executable_identity(
        environment: Mapping[str, str]) -> dict[str, object]:
    """Seal the exact CMake executable and version used by the worker."""
    executable = shutil.which(
        "cmake",
        path=environment.get("PATH"),
    )
    if executable is None:
        raise RuntimeError(
            "cannot resolve `cmake` from the MCP worker PATH")
    lexical = Path(executable)
    try:
        resolved = lexical.resolve(strict=True)
        metadata = resolved.stat()
        if not stat.S_ISREG(metadata.st_mode):
            raise RuntimeError(
                f"CMake executable is not a regular file: {resolved}")
        body = resolved.read_bytes()
    except OSError as exc:
        raise RuntimeError(
            f"cannot inspect CMake executable {lexical}: {exc}") from exc
    try:
        version = subprocess.run(
            [str(resolved), "--version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=10.0,
            env=dict(environment),
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise RuntimeError(
            f"cannot query CMake executable version {resolved}: {exc}") from exc
    if version.returncode != 0 or not version.stdout:
        raise RuntimeError(
            "CMake executable version query failed: "
            f"path={resolved} exit_code={version.returncode}")
    version_line = version.stdout.splitlines()[0].decode(
        "utf-8",
        errors="replace",
    )
    return {
        "lexical_path": str(lexical),
        "resolved_path": str(resolved),
        "sha256": hashlib.sha256(body).hexdigest(),
        "byte_count": len(body),
        "version_line": version_line,
        "version_output_sha256":
            hashlib.sha256(version.stdout).hexdigest(),
    }


def _cmake_configure_stamp_document(
        preset: str,
        *,
        mode_cache_variables: Mapping[str, str],
        managed_arguments: List[str],
        environment: Mapping[str, str],
        cmake_executable_identity: Mapping[str, object],
        presets_file: Path = CMAKE_PRESETS_FILE) -> dict[str, object]:
    """Seal every MCP-owned input which can select a CMake configuration."""
    preset_graph = _cmake_preset_payload_graph(
        presets_file,
        environment=environment,
    )
    resolved_cache = _resolved_cmake_preset_cache_variables(
        preset,
        presets_file=presets_file,
        environment=environment,
    )
    referenced_environment_names = sorted(set(re.findall(
        rb"\$(?:p?env)\{([A-Za-z_][A-Za-z0-9_]*)\}",
        b"\n".join(body for _, _, _, body in preset_graph),
    )))

    def sealed_environment_value(name: str) -> dict[str, object]:
        present = name in environment
        value = environment.get(name, "")
        return {
            "present": present,
            "sha256": (
                hashlib.sha256(value.encode("utf-8")).hexdigest()
                if present else None
            ),
        }

    referenced_environment = {
        name.decode("ascii"):
            sealed_environment_value(name.decode("ascii"))
        for name in referenced_environment_names
    }
    managed_environment_names = (
        "BIOSEQ_COMPILER_LAUNCHER",
        "BIOSEQ_GCC16_ROOT",
        "BIOSEQ_LIBCXX_MODULES_JSON",
        "BIOSEQ_LIBSTDCXX_MODULES_JSON",
        "BIOSEQ_LLVM_ROOT",
        "BIOSEQ_OPTTIMIZATION_LEVEL",
        "CC",
        "CXX",
        "CMAKE_EXPORT_COMPILE_COMMANDS",
        "CMAKE_NAMED_ENV_TYPE",
        "MACOSX_DEPLOYMENT_TARGET",
        "PATH",
        "POSTGRESQL_VERSION",
        "SDKROOT",
        "VCPKG_ROOT",
    )
    inputs = {
        "preset": preset,
        "presets_file": str(presets_file.resolve(strict=False)),
        "preset_files": [
            {
                "lexical_path": str(lexical),
                "resolved_path": str(resolved),
                "sha256": hashlib.sha256(body).hexdigest(),
                "byte_count": len(body),
                "ordered_include_targets": [
                    str(_expand_cmake_preset_include(
                        include,
                        source_dir=presets_file.parent.resolve(
                            strict=False),
                        file_dir=lexical.parent,
                        environment=environment,
                    ).resolve(strict=False))
                    for include in (
                        [payload["include"]]
                        if isinstance(payload.get("include", []), str)
                        else payload.get("include", [])
                    )
                ],
            }
            for lexical, resolved, payload, body in sorted(
                preset_graph,
                key=lambda record: str(record[1]),
            )
        ],
        "resolved_cache_variables": resolved_cache,
        "mode_cache_variables": dict(sorted(mode_cache_variables.items())),
        "managed_arguments": list(managed_arguments),
        "cmake_executable_identity": dict(cmake_executable_identity),
        "referenced_environment": referenced_environment,
        "managed_environment": {
            name: sealed_environment_value(name)
            for name in managed_environment_names
        },
    }
    canonical_inputs = json.dumps(
        inputs,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    return {
        "artifact_kind": _CMAKE_CONFIGURE_STAMP_KIND,
        "schema_version": 1,
        "inputs": inputs,
        "inputs_sha256": hashlib.sha256(canonical_inputs).hexdigest(),
    }


def _cmake_configure_stamp_status(
        build_dir: Path,
        expected: Mapping[str, object]) -> tuple[bool, str]:
    """Return whether an existing tree was configured from ``expected``."""
    stamp = build_dir / _CMAKE_CONFIGURE_STAMP_NAME
    try:
        metadata = stamp.lstat()
    except FileNotFoundError:
        return False, "configure stamp is missing"
    except OSError as exc:
        return False, f"configure stamp cannot be inspected: {exc}"
    if stamp.is_symlink() or not stat.S_ISREG(metadata.st_mode):
        return False, "configure stamp is not a regular non-symlink file"
    try:
        observed = json.loads(stamp.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        return False, f"configure stamp is unreadable: {exc}"
    if observed != expected:
        return False, "configure inputs changed"
    return True, "configure inputs match"


def _fsync_cmake_configure_stamp_directory(build_dir: Path) -> None:
    directory_flags = os.O_RDONLY
    directory_flags |= getattr(os, "O_DIRECTORY", 0)
    try:
        directory_descriptor = os.open(str(build_dir), directory_flags)
        try:
            os.fsync(directory_descriptor)
        finally:
            os.close(directory_descriptor)
    except OSError as exc:
        raise RuntimeError(
            "cannot durably update the CMake configure stamp directory "
            f"{build_dir}: {exc}") from exc


def _invalidate_cmake_configure_stamp(build_dir: Path) -> None:
    """Remove the old configure claim before attempting any reconfigure."""
    stamp = build_dir / _CMAKE_CONFIGURE_STAMP_NAME
    try:
        metadata = stamp.lstat()
    except FileNotFoundError:
        return
    except OSError as exc:
        raise RuntimeError(
            f"cannot inspect stale CMake configure stamp {stamp}: "
            f"{exc}") from exc
    if not (stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode)):
        raise RuntimeError(
            "refusing to remove non-file CMake configure stamp "
            f"{stamp}")
    try:
        stamp.unlink()
        _fsync_cmake_configure_stamp_directory(build_dir)
    except OSError as exc:
        raise RuntimeError(
            f"cannot invalidate CMake configure stamp {stamp}: {exc}") from exc


def _publish_cmake_configure_stamp(
        build_dir: Path,
        document: Mapping[str, object]) -> None:
    """Atomically publish a retry-safe configure stamp after validation."""
    stamp = build_dir / _CMAKE_CONFIGURE_STAMP_NAME
    build_dir.mkdir(parents=True, exist_ok=True)
    body = json.dumps(
        document,
        ensure_ascii=True,
        indent=2,
        sort_keys=True,
    ) + "\n"
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                prefix=f".{stamp.name}.",
                suffix=".tmp",
                dir=build_dir,
                delete=False) as handle:
            temporary_path = Path(handle.name)
            handle.write(body)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary_path, stamp)
        temporary_path = None
        try:
            _fsync_cmake_configure_stamp_directory(build_dir)
        except RuntimeError:
            with contextlib.suppress(OSError):
                stamp.unlink()
            with contextlib.suppress(RuntimeError):
                _fsync_cmake_configure_stamp_directory(build_dir)
            raise
    except OSError as exc:
        raise RuntimeError(
            f"cannot publish CMake configure stamp {stamp}: {exc}") from exc
    finally:
        if temporary_path is not None:
            with contextlib.suppress(OSError):
                temporary_path.unlink()


def _finalize_cmake_configure_stamp(
        build_dir: Path,
        before: Mapping[str, object],
        after: Mapping[str, object],
        *,
        context: str) -> str:
    """Reject configure-input mutation, then publish the validated seal."""
    if after != before:
        _invalidate_cmake_configure_stamp(build_dir)
        raise RuntimeError(
            "CMake configure inputs changed while the MCP worker was "
            f"configuring {context}; refusing a mixed configuration")
    final_matches, _ = _cmake_configure_stamp_status(
        build_dir,
        after,
    )
    if not final_matches:
        _publish_cmake_configure_stamp(
            build_dir,
            after,
        )
    seal = after.get("inputs_sha256")
    if not isinstance(seal, str) or not re.fullmatch(r"[0-9a-f]{64}", seal):
        _invalidate_cmake_configure_stamp(build_dir)
        raise RuntimeError(
            f"CMake configure stamp for {context} has no valid input seal")
    return seal


def _cmake_preset_cache_contract(
        preset: str,
        keys: tuple[str, ...],
        *,
        presets_file: Path = CMAKE_PRESETS_FILE,
        environment: Mapping[str, str] | None = None) -> dict[str, str]:
    """Return exact, non-null values selected by a configure preset."""
    variables = _resolved_cmake_preset_cache_variables(
        preset,
        presets_file=presets_file,
        environment=environment,
    )
    contract: dict[str, str] = {}
    for key in keys:
        raw_value = variables.get(key)
        if raw_value is None:
            continue
        if isinstance(raw_value, bool):
            value = "TRUE" if raw_value else "FALSE"
        elif isinstance(raw_value, str):
            value = raw_value
        elif isinstance(raw_value, dict):
            value = raw_value.get("value")
            if isinstance(value, bool):
                value = "TRUE" if value else "FALSE"
            elif not isinstance(value, str):
                raise RuntimeError(
                    f"CMake configure preset {preset!r} has invalid value "
                    f"for cache variable {key}")
        else:
            raise RuntimeError(
                f"CMake configure preset {preset!r} has invalid value for "
                f"cache variable {key}")
        contract[key] = value
    return contract


def _cmake_preset_cache_repairs(
        build_dir: Path,
        preset: str,
        *,
        keys: tuple[str, ...] = ("PostgreSQL_CONFIG",),
        presets_file: Path = CMAKE_PRESETS_FILE,
        environment: Mapping[str, str] | None = None) -> dict[str, str]:
    """Plan exact repairs for preset-owned cache values in one build tree."""
    contract = _cmake_preset_cache_contract(
        preset,
        keys,
        presets_file=presets_file,
        environment=environment,
    )
    return {
        key: expected
        for key, expected in contract.items()
        if _cmake_cache_value(build_dir, key) != expected
    }


_POSTGRESQL_DISCOVERY_CACHE_UNSET_ARGS = (
    "-UPostgreSQL_*",
    "-U_PostgreSQL_*",
    "-UFIND_PACKAGE_MESSAGE_DETAILS_PostgreSQL",
)


def _postgresql_discovery_cache_reset_args(
        build_dir: Path,
        preset: str,
        *,
        presets_file: Path = CMAKE_PRESETS_FILE,
        environment: Mapping[str, str] | None = None) -> tuple[str, ...]:
    """Invalidate derived PostgreSQL discovery when its toolchain is stale."""
    contract = _cmake_preset_cache_contract(
        preset,
        ("PostgreSQL_CONFIG",),
        presets_file=presets_file,
        environment=environment,
    )
    expected_config = contract.get("PostgreSQL_CONFIG")
    if expected_config is None:
        return ()
    cached_config = _cmake_cache_value(
        build_dir,
        "PostgreSQL_CONFIG",
    )
    cached_server = _cmake_cache_value(
        build_dir,
        "PostgreSQL_EXECUTABLE",
    )
    if cached_config != expected_config or cached_server is None:
        return _POSTGRESQL_DISCOVERY_CACHE_UNSET_ARGS

    expected_bin = Path(expected_config).parent
    cached_bin = Path(cached_server).parent
    try:
        same_bin = (
            cached_bin == expected_bin
            or cached_bin.resolve(strict=False)
                == expected_bin.resolve(strict=False)
        )
    except (OSError, RuntimeError):
        same_bin = False
    return () if same_bin else _POSTGRESQL_DISCOVERY_CACHE_UNSET_ARGS


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
    _invalidate_cmake_configure_stamp(build_dir)
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


def _run_cmake_preset_configure(
        arguments: List[str],
        *,
        build_dir: Path,
        log_handle,
        deadline: float,
        environment: Mapping[str, str]) -> int:
    """Run the sole MCP configure path after retiring any older seal."""
    if len(arguments) < 3 or arguments[:2] != ["cmake", "--preset"]:
        raise ValueError(
            "CMake preset configure command must start with "
            "`cmake --preset`")
    _invalidate_cmake_configure_stamp(build_dir)
    return _stream_subprocess(
        arguments,
        log_handle,
        deadline,
        cwd=PROJECT_ROOT,
        env=dict(environment),
    )


def _ensure_cmake_preset_tree_fresh(
        *,
        build_dir: Path,
        preset: str,
        configure_stamp: Mapping[str, object],
        configure_arguments: List[str],
        log_handle,
        deadline: float,
        environment: Mapping[str, str],
        failure_label: str = "cmake configure") -> bool:
    """Fresh-configure a tree iff Ninja and the MCP input seal disagree."""
    build_ninja = build_dir / "build.ninja"
    stamp_matches, stamp_reason = _cmake_configure_stamp_status(
        build_dir,
        configure_stamp,
    )
    if build_ninja.is_file() and stamp_matches:
        return False

    _invalidate_cmake_configure_stamp(build_dir)
    cache = build_dir / "CMakeCache.txt"
    cmake_files = build_dir / "CMakeFiles"
    if (
        cache.exists()
        or cache.is_symlink()
        or cmake_files.exists()
        or cmake_files.is_symlink()
    ):
        reason = (
            stamp_reason
            if build_ninja.exists()
            else "previous configure did not produce build.ninja"
        )
        _clear_cmake_configure_cache(
            build_dir,
            log_handle,
            reason,
        )
    log_handle.write(
        "# CMake configure freshness mismatch: "
        f"{stamp_reason}; reconfiguring with preset {preset}.\n")
    log_handle.flush()
    rc = _run_cmake_preset_configure(
        configure_arguments,
        build_dir=build_dir,
        log_handle=log_handle,
        deadline=deadline,
        environment=environment,
    )
    if rc != 0:
        raise RuntimeError(f"{failure_label} exited with code {rc}")
    if not build_ninja.is_file():
        raise RuntimeError(
            f"preset {preset!r} did not produce {build_ninja}")
    return True


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
    group. Modes resolving to the same CMake build directory also conflict
    across lock groups. Runtime modes that install/restart the shared PostgreSQL
    instance conflict as well, while unrelated build-only runs remain
    independent.
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
    basename — resolved against the active mode's `binary_subdir` directory
    at run time (see `bin_dir_for(mode, debug)`). This lets the same MODE
    definition drive isolated production, test, cache-only, and combined
    preset families, including their debug/inspect variants.
    """
    commands: List[List[str]]
    """Extra documentation"""
    docs: str
    """Optional subdirectory within REPORTING_DIR for a native run report."""
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
    """Preset family: core, isolated core tests, cache-only, or combined."""
    preset_family: str = "core"
    """Subdirectory below the active build tree containing command binaries."""
    binary_subdir: str = "bioseq"
    """CMake-owned test/benchmark targets run only after ordinary compilation."""
    post_build_targets: tuple[str, ...] = ()
    """True when all PostgreSQL work occurs in a disposable private cluster."""
    uses_private_cluster: bool = False
    """Optional mode-wide wall-clock ceiling, including build and cleanup."""
    hard_timeout_min: Optional[float] = None

    def __post_init__(self) -> None:
        if self.preset_family not in {
                "core", "test", "cache", "cache-bioseq", "website"}:
            raise ValueError(
                f"unsupported preset family {self.preset_family!r}")
        if self.uses_private_cluster and self.install_extension:
            raise ValueError(
                "private-cluster modes must not install/restart the shared "
                "PostgreSQL extension")
        if self.hard_timeout_min is not None and self.hard_timeout_min <= 0:
            raise ValueError("mode hard_timeout_min must be positive")

    @property
    def verb_is(self) -> str:
        return "are" if self.is_verb_plular else "is"

    @property
    def verb_was(self) -> str:
        return "were" if self.is_verb_plular else "was"

    @property
    def reporting_dir(self) -> Path:
        if not self.reporting_subdir_name:
            return REPORTING_DIR / "__does_not_exist__"
        return REPORTING_DIR / self.reporting_subdir_name

MODES: Dict[str, ModeDefinition] = {
    "external-api": ModeDefinition(
        lock_group="tests",
        expected_duration_min=120.0,
        build_only_expected_duration_min=60.0,
        label="External API and PostgreSQL integration tests",
        docs=(
            "Build and run generated external API clients, then install the "
            "external-enabled generated extension package in a clean private "
            "PostgreSQL 17 cluster and emit a hash-sealed catalog/behavior "
            "acceptance receipt"),
        is_verb_plular=True,
        targets=[
            "bioseq_external_integration_tests_exec",
            "postgresql_bioseq",
        ],
        commands=[
            ["bioseq_external_integration_tests_exec",
             "--reporter", "console::out=-::colour-mode=ansi"],
        ],
        post_build_targets=("bioseq_external_postgres_acceptance",),
        uses_private_cluster=True,
        preset_override=EXTERNAL_API_BUILD_PRESET,
        required_cache_variables={
            "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "ON",
            "BIOSEQ_BUILD_WEBSITE": "OFF",
            "BIOSEQ_WEBSITE_BROWSER_TESTS": "OFF",
        },
    ),
    "website": ModeDefinition(
        lock_group="website",
        expected_duration_min=15.0,
        build_only_expected_duration_min=8.0,
        label="Static documentation website",
        docs=(
            "Build the isolated website code generator (GCC 16.1 ExternalProject) "
            "against the Clang-22 core tree and stage the relocatable postgresql_bioseq "
            "extension, then run the private-cluster runner test and the bioseq_website "
            "target: Doxygen + Clang index, private-cluster runnable SQL, render, "
            "independent html5lib/§20.7 validation, pinned-browser acceptance, and the "
            "atomic publish. build_only stops at the generator for fast iteration."),
        is_verb_plular=False,
        # The generator plus the extension the site stages into its private cluster.
        targets=["bioseq_codegen_project", "postgresql_bioseq"],
        commands=[],
        # §17.3/§17.5: on a full (non-build_only) run, drive the private-cluster
        # runner test and then the whole render/validate/browser/publish pipeline.
        # bioseq_website self-stages its own private PostgreSQL, so no external
        # cluster is provisioned here; uses_private_cluster is the reporting label.
        post_build_targets=("bioseq_private_postgres_tests", "bioseq_website"),
        uses_private_cluster=True,
        preset_family="website",
        binary_subdir="website",
        # §726/§17.5: the website MCP mode forces the pinned-browser acceptance layer
        # ON. It runs only when the full bioseq_website target is built (the browser
        # step degrades to a clean SKIP when Chromium is unprovisioned), so enabling it
        # here never affects the generator-only build_only iteration path.
        required_cache_variables={
            "BIOSEQ_BUILD_WEBSITE": "ON",
            "BIOSEQ_WEBSITE_BROWSER_TESTS": "ON",
            "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
        },
    ),
    POSTGRES_ERROR_SAFETY_MODE: ModeDefinition(
        lock_group=POSTGRES_ERROR_SAFETY_MODE,
        expected_duration_min=180.0,
        build_only_expected_duration_min=30.0,
        label="PostgreSQL error-safety qualification",
        docs=(
            "Build isolated production and test-index extension profiles, "
            "seal their runtime plans, and validate both in private "
            "PostgreSQL clusters"),
        is_verb_plular=False,
        targets=[],
        commands=[],
        preset_override=POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
        uses_private_cluster=True,
        required_cache_variables={
            "BIOSEQ_BUILD_TESTS": "OFF",
            "BIOSEQ_TEST_INDEXES": "OFF",
            "BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE":
                str(POSTGRES_ERROR_SAFETY_CASE_TEMPLATE),
            "BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE": "isolated",
            POSTGRES_ERROR_SAFETY_PEER_BUILD_DIR_CACHE_KEY: str(
                PROJECT_ROOT
                / "build"
                / POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET),
            "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
            "BIOSEQ_BUILD_WEBSITE": "OFF",
        },
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
        targets=["postgresql_bioseq_cache", "postgresql_bioseq",
                 "bioseq_aligner_tests_exec", "bioseq_aligner_int_tests_exec",
                 "bioseq_bench_e2e_tests_exec",
                 "bioseq_bench_framework_example_tests_exec"],
        install_extension=True,
        commands=[
            ["bioseq_aligner_tests_exec",
             "--reporter", "console::out=-::colour-mode=ansi"],
            ["bioseq_aligner_int_tests_exec",
             "--reporter", "console::out=-::colour-mode=ansi"],
            ["bioseq_bench_e2e_tests_exec",
             "--reporter", "console::out=-::colour-mode=ansi"],
            ["bioseq_bench_framework_example_tests_exec",
             "--reporter", "console::out=-::colour-mode=ansi"],
        ],
        preset_family="test",
        # Optional component modes may be deliberately pointed at this test
        # preset for local qualification.  Restore the ordinary test tree
        # before any unit/integration target is built so their sticky CMake
        # cache entries cannot pull unavailable website/codegen toolchains
        # into an otherwise incremental test run.
        required_cache_variables={
            "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
            "BIOSEQ_BUILD_WEBSITE": "OFF",
        },
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
        targets=["postgresql_bioseq_cache", "postgresql_bioseq",
                 "bioseq_bench_e2e"],
        install_extension=True,
        commands=[["bioseq_bench_e2e"]],
        reporting_subdir_name="bioseq_bench_e2e",
        is_bench_mode=True,
    ),
    "cache-unit": ModeDefinition(
        lock_group="cache-tests",
        expected_duration_min=2.0,
        build_only_expected_duration_min=1.0,
        label="bioseq_cache unit tests",
        docs="Build bioseq_cache and run no-Postgres unit tests",
        is_verb_plular=True,
        preset_family="cache",
        binary_subdir="bioseq_caching",
        targets=[
            "postgresql_bioseq_cache",
            "bioseq_cache_unit_tests",
            "bioseq_postgres_test_support_contract_tests",
            "bioseq_cache_benchmark_report_contract_tests",
            "bioseq_durable_file_io_contract_tests",
            "bioseq_build_mcp_configure_freshness_contract_tests",
            "bioseq_cache_legacy_retirement_gate",
            "bioseq_cache_legacy_inventory_tracking",
        ],
        commands=[
            ["bioseq_cache_unit_tests",
             "--reporter", "console::out=-::colour-mode=ansi"],
            ["bioseq_postgres_test_support_contract_tests",
             "--reporter", "console::out=-::colour-mode=ansi"],
            ["bioseq_cache_benchmark_report_contract_tests",
             "--reporter", "console::out=-::colour-mode=ansi"],
            ["bioseq_durable_file_io_contract_tests",
             "--reporter", "console::out=-::colour-mode=ansi"],
            [
                "bioseq_cache_legacy_inventory_tracking",
                "--legacy-root", str(PROJECT_ROOT / "bioseq_caching"),
                "--legacy-inventory", "{legacy_inventory}",
                "--phase", "current",
                "--inventory-only",
            ],
        ],
    ),
    "cache-integration": ModeDefinition(
        lock_group="cache-tests",
        expected_duration_min=8.0,
        build_only_expected_duration_min=1.0,
        label="bioseq_cache temporary-cluster tests",
        docs="Run native cache Catch2 integration tests in a private cluster",
        is_verb_plular=True,
        preset_family="cache",
        binary_subdir="bioseq_caching",
        targets=[
            "postgresql_bioseq_cache",
            "postgresql_bioseq_cache_test_support",
            "bioseq_cache_integration_tests",
        ],
        commands=[
            [
                "bioseq_cache_integration_tests",
                "--postgres-bin-dir",
                "{cache_test_support_postgres_bin_dir}",
                "--postgres-stage-root",
                "{cache_test_support_postgres_stage_root}",
                "--postgres-stage-manifest",
                "{cache_test_support_postgres_stage_manifest}",
                "--cache-control", "{cache_test_control}",
                "--cache-install-sql", "{cache_test_install_sql}",
                "--cache-dso", "{cache_test_dso}",
                "--migration-ledger", "{migration_ledger}",
                "--migration-harness", "cache-integration",
                "--migration-report-role", "cache-integration",
                "--report", "{report}",
                "--case-timeout-seconds", "480",
                "--reporter", "console::out=-::colour-mode=ansi",
            ],
        ],
        reporting_subdir_name="bioseq_cache_integration",
        uses_private_cluster=True,
    ),
    "cache-bench": ModeDefinition(
        lock_group="bench",
        expected_duration_min=20.0,
        build_only_expected_duration_min=1.0,
        label="bioseq_cache benchmark",
        docs="Run cache core and private-cluster SQL benchmarks",
        is_verb_plular=False,
        preset_family="cache",
        binary_subdir="bioseq_caching",
        targets=["postgresql_bioseq_cache", "bioseq_cache_bench"],
        commands=[
            [
                "bioseq_cache_bench",
                "--postgres-bin-dir",
                "{cache_production_postgres_bin_dir}",
                "--postgres-stage-root",
                "{cache_production_postgres_stage_root}",
                "--postgres-stage-manifest",
                "{cache_production_postgres_stage_manifest}",
                "--cache-control", "{cache_control}",
                "--cache-install-sql", "{cache_install_sql}",
                "--cache-dso", "{cache_dso}",
                "--migration-ledger", "{migration_ledger}",
                "--scenario", "standalone",
                "--samples", "7",
                "--warmup-samples", "1",
                "--report", "{report}",
            ],
        ],
        reporting_subdir_name="bioseq_cache_bench",
        is_bench_mode=True,
        uses_private_cluster=True,
    ),
    "cache-bioseq-integration": ModeDefinition(
        lock_group="cache-tests",
        expected_duration_min=120.0,
        build_only_expected_duration_min=10.0,
        label="bioseq_cache provider integration tests",
        docs=(
            "Run support-profile Bioseq/cache diagnostics, then gate the exact "
            "2 Gbp case against both shipping production extensions"
        ),
        is_verb_plular=True,
        preset_family="cache-bioseq",
        binary_subdir="bioseq_caching",
        targets=[
            "postgresql_bioseq_cache",
            "postgresql_bioseq_cache_test_support",
            "postgresql_bioseq",
            "bioseq_cache_bioseq_support_integration_tests",
            "bioseq_cache_bioseq_production_integration_tests",
        ],
        commands=[
            [
                "bioseq_cache_bioseq_support_integration_tests",
                "--postgres-bin-dir",
                "{cache_bioseq_support_postgres_bin_dir}",
                "--postgres-stage-root",
                "{cache_bioseq_support_postgres_stage_root}",
                "--postgres-stage-manifest",
                "{cache_bioseq_support_postgres_stage_manifest}",
                "--bioseq-only-postgres-bin-dir",
                "{cache_bioseq_only_postgres_bin_dir}",
                "--bioseq-only-postgres-stage-root",
                "{cache_bioseq_only_postgres_stage_root}",
                "--bioseq-only-postgres-stage-manifest",
                "{cache_bioseq_only_postgres_stage_manifest}",
                "--cache-control", "{cache_test_control}",
                "--cache-install-sql", "{cache_test_install_sql}",
                "--cache-dso", "{cache_test_dso}",
                "--bioseq-control", "{bioseq_control}",
                "--bioseq-install-sql", "{bioseq_install_sql}",
                "--bioseq-dso", "{bioseq_dso}",
                "--provider-peer-test-dso", "{provider_peer_test_dso}",
                "--migration-ledger", "{migration_ledger}",
                "--migration-harness", "cache-bioseq-integration",
                "--migration-report-role",
                "cache-bioseq-support-integration",
                "--report", "{report}",
                "--case-timeout-seconds", "480",
                "--reporter", "console::out=-::colour-mode=ansi",
            ],
            [
                "bioseq_cache_bioseq_production_integration_tests",
                "--postgres-bin-dir",
                "{cache_bioseq_production_postgres_bin_dir}",
                "--postgres-stage-root",
                "{cache_bioseq_production_postgres_stage_root}",
                "--postgres-stage-manifest",
                "{cache_bioseq_production_postgres_stage_manifest}",
                "--cache-control", "{cache_control}",
                "--cache-install-sql", "{cache_install_sql}",
                "--cache-dso", "{cache_dso}",
                "--bioseq-control", "{bioseq_control}",
                "--bioseq-install-sql", "{bioseq_install_sql}",
                "--bioseq-dso", "{bioseq_dso}",
                "--migration-ledger", "{migration_ledger}",
                "--migration-harness", "cache-bioseq-integration",
                "--migration-report-role",
                "cache-bioseq-production-integration",
                "--report", "{report}",
                "--case-timeout-seconds", "480",
                "--large-case-timeout-seconds", "6300",
                "--reporter", "console::out=-::colour-mode=ansi",
            ],
        ],
        reporting_subdir_name="bioseq_cache_bioseq_integration",
        uses_private_cluster=True,
        hard_timeout_min=120.0,
    ),
    "cache-bioseq-bench": ModeDefinition(
        lock_group="bench",
        expected_duration_min=180.0,
        build_only_expected_duration_min=10.0,
        label="bioseq_cache provider benchmark",
        docs="Run Bioseq cache benchmarks with both DSOs in a private cluster",
        is_verb_plular=False,
        preset_family="cache-bioseq",
        binary_subdir="bioseq_caching",
        targets=[
            "postgresql_bioseq_cache",
            "postgresql_bioseq",
            "bioseq_cache_bioseq_bench",
            "bioseq_cache_legacy_retirement_gate",
        ],
        commands=[
            [
                "bioseq_cache_bioseq_bench",
                "--postgres-bin-dir",
                "{cache_bioseq_production_postgres_bin_dir}",
                "--postgres-stage-root",
                "{cache_bioseq_production_postgres_stage_root}",
                "--postgres-stage-manifest",
                "{cache_bioseq_production_postgres_stage_manifest}",
                "--cache-control", "{cache_control}",
                "--cache-install-sql", "{cache_install_sql}",
                "--cache-dso", "{cache_dso}",
                "--bioseq-control", "{bioseq_control}",
                "--bioseq-install-sql", "{bioseq_install_sql}",
                "--bioseq-dso", "{bioseq_dso}",
                "--migration-ledger", "{migration_ledger}",
                "--scenario", "bioseq-2gbp",
                "--samples", "3",
                "--warmup-samples", "1",
                "--report", "{report}",
            ],
        ],
        reporting_subdir_name="bioseq_cache_bioseq_bench",
        is_bench_mode=True,
        uses_private_cluster=True,
    ),
}
VALID_MODES: Set[str] = set(MODES.keys())
# These modes intentionally depend on local-only build trees or host resources.
# Keep them out of the remote schema and reject direct remote-run calls before
# any provisioning, synchronization, or worker dispatch occurs.
REMOTE_RUN_REJECTED_MODES: Set[str] = {
    "website",
    POSTGRES_ERROR_SAFETY_MODE,
}
REMOTE_RUN_BUILD_MODES: tuple[str, ...] = tuple(sorted(
    VALID_MODES - REMOTE_RUN_REJECTED_MODES))
BENCH_CLI_MODES: tuple[str, ...] = tuple(sorted(
    mode for mode, details in MODES.items()
    if (details.is_bench_mode and details.commands
        and not mode.startswith("cache-"))
))


@dataclass(frozen=True)
class InspectTargetDefinition:
    mode: str
    artifact_names: tuple[str, ...]
    shared_target_names: tuple[str, ...] = ()


INSPECT_TARGETS: Dict[str, InspectTargetDefinition] = {
    "bioseq_bench": InspectTargetDefinition(
        mode="bench", artifact_names=("bioseq_bench",),
        shared_target_names=("bioseq_modules_nopg",)),
    "bioseq_io_bench": InspectTargetDefinition(
        mode="bench", artifact_names=("bioseq_io_bench",),
        shared_target_names=("bioseq_modules_nopg",)),
    "bioseq_aligner_tests_exec": InspectTargetDefinition(
        mode="unit+integration", artifact_names=("bioseq_aligner_tests_exec",),
        shared_target_names=("bioseq_modules_nopg",)),
    "bioseq_aligner_int_tests_exec": InspectTargetDefinition(
        mode="unit+integration",
        artifact_names=("bioseq_aligner_int_tests_exec",),
        shared_target_names=("bioseq_modules_nopg",)),
    "bioseq_bench_e2e_tests_exec": InspectTargetDefinition(
        mode="unit+integration",
        artifact_names=("bioseq_bench_e2e_tests_exec",),
        shared_target_names=("bioseq_modules_nopg",)),
    "postgresql_bioseq": InspectTargetDefinition(
        mode="bench-e2e",
        artifact_names=("bioseq.dylib", "bioseq.so", "bioseq.dll"),
        shared_target_names=("bioseq_modules_nopg",)),
    "postgresql_bioseq_cache": InspectTargetDefinition(
        mode="cache-unit",
        artifact_names=("bioseq_cache.dylib", "bioseq_cache.so",
                        "bioseq_cache.dll"),
        shared_target_names=("bioseq_cache_core",)),
    "bioseq_cache_unit_tests": InspectTargetDefinition(
        mode="cache-unit", artifact_names=("bioseq_cache_unit_tests",),
        shared_target_names=("bioseq_cache_core",)),
    "bioseq_cache_bench": InspectTargetDefinition(
        mode="cache-bench", artifact_names=("bioseq_cache_bench",),
        shared_target_names=("bioseq_cache_benchmark_support",)),
    "bioseq_cache_bioseq_bench": InspectTargetDefinition(
        mode="cache-bioseq-bench",
        artifact_names=("bioseq_cache_bioseq_bench",),
        shared_target_names=("bioseq_cache_benchmark_support",)),
}


def get_modes_short_documentation() -> str:
    modes_list_str = ", ".join(
        f"'{mode}' (~{int(MODES[mode].expected_duration_min)} min and "
        f"~{int(MODES[mode].build_only_expected_duration_min)} min for build-only)"
        for mode in sorted(VALID_MODES)
    )
    return f"Modes: {modes_list_str}"


def post_build_targets_for(mode: str, build_only: bool) -> tuple[str, ...]:
    """Return CMake-owned run targets, suppressing all execution for build-only."""
    if build_only:
        return ()
    return MODES[mode].post_build_targets


def supports_bench_cli_options(mode: str) -> bool:
    """Whether filter/debug_report can be forwarded to a mode's CLI."""
    return mode in BENCH_CLI_MODES


def _log(msg: str) -> None:
    """Server-side log line. Always stderr — stdout is JSON-RPC only."""
    print(f"[bioseq-build-mcp] {msg}", file=sys.stderr, flush=True)


# --------------------------- state file I/O --------------------------------

_STATE_THREAD_LOCK = threading.RLock()
_STATE_LOCK_CONTEXT = threading.local()


@contextlib.contextmanager
def _global_lock():
    """Reentrant thread/process lock around state-file mutations.

    MCP servers are spawned per Claude Code session, so two sessions can
    race on start_build for the same mode. The lock makes the read-check-
    spawn-write critical section atomic across processes. A process-local
    RLock and thread-local depth make nested state helpers safe: opening a
    second independent fd and calling flock again can otherwise deadlock the
    calling process against its own first lock.
    """
    with _STATE_THREAD_LOCK:
        depth = getattr(_STATE_LOCK_CONTEXT, "depth", 0)
        if depth:
            _STATE_LOCK_CONTEXT.depth = depth + 1
            try:
                yield
            finally:
                _STATE_LOCK_CONTEXT.depth = depth
            return

        LOCK_FILE.parent.mkdir(parents=True, exist_ok=True)
        flags = os.O_RDWR | os.O_CREAT
        flags |= getattr(os, "O_CLOEXEC", 0)
        fd = os.open(str(LOCK_FILE), flags, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX)
            _STATE_LOCK_CONTEXT.depth = 1
            yield
        finally:
            _STATE_LOCK_CONTEXT.depth = 0
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


def _save_state_locked(state: dict) -> None:
    """Durably replace the state file while `_global_lock` is held."""
    if getattr(_STATE_LOCK_CONTEXT, "depth", 0) <= 0:
        raise RuntimeError(
            "_save_state_locked requires the global state lock")
    STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                prefix=f".{STATE_FILE.name}.",
                suffix=".tmp",
                dir=STATE_FILE.parent,
                delete=False) as handle:
            temporary_path = Path(handle.name)
            json.dump(state, handle, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary_path, STATE_FILE)
        temporary_path = None

        directory_flags = os.O_RDONLY
        directory_flags |= getattr(os, "O_DIRECTORY", 0)
        directory_fd = os.open(str(STATE_FILE.parent), directory_flags)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        if temporary_path is not None:
            try:
                temporary_path.unlink()
            except FileNotFoundError:
                pass


def _save_state(state: dict) -> None:
    """Durably replace the complete state under the reentrant global lock."""
    with _global_lock():
        _save_state_locked(state)


def _update_entry(mode: str, **fields) -> None:
    """Atomically merge fields without losing another worker's state."""
    with _global_lock():
        state = _load_state()
        entry = dict(state.get(mode) or {})
        entry.update(fields)
        state[mode] = entry
        _save_state_locked(state)


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


def _stream_subprocess(
        cmd: list[str],
        log_handle,
        deadline: float,
        cwd: Path | None = None,
        env: dict | None = None,
        *,
        verified_executable: Path | None = None,
) -> int:
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
        executable=(
            str(verified_executable)
            if verified_executable is not None else None
        ),
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
        if proc.stdout is not None:
            proc.stdout.close()


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
    if (not sys.platform.startswith("linux")
            or not _is_aws_preset(active_preset)):
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


def _run_postgresql_restart_cmd(
        restart_cmd: list[str], log_handle,
        *, env: dict[str, str] | None = None) -> None:
    pretty = " ".join(restart_cmd)
    log_handle.write(f"\n+ {pretty}\n")
    log_handle.flush()
    sys.stderr.write(f"\n+ {pretty}\n")
    sys.stderr.flush()
    try:
        result = subprocess.run(
            restart_cmd,
            env=env,
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


def _is_homebrew_arm64_pg_ctl(pg_ctl: str) -> bool:
    """Whether pg_ctl is from the native /opt/homebrew arm64 installation."""
    try:
        Path(pg_ctl).resolve(strict=False).relative_to(Path("/opt/homebrew"))
        return True
    except (OSError, RuntimeError, ValueError):
        return False


def _darwin_postgresql_restart_argv(
        pg_ctl: str, data_dir: str) -> list[str]:
    """Keep Postgres.app native via arch, but never scrub Homebrew dyld env."""
    command = [pg_ctl, "-D", data_dir, "restart"]
    if _is_homebrew_arm64_pg_ctl(pg_ctl):
        return command
    return ["arch", "-arm64", *command]


def _build_worker_descendant_environment(
        base_env: dict[str, str] | None = None,
        *, allowed_paths=None) -> dict[str, str]:
    """Validate the neutral selector and strip raw dyld state for workers."""
    try:
        validation = {} if allowed_paths is None else {
            "allowed_paths": allowed_paths}
        return sanitizer_descendant_environment(
            os.environ if base_env is None else base_env, **validation)
    except SanitizerRuntimeError as exc:
        raise RuntimeError(str(exc)) from exc


def _postgresql_restart_environment(
        base_env: dict[str, str] | None = None,
        *, allowed_paths=None) -> dict[str, str]:
    """Strip ambient dyld injection and map one validated runtime at PG only."""
    try:
        validation = {} if allowed_paths is None else {
            "allowed_paths": allowed_paths}
        clean = sanitizer_descendant_environment(
            os.environ if base_env is None else base_env, **validation)
        if sys.platform == "darwin":
            return postgres_sanitizer_environment(clean, **validation)
        return clean
    except SanitizerRuntimeError as exc:
        raise RuntimeError(str(exc)) from exc


def _trusted_root_owned_executable(path: str) -> Path:
    """Resolve and validate an executable before crossing to another OS uid."""
    try:
        resolved = Path(path).resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise RuntimeError(f"cannot resolve trusted executable {path}: {exc}") from exc
    _require_trusted_root_owned_path(resolved, executable=True)
    for directory in resolved.parents:
        try:
            metadata = directory.stat()
        except OSError as exc:
            raise RuntimeError(
                f"cannot stat executable parent {directory}: {exc}") from exc
        if metadata.st_uid != 0 or metadata.st_mode & 0o022:
            raise RuntimeError(
                f"executable parent {directory} is not root-owned and "
                "non-writable")
    return resolved


def _query_postgresql_data_directory(psql: str, active_preset: str) -> str:
    """Query directly, with a narrowly validated AWS postgres-uid fallback."""
    argv = [psql, "-t", "-c", "SHOW data_directory;", "postgres"]
    direct_detail = ""
    try:
        direct = subprocess.run(
            argv, capture_output=True, text=True, timeout=10)
        if direct.returncode == 0:
            return direct.stdout.strip()
        direct_detail = (direct.stderr or direct.stdout).strip()
    except (OSError, subprocess.TimeoutExpired) as exc:
        direct_detail = str(exc)

    if not _is_aws_preset(active_preset):
        raise RuntimeError(
            "direct psql SHOW data_directory failed: "
            f"{direct_detail or 'unknown error'}")

    trusted_psql = _trusted_root_owned_executable(psql)
    trusted_sudo = _trusted_root_owned_executable("/usr/bin/sudo")
    try:
        fallback = subprocess.run(
            [str(trusted_sudo), "-n", "-u", "postgres", "--",
             str(trusted_psql), "-t", "-c", "SHOW data_directory;",
             "postgres"],
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(
            "direct psql failed and trusted AWS postgres-uid fallback could "
            f"not launch (direct: {direct_detail or 'unknown error'}; "
            f"fallback: {exc})") from exc
    if fallback.returncode != 0:
        fallback_detail = (fallback.stderr or fallback.stdout).strip()
        raise RuntimeError(
            "direct psql and trusted AWS postgres-uid fallback failed "
            f"(direct: {direct_detail or 'unknown error'}; "
            f"fallback: {fallback_detail or f'exit {fallback.returncode}'})")
    return fallback.stdout.strip()


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
        data_dir = _query_postgresql_data_directory(psql, active_preset)
    except Exception as exc:
        msg = f"could not query data_directory ({exc}); cannot restart PG after installing extension"
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg) from exc

    if not data_dir or not os.path.isdir(data_dir):
        msg = f"data_directory '{data_dir}' is not a directory; cannot restart PG after installing extension"
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg)

    restart_env = _postgresql_restart_environment()
    sanitizer_runtime = restart_env.get(SANITIZER_RUNTIME_ENV)

    # Postgres.app's universal pg_ctl still needs the native-architecture
    # wrapper.  Homebrew pg_ctl is already thin arm64 and must run directly:
    # /usr/bin/arch is a platform binary, so macOS strips DYLD insertion before
    # the replacement postmaster can inherit the sanitizer runtime.
    if sys.platform == "darwin":
        if sanitizer_runtime is not None and not _is_homebrew_arm64_pg_ctl(pg_ctl):
            raise RuntimeError(
                f"{SANITIZER_RUNTIME_ENV} requires native Homebrew pg_ctl; "
                f"refusing platform-wrapped restart through {pg_ctl}")
        restart_cmd = _darwin_postgresql_restart_argv(pg_ctl, data_dir)
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
    _run_postgresql_restart_cmd(restart_cmd, log_handle, env=restart_env)


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

    data_dir = _query_postgresql_data_directory(psql, active_preset)
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
    """Return the ordinal of an exact native run-report filename.

    The benchmark framework writes ``report_<N>.json``.  Keep accepting the
    older MCP-owned ``report_build_<N>.json`` spelling so an older benchmark
    target or a mixed build tree does not lose its report path.  Exact suffix
    validation deliberately excludes crash/debug sidecars such as
    ``report_debug_<N>.jsonl`` and ``report_<N>.raw.jsonl``.
    """
    if not filename.endswith(".json"):
        return -1
    stem = filename[:-len(".json")]
    for prefix in ("report_build_", "report_"):
        if stem.startswith(prefix):
            ordinal = stem[len(prefix):]
            return int(ordinal) if ordinal.isascii() and ordinal.isdigit() else -1
    return -1


def _report_paths(reporting_dir: Path) -> set[Path]:
    """Snapshot only final JSON reports understood by :func:`_report_index`."""
    return {
        path
        for path in reporting_dir.glob("report_*.json")
        if _report_index(path.name) >= 0
    }


def _newest_report(snapshot_paths: set[Path], mode: str) -> Path | None:
    """Return the highest-numbered mode report created after the snapshot."""
    mode_details = MODES[mode]
    if not mode_details.reporting_dir.exists():
        return None
    current = _report_paths(mode_details.reporting_dir)
    new = current - snapshot_paths
    if not new:
        return None
    # Prefer the framework-native report_<N>.json on the unlikely tie where a
    # directory contains both spellings for the same ordinal.
    return max(
        new,
        key=lambda p: (
            _report_index(p.name),
            not p.name.startswith("report_build_"),
        ),
    )


_CACHE_NATIVE_ARTIFACT_KEYS = frozenset({
    "cache_test_support_postgres_bin_dir",
    "cache_test_support_postgres_stage_root",
    "cache_test_support_postgres_stage_manifest",
    "cache_production_postgres_bin_dir",
    "cache_production_postgres_stage_root",
    "cache_production_postgres_stage_manifest",
    "cache_bioseq_support_postgres_bin_dir",
    "cache_bioseq_support_postgres_stage_root",
    "cache_bioseq_support_postgres_stage_manifest",
    "cache_bioseq_only_postgres_bin_dir",
    "cache_bioseq_only_postgres_stage_root",
    "cache_bioseq_only_postgres_stage_manifest",
    "cache_bioseq_production_postgres_bin_dir",
    "cache_bioseq_production_postgres_stage_root",
    "cache_bioseq_production_postgres_stage_manifest",
    "cache_control",
    "cache_install_sql",
    "cache_dso",
    "cache_test_control",
    "cache_test_install_sql",
    "cache_test_dso",
    "bioseq_control",
    "bioseq_install_sql",
    "bioseq_dso",
    "provider_peer_test_dso",
    "migration_ledger",
    "legacy_inventory",
    "migration_catch_cases",
    "legacy_retirement_gate",
})


def _cache_native_artifacts(active_bin_dir: Path) -> dict[str, str]:
    """Load CMake-generated input paths for native cache runners."""
    manifest = active_bin_dir / "cache_native_artifacts.json"
    try:
        payload = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(
            f"cannot read native cache artifact manifest {manifest}: {exc}"
        ) from exc
    if not isinstance(payload, dict):
        raise RuntimeError(
            f"native cache artifact manifest is not an object: {manifest}")
    unknown = set(payload) - _CACHE_NATIVE_ARTIFACT_KEYS
    missing = _CACHE_NATIVE_ARTIFACT_KEYS - set(payload)
    if unknown or missing:
        raise RuntimeError(
            "native cache artifact manifest keys differ from the MCP "
            f"contract: missing={sorted(missing)} unknown={sorted(unknown)}")
    values: dict[str, str] = {}
    for key, value in payload.items():
        if not isinstance(value, str):
            raise RuntimeError(
                f"native cache artifact {key!r} is not a string")
        if value and not Path(value).is_absolute():
            raise RuntimeError(
                f"native cache artifact {key!r} is not absolute: {value}")
        values[key] = value
    return values


_CACHE_MIGRATION_LEDGER = (
    PROJECT_ROOT / "bioseq_caching" / "test"
    / "legacy_migration_ledger.tsv"
)
_CACHE_LEGACY_INVENTORY = (
    PROJECT_ROOT / "bioseq_caching" / "test"
    / "legacy_asset_inventory.txt"
)
_CACHE_LEGACY_ROOT = PROJECT_ROOT / "bioseq_caching"
_CACHE_RETIREMENT_PROOF = (
    PROJECT_ROOT / "bioseq_caching" / "test"
    / "legacy_retirement_receipt.json"
)
_CACHE_RETIREMENT_REPORTING_DIR = (
    REPORTING_DIR / "bioseq_cache_retirement"
)
_CACHE_UNIT_REPORTING_DIR = REPORTING_DIR / "bioseq_cache_unit"
_CACHE_SOURCE_SCOPES = (
    "main/CMakeLists.txt",
    "main/CMakePresets.json",
    "main/vcpkg.json",
    "main/vcpkg-configuration.json",
    "main/bench",
    "main/bench_framework",
    "main/bioseq",
    "main/bioseq_caching",
    "main/codegen",
    "main/common",
    "main/config",
    "main/overlays",
    "main/vendor",
    "main/cmake",
    ":(top,exclude,glob)main/cmake/**/__pycache__/**",
    "main/build_mcp",
    ":(top,exclude,glob)main/build_mcp/**/__pycache__/**",
)
_CACHE_EVIDENCE_ROLE_BY_EXECUTABLE = {
    "bioseq_cache_integration_tests": "cache-integration",
    "bioseq_cache_bioseq_support_integration_tests":
        "cache-bioseq-support-integration",
    "bioseq_cache_bioseq_production_integration_tests":
        "cache-bioseq-production-integration",
    "bioseq_cache_bench": "cache-bench",
    "bioseq_cache_bioseq_bench": "cache-bioseq-bench",
}
_CACHE_EVIDENCE_EXECUTABLE_BY_ROLE = {
    role: executable
    for executable, role in _CACHE_EVIDENCE_ROLE_BY_EXECUTABLE.items()
}
_CACHE_EVIDENCE_MODE_BY_ROLE = {
    "cache-integration": "cache-integration",
    "cache-bioseq-support-integration": "cache-bioseq-integration",
    "cache-bioseq-production-integration": "cache-bioseq-integration",
    "cache-bench": "cache-bench",
    "cache-bioseq-bench": "cache-bioseq-bench",
}
_CACHE_EVIDENCE_SCENARIO_BY_ROLE = {
    "cache-integration": "cache-integration",
    "cache-bioseq-support-integration": "cache-bioseq-support",
    "cache-bioseq-production-integration":
        "cache-bioseq-production-exact-2gbp",
    "cache-bench": "standalone",
    "cache-bioseq-bench": "bioseq-2gbp",
}
_CACHE_INTEGRATION_HARNESS_BY_ROLE = {
    "cache-integration": "cache-integration",
    "cache-bioseq-support-integration": "cache-bioseq-integration",
    "cache-bioseq-production-integration": "cache-bioseq-integration",
}
_CACHE_EVIDENCE_REPORTING_DIRS = (
    REPORTING_DIR / "bioseq_cache_integration",
    REPORTING_DIR / "bioseq_cache_bioseq_integration",
    REPORTING_DIR / "bioseq_cache_bench",
    REPORTING_DIR / "bioseq_cache_bioseq_bench",
)
_CACHE_RECEIPT_MODES = frozenset({
    "cache-unit",
    "cache-integration",
    "cache-bench",
    "cache-bioseq-integration",
    "cache-bioseq-bench",
})


def _cache_git_bytes(arguments: list[str]) -> bytes:
    try:
        result = subprocess.run(
            ["git", "-C", str(WORKSPACE_ROOT), *arguments],
            capture_output=True,
            check=False,
        )
    except OSError as exc:
        raise RuntimeError(
            f"cannot inspect cache proof Git provenance: {exc}"
        ) from exc
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(
            "cache proof Git provenance command failed: "
            f"{detail or result.returncode}")
    return result.stdout


def _cache_length_prefix(value: bytes) -> bytes:
    return str(len(value)).encode("ascii") + b":" + value


def _cache_source_provenance() -> dict[str, object]:
    revision = _cache_git_bytes(["rev-parse", "HEAD"]).strip().decode(
        "ascii", errors="strict")
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise RuntimeError(
            "cache proof source revision is not a lowercase 40-hex commit")
    status = _cache_git_bytes([
        "status", "--porcelain=v1", "-z", "--untracked-files=all",
        "--", *_CACHE_SOURCE_SCOPES,
    ])
    tree = _cache_git_bytes([
        "ls-tree", "-r", "-z", "--full-tree", revision,
        "--", *_CACHE_SOURCE_SCOPES,
    ])
    material = bytearray(b"bioseq-cache-source-scope/v1\0")
    material += _cache_length_prefix(revision.encode("ascii"))
    for scope in _CACHE_SOURCE_SCOPES:
        material += _cache_length_prefix(scope.encode("utf-8"))
    material += _cache_length_prefix(tree)
    return {
        "source_revision": revision,
        "source_scope_sha256": hashlib.sha256(material).hexdigest(),
        "source_scoped_clean": not status,
    }


def _cache_migration_contract(
        path: Path = _CACHE_MIGRATION_LEDGER,
) -> dict[str, str]:
    try:
        raw = path.read_bytes()
        text = raw.decode("utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise RuntimeError(
            f"cannot seal cache migration ledger {path}: {exc}"
        ) from exc
    lines = text.splitlines()
    expected_header = (
        "legacy_path\tlegacy_line_or_assertion\tstable_id\t"
        "evidence_harness\tevidence_role\tsource_kind\t"
        "scenario_or_permutation\texpected_sqlstate_or_result\t"
        "expected_state_invariant\tcatch2_case\tcatch2_section\t"
        "catch2_tags\tmigration_status"
    )
    if not lines or lines[0] != expected_header or len(lines) == 1:
        raise RuntimeError(
            "cache migration ledger header/body is invalid")
    material = bytearray(
        b"bioseq-cache-migration-evidence-contract/v1\n")
    phases: set[str] = set()
    for line_number, line in enumerate(lines[1:], start=2):
        fields = line.split("\t")
        if len(fields) != 13:
            raise RuntimeError(
                f"cache migration ledger line {line_number} "
                "does not contain 13 fields")
        phases.add(fields[12])
        for field in fields[:12]:
            material += _cache_length_prefix(field.encode("utf-8"))
        material += b"\n"
    phase = next(iter(phases)) if len(phases) == 1 else "mixed"
    if phase not in {"mapped", "passing", "retired_with_proof", "mixed"}:
        raise RuntimeError(
            f"cache migration ledger has invalid phase {phase!r}")
    return {
        "migration_ledger_sha256": hashlib.sha256(raw).hexdigest(),
        "evidence_contract_sha256":
            hashlib.sha256(material).hexdigest(),
        "migration_phase": phase,
    }


def _cache_run_provenance() -> dict[str, object]:
    source = _cache_source_provenance()
    if not source["source_scoped_clean"]:
        raise RuntimeError(
            "cache qualification requires a clean relevant source scope")
    return {**source, **_cache_migration_contract()}


def _cache_qualification_provenance(
        mode: str, build_only: bool) -> dict[str, object] | None:
    """Start a release receipt only for executable lifecycle phases."""
    if build_only or mode not in _CACHE_RECEIPT_MODES:
        return None
    phase = _cache_migration_contract()["migration_phase"]
    if phase not in {"passing", "retired_with_proof"}:
        return None
    return _cache_run_provenance()


def _require_same_cache_run_provenance(
        before: Mapping[str, object],
        after: Mapping[str, object],
) -> None:
    if before != after or not after.get("source_scoped_clean"):
        raise RuntimeError(
            "cache source/ledger provenance changed during qualification")


_CACHE_NATIVE_ARTIFACT_FIELDS = frozenset({
    "name",
    "preinstalled_stage_root",
    "stage_manifest",
    "stage_manifest_sha256",
    "control_file",
    "install_sql",
    "dso",
    "staged_control_file",
    "staged_install_sql",
    "staged_dso",
    "control_sha256",
    "install_sql_sha256",
    "dso_sha256",
    "staged_control_sha256",
})
_CACHE_NATIVE_ARTIFACT_PATH_FIELDS = (
    "preinstalled_stage_root",
    "stage_manifest",
    "control_file",
    "install_sql",
    "dso",
    "staged_control_file",
    "staged_install_sql",
    "staged_dso",
)
_CACHE_NATIVE_ARTIFACT_DIGEST_FIELDS = (
    "stage_manifest_sha256",
    "control_sha256",
    "install_sql_sha256",
    "dso_sha256",
    "staged_control_sha256",
)


def _cache_native_artifact_contracts(
        artifacts: object,
) -> list[dict[str, object]]:
    if (
        not isinstance(artifacts, list)
        or not artifacts
        or any(not isinstance(artifact, dict)
               for artifact in artifacts)
    ):
        raise RuntimeError(
            "native cache report artifact contract is incomplete")
    result: list[dict[str, object]] = []
    for ordinal, artifact in enumerate(artifacts):
        if set(artifact) != _CACHE_NATIVE_ARTIFACT_FIELDS:
            raise RuntimeError(
                "native cache report artifact has an inexact field "
                f"contract at ordinal {ordinal}")
        if artifact.get("name") not in {"bioseq_cache", "bioseq"}:
            raise RuntimeError(
                "native cache report artifact names an unexpected "
                f"extension at ordinal {ordinal}")
        for field in _CACHE_NATIVE_ARTIFACT_PATH_FIELDS:
            value = artifact.get(field)
            if (
                not isinstance(value, str)
                or not Path(value).is_absolute()
                or os.path.normpath(value) != value
            ):
                raise RuntimeError(
                    "native cache report artifact path is not normalized "
                    f"and absolute: artifacts[{ordinal}].{field}")
        for field in _CACHE_NATIVE_ARTIFACT_DIGEST_FIELDS:
            value = artifact.get(field)
            if (
                not isinstance(value, str)
                or not re.fullmatch(r"[0-9a-f]{64}", value)
            ):
                raise RuntimeError(
                    "native cache report artifact digest is invalid: "
                    f"artifacts[{ordinal}].{field}")
        result.append(dict(artifact))
    return result


def _require_absent_nofollow_path(
        path: Path,
        context: str,
) -> None:
    if not path.is_absolute():
        raise RuntimeError(f"{context} is not absolute: {path}")
    current = Path(path.anchor)
    for component in path.parts[1:]:
        current /= component
        try:
            metadata = current.lstat()
        except FileNotFoundError:
            return
        except OSError as exc:
            raise RuntimeError(
                f"cannot inspect {context} component {current}: {exc}"
            ) from exc
        if stat.S_ISLNK(metadata.st_mode):
            raise RuntimeError(
                f"{context} traverses a symlink: {current}")
        if current == path:
            raise RuntimeError(f"{context} still exists: {path}")
        if not stat.S_ISDIR(metadata.st_mode):
            raise RuntimeError(
                f"{context} traverses a non-directory: {current}")
    raise RuntimeError(f"{context} still exists: {path}")


def _cache_cleanup_root_contracts(
        cleanup_observations: list[object],
) -> list[str]:
    canonical_temporary_directory = Path(
        tempfile.gettempdir()).resolve(strict=True)
    roots = [
        observation.get("root")
        if isinstance(observation, dict) else None
        for observation in cleanup_observations
    ]
    if any(
        not isinstance(root, str)
        or not Path(root).is_absolute()
        or os.path.normpath(root) != root
        or not Path(root).name.startswith("bioseq-pg-test-")
        or Path(root).parent != canonical_temporary_directory
        for root in roots
    ):
        raise RuntimeError(
            "native cache integration cleanup roots are invalid or "
            "not marker-owned direct children of the canonical temporary "
            "directory")
    return roots


def _require_cache_cleanup_roots_absent(
        cleanup_roots: list[Path],
        context: str,
) -> None:
    for cleanup_root in cleanup_roots:
        _require_absent_nofollow_path(cleanup_root, context)


def _cache_native_report_contract(
        report_path: Path,
) -> dict[str, object]:
    try:
        value = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(
            f"cannot read native cache report {report_path}: {exc}"
        ) from exc
    if not isinstance(value, dict):
        raise RuntimeError("native cache report is not a JSON object")
    schema = value.get("schema_version")
    if schema == "bioseq-cache-test-v2":
        cases = value.get("cases")
        arguments = value.get("arguments")
        cleanup = value.get("cleanup")
        cleanup_observations = value.get("cleanup_observations")
        cluster = value.get("cluster_configuration")
        if (
            not isinstance(cases, list)
            or not cases
            or any(
                not isinstance(case, dict)
                or case.get("status") != "passed"
                for case in cases
            )
            or not isinstance(cleanup, dict)
            or cleanup.get("succeeded") is not True
            or not isinstance(cleanup_observations, list)
            or not cleanup_observations
            or any(
                not isinstance(observation, dict)
                or observation.get("succeeded") is not True
                for observation in cleanup_observations
            )
            or not isinstance(cluster, dict)
            or cluster.get("retained-supervision-state") != "none"
        ):
            raise RuntimeError(
                "native cache integration report did not pass with "
                "complete cleanup evidence")
        role = value.get("migration_report_role")
        harness = value.get("migration_evidence_harness")
        ledger_sha256 = value.get("migration_ledger_sha256")
        evidence_sha256 = value.get("evidence_contract_sha256")
        phase = value.get("migration_phase")
        coverage = value.get("migration_coverage")
        executable = value.get("executable")
        executable_sha256 = value.get("build_identity")
        artifacts = _cache_native_artifact_contracts(
            value.get("artifacts"))
        cleanup_roots = _cache_cleanup_root_contracts(
            cleanup_observations)
        scenario = (
            _CACHE_EVIDENCE_SCENARIO_BY_ROLE.get(role)
            if isinstance(role, str) else None
        )
        if not isinstance(arguments, list) or not arguments:
            raise RuntimeError(
                "native cache integration report has no executed argv")
        reported_argv: list[str] = []
        for index, argument in enumerate(arguments):
            if (
                not isinstance(argument, dict)
                or set(argument) != {"name", "value", "redacted"}
                or argument.get("name") != f"argv[{index}]"
                or not isinstance(argument.get("value"), str)
                or argument.get("redacted") is not False
            ):
                raise RuntimeError(
                    "native cache integration report argv is not an "
                    "exact unredacted sequence")
            reported_argv.append(argument["value"])
        allowed_options = {
            "--postgres-bin-dir",
            "--postgres-stage-root",
            "--postgres-stage-manifest",
            "--bioseq-only-postgres-bin-dir",
            "--bioseq-only-postgres-stage-root",
            "--bioseq-only-postgres-stage-manifest",
            "--cache-control",
            "--cache-install-sql",
            "--cache-dso",
            "--bioseq-control",
            "--bioseq-install-sql",
            "--bioseq-dso",
            "--provider-peer-test-dso",
            "--migration-ledger",
            "--migration-harness",
            "--migration-report-role",
            "--report",
            "--case-timeout-seconds",
            "--large-case-timeout-seconds",
            "--reporter",
        }
        if len(reported_argv) % 2 != 1:
            raise RuntimeError(
                "native cache integration report argv has a missing "
                "option value")
        reported_options: dict[str, str] = {}
        for index in range(1, len(reported_argv), 2):
            option = reported_argv[index]
            if (
                option not in allowed_options
                or option in reported_options
            ):
                raise RuntimeError(
                    "native cache integration report argv contains a "
                    "Catch2 filter, unknown option, or duplicate option")
            reported_options[option] = reported_argv[index + 1]
        if (
            reported_options.get("--migration-harness") != harness
            or reported_options.get("--migration-report-role") != role
            or (
                "--reporter" in reported_options
                and reported_options["--reporter"]
                    != "console::out=-::colour-mode=ansi"
            )
        ):
            raise RuntimeError(
                "native cache integration report argv is not bound to "
                "its fixed migration harness/role")
    elif schema == "bioseq-cache-benchmark-v2":
        metadata = value.get("metadata")
        cleanup = value.get("cleanup")
        migration = value.get("migration_evidence")
        build_identity = (
            metadata.get("build_identity")
            if isinstance(metadata, dict) else None
        )
        if (
            not isinstance(metadata, dict)
            or metadata.get("status") != "passed"
            or metadata.get("error") != ""
            or not isinstance(cleanup, dict)
            or cleanup.get("attempted") is not True
            or cleanup.get("succeeded") is not True
            or cleanup.get("private_root_removed") is not True
            or not isinstance(migration, dict)
            or not isinstance(build_identity, dict)
        ):
            raise RuntimeError(
                "native cache benchmark did not pass with complete "
                "cleanup and migration evidence")
        role = migration.get("migration_report_role")
        ledger_sha256 = migration.get("ledger_sha256")
        evidence_sha256 = migration.get("evidence_contract_sha256")
        phase = migration.get("migration_phase")
        coverage = migration.get("coverage")
        executable = build_identity.get("executable_path")
        executable_sha256 = build_identity.get("executable_sha256")
        scenario = value.get("scenario")
        harness = migration.get("migration_evidence_harness")
        configured_samples = metadata.get("configured_samples")
        configured_warmup_samples = metadata.get(
            "configured_warmup_samples")
        artifacts = _cache_native_artifact_contracts(
            metadata.get("artifacts"))
        cleanup_roots = []
        if (
            not isinstance(configured_samples, int)
            or isinstance(configured_samples, bool)
            or configured_samples <= 0
            or not isinstance(configured_warmup_samples, int)
            or isinstance(configured_warmup_samples, bool)
            or configured_warmup_samples < 0
        ):
            raise RuntimeError(
                "native cache benchmark invocation/artifact contract "
                "is incomplete")
    else:
        raise RuntimeError(
            f"native cache report schema is not release-safe: {schema!r}")
    if (
        role not in set(_CACHE_EVIDENCE_ROLE_BY_EXECUTABLE.values())
        or coverage != "full-role"
        or not isinstance(ledger_sha256, str)
        or not re.fullmatch(r"[0-9a-f]{64}", ledger_sha256)
        or not isinstance(evidence_sha256, str)
        or not re.fullmatch(r"[0-9a-f]{64}", evidence_sha256)
        or not isinstance(executable, str)
        or not Path(executable).is_absolute()
        or not isinstance(executable_sha256, str)
        or not re.fullmatch(r"[0-9a-f]{64}", executable_sha256)
        or harness != _CACHE_INTEGRATION_HARNESS_BY_ROLE.get(
            role, role)
        or scenario != _CACHE_EVIDENCE_SCENARIO_BY_ROLE.get(role)
        or phase not in {
            "mapped", "passing", "retired_with_proof", "mixed"}
    ):
        raise RuntimeError(
            "native cache report migration contract is incomplete")
    expected_executable = _CACHE_EVIDENCE_EXECUTABLE_BY_ROLE[role]
    if (
        Path(executable).name != expected_executable
        or (
            schema == "bioseq-cache-test-v2"
            and Path(reported_argv[0]) != Path(executable)
        )
    ):
        raise RuntimeError(
            "native cache report executable is incompatible with its "
            "fixed migration role")
    return {
        "report_role": role,
        "migration_ledger_sha256": ledger_sha256,
        "evidence_contract_sha256": evidence_sha256,
        "migration_phase": phase,
        "executable": executable,
        "executable_sha256": executable_sha256,
        "scenario": scenario,
        "arguments": (
            reported_argv
            if schema == "bioseq-cache-test-v2" else []
        ),
        "configured_samples": (
            configured_samples
            if schema == "bioseq-cache-benchmark-v2" else None
        ),
        "configured_warmup_samples": (
            configured_warmup_samples
            if schema == "bioseq-cache-benchmark-v2" else None
        ),
        "artifacts": artifacts,
        "cleanup_roots": cleanup_roots,
    }


def _write_json_atomic(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() or path.is_symlink():
        raise RuntimeError(f"refusing to replace evidence receipt {path}")
    temporary = path.with_name(path.name + ".partial")
    if temporary.exists() or temporary.is_symlink():
        raise RuntimeError(
            f"stale evidence receipt temporary path exists: {temporary}")
    body = json.dumps(
        value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
    try:
        with temporary.open("x", encoding="utf-8") as handle:
            handle.write(body)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
        directory_flags = os.O_RDONLY
        directory_flags |= getattr(os, "O_DIRECTORY", 0)
        directory_fd = os.open(str(path.parent), directory_flags)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except OSError as exc:
        with contextlib.suppress(OSError):
            temporary.unlink()
        raise RuntimeError(
            f"cannot publish evidence receipt {path}: {exc}"
        ) from exc


def _cache_report_receipt_path(report_path: Path) -> Path:
    ordinal = _report_index(report_path.name)
    if ordinal < 0:
        raise RuntimeError(
            f"cache native report has no stable ordinal: {report_path}")
    return report_path.with_name(f"receipt_{ordinal}.json")


def _seal_cache_native_report(
        mode: str,
        active_bin_dir: Path,
        invocation: list[str],
        report_path: Path,
        provenance: Mapping[str, object],
) -> Path:
    if not invocation:
        raise RuntimeError("cache report invocation is empty")
    executable_name = Path(invocation[0]).name
    expected_role = _CACHE_EVIDENCE_ROLE_BY_EXECUTABLE.get(executable_name)
    if expected_role is None:
        raise RuntimeError(
            "cache report executable has no fixed role: "
            f"{executable_name}")
    native = _cache_native_report_contract(report_path)
    if (
        native["report_role"] != expected_role
        or mode != _CACHE_EVIDENCE_MODE_BY_ROLE[expected_role]
        or native["scenario"]
            != _CACHE_EVIDENCE_SCENARIO_BY_ROLE[expected_role]
    ):
        raise RuntimeError(
            "native cache report mode/role/scenario differs from its "
            "fixed executable")
    expected_argv = list(invocation)
    expected_argv[0] = str(Path(expected_argv[0]).resolve(strict=True))
    if len(expected_argv) % 2 != 1:
        raise RuntimeError(
            "fixed cache report command has an invalid option shape")
    expected_options = {
        expected_argv[index]: expected_argv[index + 1]
        for index in range(1, len(expected_argv), 2)
    }
    if len(expected_options) != (len(expected_argv) - 1) // 2:
        raise RuntimeError(
            "fixed cache report command duplicates an option")
    if expected_role in _CACHE_INTEGRATION_HARNESS_BY_ROLE:
        reported_argv = list(native["arguments"])
        reported_argv[0] = str(
            Path(reported_argv[0]).resolve(strict=True))
        if reported_argv != expected_argv:
            raise RuntimeError(
                "native cache integration report argv differs from the "
                "exact MCP command")
    else:
        if (
            native["scenario"] != expected_options.get("--scenario")
            or native["configured_samples"]
                != int(expected_options["--samples"])
            or native["configured_warmup_samples"]
                != int(expected_options["--warmup-samples"])
        ):
            raise RuntimeError(
                "native cache benchmark invocation differs from the "
                "fixed MCP command")
    artifact_contracts = [
        (
            "--postgres-stage-root",
            "--postgres-stage-manifest",
            "bioseq_cache",
            ("--cache-control", "--cache-install-sql", "--cache-dso"),
        ),
    ]
    if "--bioseq-control" in expected_options:
        artifact_contracts.append((
            "--postgres-stage-root",
            "--postgres-stage-manifest",
            "bioseq",
            ("--bioseq-control", "--bioseq-install-sql", "--bioseq-dso"),
        ))
    if "--bioseq-only-postgres-stage-root" in expected_options:
        artifact_contracts.append((
            "--bioseq-only-postgres-stage-root",
            "--bioseq-only-postgres-stage-manifest",
            "bioseq",
            ("--bioseq-control", "--bioseq-install-sql", "--bioseq-dso"),
        ))
    if "--provider-peer-test-dso" in expected_options:
        provider_contract = (
            "--postgres-stage-root",
            "--postgres-stage-manifest",
            "bioseq_cache",
            (
                "--cache-control",
                "--cache-install-sql",
                "--provider-peer-test-dso",
            ),
        )
        artifact_contracts.extend(
            (provider_contract, provider_contract))
    expected_artifacts: dict[tuple[object, ...], int] = {}
    for (
        stage_root_option,
        stage_manifest_option,
        name,
        artifact_options,
    ) in artifact_contracts:
        paths = tuple(
            Path(expected_options[option]).resolve(strict=True)
            for option in artifact_options
        )
        key = (
            name,
            Path(expected_options[stage_root_option]).resolve(
                strict=True),
            Path(expected_options[stage_manifest_option]).resolve(
                strict=True),
            *paths,
        )
        expected_artifacts[key] = expected_artifacts.get(key, 0) + 1
    reported_artifacts: dict[tuple[object, ...], int] = {}
    reported_rows: list[
        tuple[tuple[object, ...], Mapping[str, object]]
    ] = []
    for artifact in native["artifacts"]:
        name = artifact.get("name")
        if not isinstance(name, str):
            raise RuntimeError(
                "native cache report artifact name is invalid")
        key = (
            name,
            Path(artifact.get(
                "preinstalled_stage_root", "")).resolve(strict=True),
            Path(artifact.get(
                "stage_manifest", "")).resolve(strict=True),
            Path(artifact.get(
                "control_file", "")).resolve(strict=True),
            Path(artifact.get(
                "install_sql", "")).resolve(strict=True),
            Path(artifact.get("dso", "")).resolve(strict=True),
        )
        reported_artifacts[key] = reported_artifacts.get(key, 0) + 1
        reported_rows.append((key, artifact))
    if reported_artifacts != expected_artifacts:
        raise RuntimeError(
            "native cache report artifact roster differs from the "
            "fixed MCP command")
    lexical_cleanup_roots = [
        Path(root) for root in native["cleanup_roots"]
    ]
    _require_cache_cleanup_roots_absent(
        lexical_cleanup_roots,
        "native cache integration cleanup root",
    )
    cleanup_roots = {
        root.resolve(strict=False)
        for root in lexical_cleanup_roots
    }
    provider_staged_dsos: set[Path] = set()
    canonical_cache_dso = Path(
        expected_options["--cache-dso"]).resolve(strict=True)
    canonical_rows = [
        (key, artifact)
        for key, artifact in reported_rows
        if key[0] == "bioseq_cache"
        and key[5] == canonical_cache_dso
    ]
    if len(canonical_rows) != 1:
        raise RuntimeError(
            "native cache report does not contain one canonical cache "
            "artifact")
    canonical_key, canonical_artifact = canonical_rows[0]
    canonical_stage_root = canonical_key[1]
    canonical_staged_control = Path(
        canonical_artifact["staged_control_file"]).resolve(strict=True)
    canonical_staged_install_sql = Path(
        canonical_artifact["staged_install_sql"]).resolve(strict=True)
    canonical_cache_staged_dso = Path(
        canonical_artifact["staged_dso"]).resolve(strict=True)
    canonical_shared_values = tuple(
        canonical_artifact[field]
        for field in (
            "stage_manifest_sha256",
            "control_sha256",
            "install_sql_sha256",
            "staged_control_sha256",
        )
    )
    for key, artifact in reported_rows:
        (
            name,
            stage_root,
            stage_manifest,
            control_file,
            install_sql,
            dso,
        ) = key
        expected_paths = (control_file, install_sql, dso)
        expected_hashes = tuple(
            _sha256_path(path) for path in expected_paths)
        staged_control = Path(artifact.get(
            "staged_control_file", "")).resolve(strict=True)
        staged_install_sql = Path(artifact.get(
            "staged_install_sql", "")).resolve(strict=True)
        staged_dso_reported = Path(
            artifact.get("staged_dso", ""))
        if (
            not staged_control.is_relative_to(stage_root)
            or not staged_install_sql.is_relative_to(stage_root)
        ):
            raise RuntimeError(
                "native cache report staged control/SQL escapes its "
                f"preinstalled stage: {name}")
        is_provider_override = (
            name == "bioseq_cache"
            and "--provider-peer-test-dso" in expected_options
            and dso == Path(expected_options[
                "--provider-peer-test-dso"]).resolve(strict=True)
        )
        if (
            artifact.get("stage_manifest_sha256")
                != _sha256_path(stage_manifest)
            or (
                artifact.get("control_sha256"),
                artifact.get("install_sql_sha256"),
                artifact.get("dso_sha256"),
            ) != expected_hashes
            or artifact.get("staged_control_sha256")
                != _sha256_path(staged_control)
            or _sha256_path(staged_install_sql) != expected_hashes[1]
        ):
            raise RuntimeError(
                "native cache report artifact identity differs from "
                f"its fixed MCP command: {name}")
        if is_provider_override:
            lexical_runtime_root = (
                staged_dso_reported.parent.parent)
            if not lexical_runtime_root.name.startswith(
                    "bioseq-pg-test-"):
                raise RuntimeError(
                    "native provider-peer artifact runtime root is not "
                    "marker-owned")
            _require_absent_nofollow_path(
                staged_dso_reported,
                "native provider-peer staged DSO",
            )
            staged_dso = staged_dso_reported.resolve(strict=False)
            runtime_root = (
                lexical_runtime_root.resolve(strict=False))
            if (
                stage_root != canonical_stage_root
                or staged_control != canonical_staged_control
                or staged_install_sql
                    != canonical_staged_install_sql
                or tuple(
                    artifact[field]
                    for field in (
                        "stage_manifest_sha256",
                        "control_sha256",
                        "install_sql_sha256",
                        "staged_control_sha256",
                    )
                ) != canonical_shared_values
                or staged_dso.exists()
                or staged_dso.is_symlink()
                or staged_dso.parent.name != "runtime-library"
                or runtime_root not in cleanup_roots
                or not runtime_root.name.startswith("bioseq-pg-test-")
                or staged_dso in provider_staged_dsos
            ):
                raise RuntimeError(
                    "native provider-peer artifact does not identify a "
                    "distinct cleaned marker-owned runtime DSO")
            provider_staged_dsos.add(staged_dso)
        else:
            staged_dso = staged_dso_reported.resolve(strict=True)
            if (
                not staged_dso.is_relative_to(stage_root)
                or _sha256_path(staged_dso) != expected_hashes[2]
            ):
                raise RuntimeError(
                    "native cache report staged DSO differs from its "
                    f"fixed MCP command: {name}")
    if (
        "--provider-peer-test-dso" in expected_options
        and (
            len(provider_staged_dsos) != 2
            or any(
                path.name != canonical_cache_staged_dso.name
                for path in provider_staged_dsos
            )
        )
    ):
        raise RuntimeError(
            "native provider-peer artifact multiplicity or staged DSO "
            "basename differs from the canonical cache artifact")
    executable = (active_bin_dir / executable_name).resolve(strict=True)
    native_executable = Path(native["executable"]).resolve(strict=True)
    if (
        native_executable != executable
        or native["executable_sha256"] != _sha256_path(executable)
    ):
        raise RuntimeError(
            "native cache report executable identity differs from its "
            "fixed MCP command")
    for field in (
        "migration_ledger_sha256",
        "evidence_contract_sha256",
        "migration_phase",
    ):
        if native[field] != provenance[field]:
            raise RuntimeError(
                f"native cache report {field} differs from MCP provenance")
    receipt = {
        "schema_version": "bioseq-cache-mcp-evidence-receipt-v1",
        "mode": mode,
        "report_role": expected_role,
        "native_report": str(report_path.resolve(strict=True)),
        "native_report_sha256": _sha256_path(report_path),
        "executable": str(executable),
        "executable_sha256": native["executable_sha256"],
        "scenario": native["scenario"],
        "source_revision": provenance["source_revision"],
        "source_scope_sha256": provenance["source_scope_sha256"],
        "source_scoped_clean": provenance["source_scoped_clean"],
        "migration_ledger_sha256":
            provenance["migration_ledger_sha256"],
        "evidence_contract_sha256":
            provenance["evidence_contract_sha256"],
        "migration_phase": provenance["migration_phase"],
    }
    output = _cache_report_receipt_path(report_path)
    _require_cache_cleanup_roots_absent(
        lexical_cleanup_roots,
        "native cache integration cleanup root before receipt publication",
    )
    _write_json_atomic(output, receipt)
    return output


def _next_cache_unit_receipt_path() -> Path:
    _CACHE_UNIT_REPORTING_DIR.mkdir(parents=True, exist_ok=True)
    ordinals = []
    for path in _CACHE_UNIT_REPORTING_DIR.glob("receipt_*.json"):
        match = re.fullmatch(r"receipt_([0-9]+)\.json", path.name)
        if match:
            ordinals.append(int(match.group(1)))
    return _CACHE_UNIT_REPORTING_DIR / (
        f"receipt_{max(ordinals, default=-1) + 1}.json")


def _seal_cache_unit_receipt(
        active_bin_dir: Path,
        provenance: Mapping[str, object],
        command_records: list[dict[str, object]],
) -> Path:
    configured = MODES["cache-unit"].commands
    if len(command_records) != len(configured):
        raise RuntimeError(
            "cache-unit receipt requires one frozen identity for each "
            "fixed native command")
    commands: list[dict[str, object]] = []
    for command, record in zip(
            configured, command_records, strict=True):
        executable = record.get("executable")
        executable_sha256 = record.get("executable_sha256")
        arguments = record.get("arguments")
        identity = record.get("_identity")
        if (
            not isinstance(executable, str)
            or not Path(executable).is_absolute()
            or not isinstance(executable_sha256, str)
            or not re.fullmatch(r"[0-9a-f]{64}", executable_sha256)
            or not isinstance(arguments, list)
            or any(not isinstance(argument, str)
                   for argument in arguments)
            or not isinstance(identity, tuple)
        ):
            raise RuntimeError(
                "cache-unit frozen native command identity is invalid")
        policy = _run_command_argv(
            "cache-unit", active_bin_dir, command)
        policy[0] = str(Path(policy[0]).resolve(strict=True))
        invocation = [executable, *arguments]
        if invocation != policy:
            raise RuntimeError(
                "cache-unit frozen invocation differs from its fixed "
                "mode policy")
        if _stable_native_command_identity(invocation) != record:
            raise RuntimeError(
                "cache-unit executable identity changed before receipt "
                f"publication: {executable}")
        commands.append({
            "executable": executable,
            "executable_sha256": executable_sha256,
            "arguments": arguments,
        })
    receipt = {
        "schema_version": "bioseq-cache-mcp-unit-receipt-v1",
        "mode": "cache-unit",
        "status": "passed",
        "source_revision": provenance["source_revision"],
        "source_scope_sha256": provenance["source_scope_sha256"],
        "source_scoped_clean": provenance["source_scoped_clean"],
        "migration_ledger_sha256":
            provenance["migration_ledger_sha256"],
        "evidence_contract_sha256":
            provenance["evidence_contract_sha256"],
        "migration_phase": provenance["migration_phase"],
        "commands": commands,
    }
    output = _next_cache_unit_receipt_path()
    _write_json_atomic(output, receipt)
    return output


def _load_json_object(path: Path, context: str) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"cannot read {context} {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise RuntimeError(f"{context} is not a JSON object: {path}")
    return value


def _cache_receipt_candidates() -> list[Path]:
    result: list[Path] = []
    for directory in _CACHE_EVIDENCE_REPORTING_DIRS:
        result.extend(directory.glob("receipt_*.json"))
    return result


def _cache_receipt_sort_key(path: Path) -> tuple[int, str]:
    try:
        return path.stat().st_mtime_ns, str(path)
    except OSError:
        return -1, str(path)


def _find_current_cache_evidence_receipts(
        provenance: Mapping[str, object]) -> list[Path]:
    by_role: dict[str, Path] = {}
    for path in sorted(
            _cache_receipt_candidates(), key=_cache_receipt_sort_key):
        value = _load_json_object(path, "cache evidence receipt")
        if (
            value.get("schema_version")
                != "bioseq-cache-mcp-evidence-receipt-v1"
            or value.get("source_revision")
                != provenance["source_revision"]
            or value.get("source_scope_sha256")
                != provenance["source_scope_sha256"]
            or value.get("source_scoped_clean") is not True
            or value.get("migration_ledger_sha256")
                != provenance["migration_ledger_sha256"]
            or value.get("evidence_contract_sha256")
                != provenance["evidence_contract_sha256"]
            or value.get("migration_phase")
                != provenance["migration_phase"]
        ):
            continue
        role = value.get("report_role")
        executable = value.get("executable")
        executable_sha256 = value.get("executable_sha256")
        if (
            role in set(_CACHE_EVIDENCE_ROLE_BY_EXECUTABLE.values())
            and value.get("mode") == _CACHE_EVIDENCE_MODE_BY_ROLE[role]
            and isinstance(executable, str)
            and Path(executable).is_absolute()
            and Path(executable).name
                == _CACHE_EVIDENCE_EXECUTABLE_BY_ROLE[role]
            and isinstance(executable_sha256, str)
            and re.fullmatch(r"[0-9a-f]{64}", executable_sha256)
            and value.get("scenario")
                == _CACHE_EVIDENCE_SCENARIO_BY_ROLE[role]
        ):
            by_role[str(role)] = path
    required = set(_CACHE_EVIDENCE_ROLE_BY_EXECUTABLE.values())
    if set(by_role) != required:
        raise RuntimeError(
            "current clean source has no exact five-role cache evidence "
            f"roster; missing={sorted(required - set(by_role))}")
    return [by_role[role] for role in (
        "cache-integration",
        "cache-bioseq-support-integration",
        "cache-bioseq-production-integration",
        "cache-bench",
        "cache-bioseq-bench",
    )]


def _find_current_cache_unit_receipt(
        provenance: Mapping[str, object]) -> Path:
    matches: list[Path] = []
    for path in _CACHE_UNIT_REPORTING_DIR.glob("receipt_*.json"):
        value = _load_json_object(path, "cache-unit receipt")
        if (
            value.get("schema_version")
                == "bioseq-cache-mcp-unit-receipt-v1"
            and value.get("mode") == "cache-unit"
            and value.get("status") == "passed"
            and value.get("source_revision")
                == provenance["source_revision"]
            and value.get("source_scope_sha256")
                == provenance["source_scope_sha256"]
            and value.get("source_scoped_clean") is True
            and value.get("migration_ledger_sha256")
                == provenance["migration_ledger_sha256"]
            and value.get("evidence_contract_sha256")
                == provenance["evidence_contract_sha256"]
            and value.get("migration_phase")
                == provenance["migration_phase"]
        ):
            matches.append(path)
    if not matches:
        raise RuntimeError(
            "current clean source has no passing cache-unit MCP receipt")
    return max(matches, key=_cache_receipt_sort_key)


def _load_cache_retirement_proof() -> dict[str, object]:
    value = _load_json_object(
        _CACHE_RETIREMENT_PROOF, "cache retirement proof")
    if value.get("schema_version") != "bioseq-cache-retirement-proof-v1":
        raise RuntimeError("cache retirement proof schema is invalid")
    return value


def _find_proof_cache_receipts(
        proof: Mapping[str, object]) -> tuple[Path, list[Path]]:
    unit_sha256 = proof.get("unit_receipt_sha256")
    report_records = proof.get("reports")
    if (
        not isinstance(unit_sha256, str)
        or not re.fullmatch(r"[0-9a-f]{64}", unit_sha256)
        or not isinstance(report_records, list)
        or len(report_records) != 5
    ):
        raise RuntimeError(
            "cache retirement proof receipt seals are invalid")
    expected_reports: dict[str, str] = {}
    for record in report_records:
        if not isinstance(record, dict):
            raise RuntimeError(
                "cache retirement proof report seal is invalid")
        role = record.get("report_role")
        receipt_sha256 = record.get("receipt_sha256")
        if (
            role in expected_reports
            or role not in set(
                _CACHE_EVIDENCE_ROLE_BY_EXECUTABLE.values())
            or not isinstance(receipt_sha256, str)
            or not re.fullmatch(r"[0-9a-f]{64}", receipt_sha256)
        ):
            raise RuntimeError(
                "cache retirement proof report roster is invalid")
        expected_reports[str(role)] = receipt_sha256
    found_reports: dict[str, Path] = {}
    for path in _cache_receipt_candidates():
        digest = _sha256_path(path)
        for role, expected in expected_reports.items():
            if digest == expected:
                if role in found_reports:
                    raise RuntimeError(
                        f"duplicate retained proof receipt for {role}")
                found_reports[role] = path
    if set(found_reports) != set(expected_reports):
        raise RuntimeError(
            "retained proof-ready native report receipts are incomplete")
    unit_matches = [
        path
        for path in _CACHE_UNIT_REPORTING_DIR.glob("receipt_*.json")
        if _sha256_path(path) == unit_sha256
    ]
    if len(unit_matches) != 1:
        raise RuntimeError(
            "retained proof-ready cache-unit receipt is unavailable "
            "or ambiguous")
    return unit_matches[0], [
        found_reports[role] for role in (
            "cache-integration",
            "cache-bioseq-support-integration",
            "cache-bioseq-production-integration",
            "cache-bench",
            "cache-bioseq-bench",
        )
    ]


def _cache_legacy_inventory_paths() -> list[str]:
    try:
        lines = _CACHE_LEGACY_INVENTORY.read_text(
            encoding="utf-8").splitlines()
    except OSError as exc:
        raise RuntimeError(
            f"cannot read cache legacy inventory: {exc}") from exc
    if not lines or lines[0] != "legacy_path\tsha256":
        raise RuntimeError("cache legacy inventory header is invalid")
    paths: list[str] = []
    for line in lines[1:]:
        fields = line.split("\t")
        if (
            len(fields) != 2
            or not fields[0]
            or not re.fullmatch(r"[0-9a-f]{64}", fields[1])
        ):
            raise RuntimeError("cache legacy inventory row is invalid")
        paths.append(fields[0])
    if len(paths) != 85 or paths != sorted(set(paths)):
        raise RuntimeError(
            "cache legacy inventory must contain 85 sorted unique paths")
    return paths


def _cache_retirement_transition_sha256(
        proof_revision: str, current_revision: str) -> str:
    if (
        not re.fullmatch(r"[0-9a-f]{40}", proof_revision)
        or not re.fullmatch(r"[0-9a-f]{40}", current_revision)
        or proof_revision == current_revision
    ):
        raise RuntimeError(
            "cache retirement transition revisions are invalid")
    ancestor = subprocess.run(
        ["git", "-C", str(WORKSPACE_ROOT), "merge-base",
         "--is-ancestor", proof_revision, current_revision],
        capture_output=True,
        check=False,
    )
    if ancestor.returncode != 0:
        raise RuntimeError(
            "cache proof revision is not an ancestor of retired source")
    raw = _cache_git_bytes([
        "diff", "--name-status", "-z", "--no-renames",
        proof_revision, current_revision, "--", *_CACHE_SOURCE_SCOPES,
    ])
    tokens = raw.split(b"\0")
    if tokens and tokens[-1] == b"":
        tokens.pop()
    if len(tokens) % 2 != 0:
        raise RuntimeError(
            "cache retirement Git transition is malformed")
    actual: dict[str, str] = {}
    for index in range(0, len(tokens), 2):
        status = tokens[index].decode("ascii", errors="strict")
        path = tokens[index + 1].decode("utf-8", errors="strict")
        if status not in {"A", "M", "D"} or path in actual:
            raise RuntimeError(
                "cache retirement Git transition contains unsupported "
                "or duplicate changes")
        actual[path] = status
    expected = {
        f"main/bioseq_caching/{path}": "D"
        for path in _cache_legacy_inventory_paths()
    }
    expected["main/bioseq_caching/test/legacy_migration_ledger.tsv"] = "M"
    receipt_path = (
        "main/bioseq_caching/test/legacy_retirement_receipt.json")
    receipt_status = actual.get(receipt_path)
    if receipt_status != "A":
        raise RuntimeError(
            "cache retirement transition must add the append-only proof "
            "receipt")
    expected[receipt_path] = "A"
    if actual != expected:
        missing = sorted(set(expected) - set(actual))
        unexpected = sorted(set(actual) - set(expected))
        wrong = sorted(
            path for path in set(actual) & set(expected)
            if actual[path] != expected[path])
        raise RuntimeError(
            "cache retirement transition is not exact: "
            f"missing={missing} unexpected={unexpected} wrong={wrong}")
    material = bytearray(b"bioseq-cache-retirement-transition/v1\n")
    material += _cache_length_prefix(proof_revision.encode("ascii"))
    material += _cache_length_prefix(current_revision.encode("ascii"))
    for path, status in sorted(actual.items()):
        material += _cache_length_prefix(status.encode("ascii"))
        material += _cache_length_prefix(path.encode("utf-8"))
    return hashlib.sha256(material).hexdigest()


def _next_cache_retirement_bundle_path(operation: str) -> Path:
    _CACHE_RETIREMENT_REPORTING_DIR.mkdir(parents=True, exist_ok=True)
    return _CACHE_RETIREMENT_REPORTING_DIR / (
        f"gate_{operation}_{time.time_ns()}.json")


def _cache_transition_receipt_path(
        provenance: Mapping[str, object]) -> Path:
    revision = provenance["source_revision"]
    if not isinstance(revision, str) or not re.fullmatch(
            r"[0-9a-f]{40}", revision):
        raise RuntimeError(
            "cache transition receipt source revision is invalid")
    return _CACHE_RETIREMENT_REPORTING_DIR / (
        f"transition_{revision}.json")


def _cache_combined_case_manifest(
        active_bin_dir: Path) -> Path:
    path = Path(
        _cache_native_artifacts(
            active_bin_dir)["migration_catch_cases"])
    if not path.is_file() or path.is_symlink():
        raise RuntimeError(
            f"combined cache Catch manifest is unavailable: {path}")
    return path.resolve(strict=True)


def _write_cache_retirement_bundle(
        operation: str,
        provenance: Mapping[str, object],
        unit_receipt: Path,
        evidence_receipts: list[Path],
        active_bin_dir: Path,
        transition_sha256: str = "",
        proof_unit_receipt: Path | None = None,
        proof_evidence_receipts: list[Path] | None = None,
) -> Path:
    if operation not in {
            "proof-ready", "retired-transition", "retired-final"}:
        raise RuntimeError(
            f"invalid cache retirement operation {operation!r}")
    bundle = {
        "schema_version": "bioseq-cache-retirement-gate-input-v1",
        "phase": operation,
        "ledger": str(_CACHE_MIGRATION_LEDGER.resolve(strict=True)),
        "catch_cases":
            str(_cache_combined_case_manifest(active_bin_dir)),
        "legacy_root": str(_CACHE_LEGACY_ROOT.resolve(strict=True)),
        "legacy_inventory":
            str(_CACHE_LEGACY_INVENTORY.resolve(strict=True)),
        "proof_receipt": str(_CACHE_RETIREMENT_PROOF.absolute()),
        "current_source_revision": provenance["source_revision"],
        "current_source_scope_sha256":
            provenance["source_scope_sha256"],
        "current_source_scoped_clean":
            provenance["source_scoped_clean"],
        "retirement_transition_sha256": transition_sha256,
        "unit_receipt": str(unit_receipt.resolve(strict=True)),
        "evidence_receipts": [
            str(path.resolve(strict=True)) for path in evidence_receipts],
        "proof_unit_receipt": (
            str(proof_unit_receipt.resolve(strict=True))
            if proof_unit_receipt is not None else ""
        ),
        "proof_evidence_receipts": [
            str(path.resolve(strict=True))
            for path in (proof_evidence_receipts or [])
        ],
        "transition_receipt": (
            str(
                _cache_transition_receipt_path(provenance).resolve(
                    strict=operation == "retired-final"))
            if operation != "proof-ready"
            else ""
        ),
    }
    output = _next_cache_retirement_bundle_path(operation)
    _write_json_atomic(output, bundle)
    return output


def _seal_cache_mode_receipts(
        mode: str,
        active_bin_dir: Path,
        report_files: list[Path],
        report_invocations: list[list[str]],
        unit_command_records: list[dict[str, object]],
        provenance: Mapping[str, object],
) -> tuple[Path | None, list[Path]]:
    """Seal the exact native evidence produced by one successful MCP mode."""
    if mode == "cache-unit":
        if report_files or report_invocations:
            raise RuntimeError(
                "cache-unit unexpectedly produced native report files")
        return _seal_cache_unit_receipt(
            active_bin_dir, provenance, unit_command_records), []
    if unit_command_records:
        raise RuntimeError(
            f"{mode} unexpectedly captured cache-unit command identities")

    report_commands = [
        command for command in MODES[mode].commands
        if "{report}" in command
    ]
    if (
        len(report_commands) != len(report_files)
        or len(report_commands) != len(report_invocations)
    ):
        raise RuntimeError(
            f"{mode} cannot bind {len(report_files)} reports and "
            f"{len(report_invocations)} invocations to its "
            f"{len(report_commands)} fixed report commands")
    receipts: list[Path] = []
    for command, report_path, invocation in zip(
            report_commands, report_files, report_invocations,
            strict=True):
        policy = _run_command_argv(
            mode,
            active_bin_dir,
            command,
            report_path=report_path,
        )
        policy[0] = str(Path(policy[0]).resolve(strict=True))
        actual = list(invocation)
        if not actual:
            raise RuntimeError(
                f"{mode} captured an empty native report invocation")
        actual[0] = str(Path(actual[0]).resolve(strict=True))
        role = _CACHE_EVIDENCE_ROLE_BY_EXECUTABLE.get(
            Path(actual[0]).name)
        if role == "cache-bioseq-production-integration":
            option = "--large-case-timeout-seconds"
            try:
                policy_index = policy.index(option)
                actual_index = actual.index(option)
                configured_timeout = int(policy[policy_index + 1])
                actual_timeout = int(actual[actual_index + 1])
            except (ValueError, IndexError) as exc:
                raise RuntimeError(
                    "captured exact-2G invocation has no valid bounded "
                    "large-case watchdog") from exc
            if (
                policy_index != actual_index
                or actual_timeout <= 0
                or actual_timeout > configured_timeout
            ):
                raise RuntimeError(
                    "captured exact-2G invocation violates the MCP "
                    "runtime-budget policy")
            policy[policy_index + 1] = str(actual_timeout)
        if actual != policy:
            raise RuntimeError(
                f"{mode} captured invocation differs from its fixed "
                "mode policy")
        receipts.append(_seal_cache_native_report(
            mode, active_bin_dir, actual, report_path, provenance))
    return None, receipts


def _run_cache_retirement_lifecycle_gate(
        mode: str,
        active_bin_dir: Path,
        active_build_dir: Path,
        run_env: dict[str, str],
        provenance: Mapping[str, object],
        log_handle,
        deadline: float,
) -> dict[str, object] | None:
    """Run only the fixed lifecycle closure point for the current phase."""
    phase = provenance["migration_phase"]
    operation: str | None = None
    transition_sha256 = ""
    proof_unit_receipt: Path | None = None
    proof_evidence_receipts: list[Path] = []

    if mode == "cache-unit" and phase == "retired_with_proof":
        operation = "retired-transition"
        proof = _load_cache_retirement_proof()
        unit_receipt, evidence_receipts = _find_proof_cache_receipts(
            proof)
        proof_revision = proof.get("proof_source_revision")
        if not isinstance(proof_revision, str):
            raise RuntimeError(
                "cache retirement proof has no source revision")
        transition_sha256 = _cache_retirement_transition_sha256(
            proof_revision, str(provenance["source_revision"]))
    elif mode == "cache-bioseq-bench" and phase in {
            "passing", "retired_with_proof"}:
        unit_receipt = _find_current_cache_unit_receipt(provenance)
        evidence_receipts = _find_current_cache_evidence_receipts(
            provenance)
        if phase == "passing":
            operation = "proof-ready"
        else:
            operation = "retired-final"
            proof = _load_cache_retirement_proof()
            (
                proof_unit_receipt,
                proof_evidence_receipts,
            ) = _find_proof_cache_receipts(proof)
            proof_revision = proof.get("proof_source_revision")
            if not isinstance(proof_revision, str):
                raise RuntimeError(
                    "cache retirement proof has no source revision")
            transition_sha256 = _cache_retirement_transition_sha256(
                proof_revision, str(provenance["source_revision"]))
    else:
        return None

    bundle = _write_cache_retirement_bundle(
        operation,
        provenance,
        unit_receipt,
        evidence_receipts,
        active_bin_dir,
        transition_sha256,
        proof_unit_receipt,
        proof_evidence_receipts,
    )
    gate = Path(_cache_native_artifacts(
        active_bin_dir)["legacy_retirement_gate"])
    gate_command = [str(gate), "--bundle", str(bundle)]
    _update_entry(
        mode,
        step=f"running cache retirement gate {operation}",
        cache_retirement_gate_operation=operation,
        cache_retirement_gate_bundle=str(bundle),
    )
    with _verified_native_command_execution(gate_command) as (
            gate_identity, verified_executable):
        rc = _stream_subprocess(
            gate_command,
            log_handle,
            deadline,
            cwd=active_build_dir,
            env=run_env,
            verified_executable=verified_executable,
        )
    if _stable_native_command_identity(
            gate_command) != gate_identity:
        raise RuntimeError(
            "cache retirement gate executable identity changed during "
            f"{operation}")
    if rc != 0:
        raise RuntimeError(
            f"cache retirement gate {operation} exited with code {rc}")
    proof_path = _CACHE_RETIREMENT_PROOF.resolve(strict=True)
    return {
        "operation": operation,
        "bundle": str(bundle.resolve(strict=True)),
        "proof_receipt": str(proof_path),
        "proof_receipt_sha256": _sha256_path(proof_path),
        "retirement_transition_sha256": transition_sha256,
        "gate_executable": gate_identity["executable"],
        "gate_executable_sha256":
            gate_identity["executable_sha256"],
    }


def _next_report_path(reporting_dir: Path) -> Path:
    """Reserve the next deterministic report name without creating the file."""
    reporting_dir.mkdir(parents=True, exist_ok=True)
    ordinals = [
        _report_index(path.name)
        for path in _report_paths(reporting_dir)
    ]
    next_ordinal = max(ordinals, default=-1) + 1
    return reporting_dir / f"report_{next_ordinal}.json"


def _require_allocated_command_report(
        mode: str, command: List[str], report_path: Path) -> None:
    """Fail closed unless one native command published its exact receipt."""
    try:
        metadata = report_path.lstat()
    except OSError as exc:
        raise RuntimeError(
            f"{mode} command {command[0]!r} completed without publishing "
            f"its allocated regular JSON report: {report_path}"
        ) from exc
    if report_path.is_symlink() or not stat.S_ISREG(metadata.st_mode):
        raise RuntimeError(
            f"{mode} command {command[0]!r} did not publish its allocated "
            f"path as a regular non-symlink JSON report: {report_path}")


def _cache_apply_runtime_budget(
        mode: str, argv: list[str], deadline: float,
        *, now: float | None = None) -> list[str]:
    """Reserve bounded in-run time for watchdog cleanup/report publication."""
    if (
        mode != "cache-bioseq-integration"
        or not argv
        or Path(argv[0]).name
            != "bioseq_cache_bioseq_production_integration_tests"
        or not math.isfinite(deadline)
    ):
        return argv
    current_time = time.time() if now is None else now
    usable_seconds = math.floor(
        deadline - CACHE_EXACT2G_FINALIZATION_RESERVE_SEC - current_time)
    if usable_seconds <= 0:
        raise TimeoutError(
            "cache-bioseq-integration has no remaining execution budget "
            "before its cleanup/report reserve")
    try:
        option = argv.index("--large-case-timeout-seconds")
        configured = int(argv[option + 1])
    except (ValueError, IndexError) as exc:
        raise RuntimeError(
            "exact-2G command has no valid large-case watchdog") from exc
    bounded = min(configured, usable_seconds)
    if bounded <= 0:
        raise TimeoutError(
            "exact-2G watchdog budget is exhausted")
    result = list(argv)
    result[option + 1] = str(bounded)
    return result


def _validate_failed_cache_terminal_report(report_path: Path) -> None:
    """A timed/failed runner may fail, but never retain owned resources."""
    value = _load_json_object(
        report_path, "failed cache integration terminal report")
    if value.get("schema_version") != "bioseq-cache-test-v2":
        raise RuntimeError(
            "failed cache integration has no v2 terminal report")
    cleanup = value.get("cleanup")
    observations = value.get("cleanup_observations")
    cluster = value.get("cluster_configuration")
    if (
        not isinstance(cleanup, dict)
        or cleanup.get("succeeded") is not True
        or not isinstance(observations, list)
        or not observations
        or any(
            not isinstance(observation, dict)
            or observation.get("succeeded") is not True
            for observation in observations
        )
        or not isinstance(cluster, dict)
        or cluster.get("retained-supervision-state") != "none"
    ):
        raise RuntimeError(
            "failed cache integration terminal report does not prove "
            "successful cleanup and zero retained supervision state")
    cleanup_roots = _cache_cleanup_root_contracts(observations)
    lexical_cleanup_roots = [
        Path(root) for root in cleanup_roots
    ]
    _require_cache_cleanup_roots_absent(
        lexical_cleanup_roots,
        "failed cache integration cleanup root",
    )
    _require_cache_cleanup_roots_absent(
        lexical_cleanup_roots,
        "failed cache integration cleanup root before acceptance",
    )


def _native_executable_identity(
        metadata: os.stat_result,
) -> tuple[int, ...]:
    # Access time is intentionally omitted because hashing/execution may
    # update it. The execution snapshot is a separate inode, so source link
    # count and ctime remain authoritative retained-handle metadata.
    return (
        metadata.st_dev,
        metadata.st_ino,
        metadata.st_mode,
        metadata.st_nlink,
        metadata.st_uid,
        metadata.st_size,
        metadata.st_mtime_ns,
        metadata.st_ctime_ns,
    )


def _sha256_descriptor(descriptor: int) -> str:
    digest = hashlib.sha256()
    offset = 0
    while True:
        chunk = os.pread(descriptor, 1024 * 1024, offset)
        if not chunk:
            break
        digest.update(chunk)
        offset += len(chunk)
    return digest.hexdigest()


def _open_native_command_identity(
        argv: list[str],
) -> tuple[int, dict[str, object]]:
    if not argv:
        raise RuntimeError("native command invocation is empty")
    requested = Path(argv[0])
    try:
        requested_status = requested.lstat()
        resolved = requested.resolve(strict=True)
    except OSError as exc:
        raise RuntimeError(
            f"native command executable is unavailable: {requested}"
        ) from exc
    if requested.is_symlink() or not stat.S_ISREG(requested_status.st_mode):
        raise RuntimeError(
            "native command executable is not a regular non-symlink file: "
            f"{requested}")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(str(resolved), flags)
    except OSError as exc:
        raise RuntimeError(
            f"cannot open native command executable: {resolved}") from exc
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode):
            raise RuntimeError(
                f"native command executable is not regular: {resolved}")
        digest = _sha256_descriptor(descriptor)
        after = os.fstat(descriptor)
    except BaseException:
        os.close(descriptor)
        raise
    identity = _native_executable_identity(before)
    if identity != _native_executable_identity(after):
        os.close(descriptor)
        raise RuntimeError(
            "native command executable changed while it was hashed: "
            f"{resolved}")
    return descriptor, {
        "executable": str(resolved),
        "executable_sha256": digest,
        "arguments": list(argv[1:]),
        "_identity": identity,
    }


def _stable_native_command_identity(
        argv: list[str]) -> dict[str, object]:
    descriptor, result = _open_native_command_identity(argv)
    os.close(descriptor)
    return result


def _copy_descriptor_exact(
        source_descriptor: int,
        destination_descriptor: int,
) -> None:
    offset = 0
    while True:
        chunk = os.pread(
            source_descriptor, 1024 * 1024, offset)
        if not chunk:
            break
        written = 0
        while written < len(chunk):
            count = os.write(
                destination_descriptor, chunk[written:])
            if count <= 0:
                raise RuntimeError(
                    "short write while creating verified native "
                    "execution snapshot")
            written += count
        offset += len(chunk)


def _verified_native_execution_snapshot(
        executable: Path,
        source_descriptor: int,
        expected_sha256: str,
) -> tuple[Path, int, tuple[int, ...]]:
    # Darwin intentionally has no fexecve/execveat and rejects execve on
    # /dev/fd.  A private byte snapshot in the same directory preserves
    # @executable_path/$ORIGIN semantics while binding the launched bytes to
    # the retained source FD.  This closes concurrent build/output mutation of
    # the source path; it is not a security boundary against a malicious
    # same-UID process that actively hunts and replaces random snapshot names.
    flags = os.O_RDWR | os.O_CREAT | os.O_EXCL
    flags |= getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    for _attempt in range(32):
        candidate = executable.with_name(
            ".bioseq-verified-exec-"
            f"{os.getpid()}-{secrets.token_hex(12)}")
        try:
            creator_descriptor = os.open(
                candidate, flags, 0o700)
        except FileExistsError:
            continue
        except OSError as exc:
            raise RuntimeError(
                "cannot create verified native execution snapshot beside "
                f"{executable}: {exc}") from exc
        retained_descriptor: int | None = None
        try:
            _copy_descriptor_exact(
                source_descriptor, creator_descriptor)
            os.fchmod(creator_descriptor, 0o500)
            os.fsync(creator_descriptor)
            directory_flags = os.O_RDONLY
            directory_flags |= getattr(os, "O_DIRECTORY", 0)
            directory_descriptor = os.open(
                str(candidate.parent), directory_flags)
            try:
                os.fsync(directory_descriptor)
            finally:
                os.close(directory_descriptor)
            snapshot_status = os.fstat(creator_descriptor)
            path_status = candidate.lstat()
            snapshot_identity = _native_executable_identity(
                snapshot_status)
            if (
                candidate.is_symlink()
                or not stat.S_ISREG(snapshot_status.st_mode)
                or _native_executable_identity(path_status)
                    != snapshot_identity
                or _sha256_descriptor(source_descriptor)
                    != expected_sha256
                or _sha256_descriptor(creator_descriptor)
                    != expected_sha256
                or _sha256_path(candidate) != expected_sha256
            ):
                raise RuntimeError(
                    "native executable changed while its verified byte "
                    f"snapshot was created: {executable}")
            os.close(creator_descriptor)
            creator_descriptor = None
            retained_flags = os.O_RDONLY
            retained_flags |= getattr(os, "O_CLOEXEC", 0)
            retained_flags |= getattr(os, "O_NOFOLLOW", 0)
            retained_descriptor = os.open(
                candidate, retained_flags)
            retained_status = os.fstat(retained_descriptor)
            if (
                _native_executable_identity(retained_status)
                    != snapshot_identity
                or _sha256_descriptor(retained_descriptor)
                    != expected_sha256
                or (
                    fcntl.fcntl(retained_descriptor, fcntl.F_GETFL)
                    & os.O_ACCMODE
                ) != os.O_RDONLY
            ):
                raise RuntimeError(
                    "verified native execution snapshot could not be "
                    "retained read-only")
            result_descriptor = retained_descriptor
            retained_descriptor = None
            return candidate, result_descriptor, snapshot_identity
        except BaseException:
            if retained_descriptor is not None:
                os.close(retained_descriptor)
            if creator_descriptor is not None:
                os.close(creator_descriptor)
            with contextlib.suppress(OSError):
                candidate.unlink()
            raise
    raise RuntimeError(
        "cannot allocate a unique verified native execution snapshot "
        f"beside {executable}")


def _remove_verified_native_execution_snapshot(
        snapshot: Path,
) -> None:
    try:
        snapshot.unlink()
    except FileNotFoundError:
        # The desired terminal state is absence. Reconcile it below using
        # component-wise no-follow inspection rather than suppressing an
        # arbitrary unlink failure.
        pass
    except OSError as exc:
        raise RuntimeError(
            "cannot remove verified native execution snapshot "
            f"{snapshot}: {exc}") from exc
    _require_absent_nofollow_path(
        snapshot,
        "verified native execution snapshot after cleanup",
    )


@contextlib.contextmanager
def _verified_native_command_execution(
        argv: list[str],
):
    descriptor, record = _open_native_command_identity(argv)
    executable = Path(str(record["executable"]))
    identity = record["_identity"]
    executable_sha256 = record["executable_sha256"]
    if (
        not isinstance(identity, tuple)
        or not isinstance(executable_sha256, str)
    ):
        os.close(descriptor)
        raise RuntimeError(
            "native executable retained identity is invalid")
    snapshot: Path | None = None
    snapshot_descriptor: int | None = None
    snapshot_identity: tuple[int, ...] | None = None
    try:
        (
            snapshot,
            snapshot_descriptor,
            snapshot_identity,
        ) = _verified_native_execution_snapshot(
            executable, descriptor, executable_sha256)
        yield record, snapshot
        if (
            _native_executable_identity(os.fstat(descriptor))
                != identity
            or _sha256_descriptor(descriptor) != executable_sha256
            or _native_executable_identity(
                os.fstat(snapshot_descriptor))
                != snapshot_identity
            or _sha256_descriptor(snapshot_descriptor)
                != executable_sha256
            or _native_executable_identity(snapshot.lstat())
                != snapshot_identity
            or _sha256_path(snapshot) != executable_sha256
        ):
            raise RuntimeError(
                "native executable changed while its verified handle "
                f"was executing: {executable}")
    finally:
        try:
            if snapshot_descriptor is not None:
                os.close(snapshot_descriptor)
            if snapshot is not None:
                _remove_verified_native_execution_snapshot(
                    snapshot)
        finally:
            os.close(descriptor)


def _run_mode_commands(
        mode: str,
        active_bin_dir: Path,
        active_build_dir: Path,
        run_env: dict[str, str],
        log_handle,
        deadline: float,
        pre_reports: set[Path],
        bench_flags=None,
        test_flags=None,
) -> tuple[
    Path | None,
    list[Path],
    list[list[str]],
    list[dict[str, object]],
]:
    """Run configured commands and persist every ordered report receipt."""
    mode_details = MODES[mode]
    expected_report_count = sum(
        1 for command in mode_details.commands if "{report}" in command)
    command_report_files: list[Path] = []
    command_report_invocations: list[list[str]] = []
    unit_command_records: list[dict[str, object]] = []

    for command in mode_details.commands:
        # MODES.commands store binary BASENAMES; resolve against the active
        # build's bin dir (DOC0270). Args after the first slot are passed
        # through verbatim.
        command_report_path = (
            _next_report_path(mode_details.reporting_dir)
            if "{report}" in command else None
        )
        full_command = _run_command_argv(
            mode,
            active_bin_dir,
            command,
            bench_flags=bench_flags,
            test_flags=test_flags,
            report_path=command_report_path,
        )
        full_command = _cache_apply_runtime_budget(
            mode, full_command, deadline)
        unit_command_before: dict[str, object] | None = None
        if mode == "cache-unit":
            with _verified_native_command_execution(
                    full_command) as (
                        unit_command_before,
                        verified_executable,
                    ):
                rc = _stream_subprocess(
                    full_command,
                    log_handle,
                    deadline,
                    cwd=active_build_dir,
                    env=run_env,
                    verified_executable=verified_executable,
                )
        else:
            try:
                rc = _stream_subprocess(
                    full_command,
                    log_handle,
                    deadline,
                    cwd=active_build_dir,
                    env=run_env,
                )
            except TimeoutError:
                if (
                    mode in {
                        "cache-integration",
                        "cache-bioseq-integration",
                    }
                    and command_report_path is not None
                ):
                    _require_allocated_command_report(
                        mode, command, command_report_path)
                    _validate_failed_cache_terminal_report(
                        command_report_path)
                raise
        if unit_command_before is not None:
            unit_command_after = _stable_native_command_identity(
                full_command)
            if unit_command_after != unit_command_before:
                raise RuntimeError(
                    "cache-unit executable identity changed during its "
                    f"native command: {full_command[0]}")
            unit_command_records.append(unit_command_before)

        if command_report_path is not None:
            _require_allocated_command_report(
                mode, command, command_report_path)
            command_report_files.append(command_report_path)
            command_report_invocations.append(list(full_command))
            ordered_receipts = [
                str(path) for path in command_report_files
            ]
            _update_entry(
                mode,
                report_file=ordered_receipts[-1],
                report_files=ordered_receipts,
            )

        if rc != 0:
            if (
                mode == "cache-bioseq-integration"
                and command_report_path is not None
            ):
                _validate_failed_cache_terminal_report(
                    command_report_path)
            # Bench exits non-zero when regression rules are violated
            # (exit code 2) — the report is still a valid receipt, so this is
            # a "completed with violations" outcome rather than a worker
            # failure. The console log already explains the violations.
            if (
                mode_details.is_bench_mode
                and rc == 2
                and mode not in _CACHE_RECEIPT_MODES
            ):
                _log(
                    f"bench reported regression violations (exit {rc}); "
                    "treating as completed-with-violations")
                break
            raise RuntimeError(
                f"run failed with exit code {rc}: {' '.join(command)}")

    if expected_report_count:
        if len(command_report_files) != expected_report_count:
            raise RuntimeError(
                f"{mode} published {len(command_report_files)} ordered "
                "command reports, but its command contract requires exactly "
                f"{expected_report_count}")
        return (
            command_report_files[-1],
            command_report_files,
            command_report_invocations,
            unit_command_records,
        )

    report_path = (
        _newest_report(pre_reports, mode)
        if mode_details.reporting_subdir_name else None
    )
    if mode_details.reporting_subdir_name and report_path is None:
        raise RuntimeError(
            f"{mode} completed without publishing its required native "
            "JSON report")
    return (
        report_path,
        ([report_path] if report_path is not None else []),
        [],
        unit_command_records,
    )


def _sql_guardrail_workflow_manifest(
        active_build_dir: Path) -> dict[str, str]:
    path = active_build_dir / "bioseq_sql_guardrail_workflow_paths.json"
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(
            f"cannot read CMake-owned SQL guardrail manifest {path}: {exc}")
    if (
        not isinstance(value, dict)
        or value.get("schema_version") != 1
        or value.get("artifact_kind")
            != "doc0388_sql_codegen_guardrail_workflow_paths"
    ):
        raise RuntimeError(
            f"invalid CMake-owned SQL guardrail manifest: {path}")
    fields = {
        "repository_root",
        "cmake_source_dir",
        "build_dir",
        "tool",
        "artifact_inventory",
        "generator_output_inventory",
        "benchmark_configuration",
        "calibration_configuration",
        "generator_executable",
        "production_dso",
        "installed_production_dso",
        "benchmark_executable",
        "comparator_dso",
        "stage0_measurement",
        "recovered_measurement",
        "candidate_measurement",
        "runtime_samples",
        "qualification",
    }
    if set(value) != fields | {"schema_version", "artifact_kind"}:
        raise RuntimeError(
            "CMake-owned SQL guardrail manifest field set is not exact")
    result: dict[str, str] = {}
    workspace = WORKSPACE_ROOT.resolve(strict=True)
    for field in fields:
        raw = value.get(field)
        if not isinstance(raw, str) or not raw:
            raise RuntimeError(
                f"SQL guardrail manifest field {field} is empty")
        resolved = Path(raw).resolve(strict=False)
        if field != "installed_production_dso":
            try:
                resolved.relative_to(workspace)
            except ValueError as exc:
                raise RuntimeError(
                    f"SQL guardrail manifest path escapes workspace: {field}"
                ) from exc
        elif not Path(raw).is_absolute():
            raise RuntimeError(
                "SQL guardrail installed production DSO path is not absolute")
        result[field] = str(resolved)
    if Path(result["repository_root"]) != workspace:
        raise RuntimeError(
            "SQL guardrail manifest repository_root is not the workspace")
    if Path(result["cmake_source_dir"]) != PROJECT_ROOT.resolve(strict=True):
        raise RuntimeError(
            "SQL guardrail manifest CMake source is not main/")
    if Path(result["build_dir"]) != active_build_dir.resolve(strict=True):
        raise RuntimeError(
            "SQL guardrail manifest build_dir is not the active MCP tree")
    for field in (
        "tool",
        "artifact_inventory",
        "generator_output_inventory",
        "benchmark_configuration",
        "calibration_configuration",
        "generator_executable",
        "production_dso",
        "benchmark_executable",
        "comparator_dso",
    ):
        if not Path(result[field]).is_file():
            raise RuntimeError(
                f"SQL guardrail manifest input is missing: {field}")
    return result


def _read_sql_guardrail_evidence(
        path: Path,
        *,
        expected_sha256: str | None = None) -> bytes:
    workspace = WORKSPACE_ROOT.resolve(strict=True)
    resolved = path.resolve(strict=True)
    resolved.relative_to(workspace)
    metadata = resolved.lstat()
    if (
        not stat.S_ISREG(metadata.st_mode)
        or metadata.st_nlink != 1
    ):
        raise RuntimeError(
            "SQL guardrail evidence must be a regular single-link file: "
            f"{resolved}")
    body = resolved.read_bytes()
    digest = _sha256_bytes(body)
    if expected_sha256 is not None and digest != expected_sha256:
        raise RuntimeError(
            f"SQL guardrail evidence hash changed for {resolved}: "
            f"{digest} != {expected_sha256}")
    return body


def _write_sql_guardrail_evidence_exclusive(
        path: Path, body: bytes) -> None:
    workspace = WORKSPACE_ROOT.resolve(strict=True)
    parent = path.parent.resolve(strict=True)
    parent.relative_to(workspace)
    resolved = path.resolve(strict=False)
    resolved.relative_to(workspace)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(resolved, flags, 0o600)
    try:
        with os.fdopen(descriptor, "wb", closefd=False) as output:
            output.write(body)
            output.flush()
            os.fsync(output.fileno())
    finally:
        os.close(descriptor)


def _sql_guardrail_artifact_record(path: Path) -> dict[str, object]:
    body = _read_sql_guardrail_evidence(path)
    return {
        "path": str(path.resolve(strict=True).relative_to(
            WORKSPACE_ROOT.resolve(strict=True))),
        "bytes": len(body),
        "sha256": _sha256_bytes(body),
    }


class _SqlGuardrailLiveAuthorityBroker:
    """One-use live callback owned in memory by the running MCP worker.

    This prevents an ordinary/direct CLI invocation from accidentally
    publishing or consuming an authoritative DOC0388 qualification.  It is
    intentionally not represented as authentication against arbitrary
    same-UID process or workspace compromise: that stronger threat model
    requires an external signer or permission-separated service.
    """

    def __init__(
            self,
            *,
            path: Path,
            run_nonce: int,
            authority_file_sha256: str,
            request_snapshot_sha256: str):
        self.path = path
        self.run_nonce = run_nonce
        self.authority_file_sha256 = authority_file_sha256
        self.request_snapshot_sha256 = request_snapshot_sha256
        self.consumed = False
        self.error: BaseException | None = None
        self._stop = threading.Event()
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        if path.exists() or path.is_symlink():
            raise RuntimeError(
                f"live authority broker path already exists: {path}")
        self._socket.bind(str(path))
        os.chmod(path, 0o600)
        self._socket.listen(1)
        self._socket.settimeout(0.1)
        self._thread = threading.Thread(
            target=self._serve,
            name="sql-guardrail-live-authority-broker",
            daemon=True,
        )

    def start(self) -> None:
        self._thread.start()

    def _serve(self) -> None:
        try:
            while not self._stop.is_set():
                try:
                    connection, _ = self._socket.accept()
                except socket.timeout:
                    continue
                with connection:
                    connection.settimeout(2.0)
                    body = bytearray()
                    while b"\n" not in body and len(body) <= 4096:
                        chunk = connection.recv(4096)
                        if not chunk:
                            break
                        body.extend(chunk)
                    try:
                        request = json.loads(
                            bytes(body).split(b"\n", 1)[0])
                    except (UnicodeDecodeError, json.JSONDecodeError):
                        request = None
                    accepted = (
                        not self.consumed
                        and isinstance(request, dict)
                        and set(request) == {
                            "schema_version",
                            "artifact_kind",
                            "run_nonce",
                            "authority_file_sha256",
                            "request_snapshot_sha256",
                        }
                        and request.get("schema_version") == 1
                        and request.get("artifact_kind")
                            == "doc0388_sql_guardrail_authority_claim"
                        and request.get("run_nonce") == self.run_nonce
                        and request.get("authority_file_sha256")
                            == self.authority_file_sha256
                        and request.get("request_snapshot_sha256")
                            == self.request_snapshot_sha256
                    )
                    if accepted:
                        self.consumed = True
                    response = {
                        "schema_version": 1,
                        "artifact_kind":
                            "doc0388_sql_guardrail_authority_response",
                        "accepted": accepted,
                        "run_nonce": self.run_nonce,
                        "authority_file_sha256":
                            self.authority_file_sha256,
                        "request_snapshot_sha256":
                            self.request_snapshot_sha256,
                        "broker_pid": os.getpid(),
                        "broker_session_id": os.getsid(0),
                    }
                    connection.sendall(
                        (json.dumps(
                            response,
                            sort_keys=True,
                            separators=(",", ":"),
                        ) + "\n").encode("utf-8")
                    )
                    if accepted:
                        return
        except BaseException as exc:
            self.error = exc

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=3.0)
        try:
            self._socket.close()
        finally:
            self.path.unlink(missing_ok=True)
        if self._thread.is_alive():
            raise RuntimeError(
                "live SQL guardrail authority broker did not stop")
        if self.error is not None:
            raise RuntimeError(
                "live SQL guardrail authority broker failed: "
                f"{type(self.error).__name__}: {self.error}")


def _create_sql_guardrail_live_authority(
        *,
        output: Path,
        request: dict[str, object],
        run_nonce: int,
        qualification: Path,
        baseline: Path,
        measurement: Path,
        broker_socket: Path) -> dict[str, object]:
    """Seal one non-replayable authority owned by the running MCP worker."""
    state = _load_state()
    entry = state.get("bench-e2e")
    worker_session_id = os.getsid(0)
    request_snapshot_sha256 = _canonical_json_sha256(request)
    if (
        not isinstance(entry, dict)
        or entry.get("status") != "running"
        or entry.get("pid") != os.getpid()
        or entry.get("started_at") is None
        or entry.get("sql_guardrail_request_sha256")
            != request_snapshot_sha256
        or os.getsid(os.getpid()) != worker_session_id
    ):
        raise RuntimeError(
            "live SQL guardrail authority is not owned by the exact "
            "running MCP worker/request/session")
    receipt_path = Path(
        str(request["baseline_publication_receipt"]))
    receipt_body = _read_sql_guardrail_evidence(
        receipt_path,
        expected_sha256=str(
            request["baseline_publication_receipt_sha256"]),
    )
    receipt = json.loads(receipt_body)
    if (
        not isinstance(receipt, dict)
        or receipt.get("publication_id")
            != request["baseline_publication_id"]
    ):
        raise RuntimeError(
            "live SQL guardrail baseline publication join is invalid")

    def artifact(path: Path) -> dict[str, object]:
        body = _read_sql_guardrail_evidence(path)
        try:
            value = json.loads(body)
        except json.JSONDecodeError as exc:
            raise RuntimeError(
                f"live SQL guardrail artifact is not JSON: {path}"
            ) from exc
        if (
            not isinstance(value, dict)
            or re.fullmatch(
                r"[0-9a-f]{64}",
                str(value.get("content_sha256")),
            ) is None
        ):
            raise RuntimeError(
                f"live SQL guardrail artifact has no content seal: {path}")
        return {
            "path": str(path.resolve(strict=True)),
            "file_sha256": _sha256_bytes(body),
            "content_sha256": value["content_sha256"],
        }

    qualification_record = artifact(qualification)
    baseline_record = artifact(baseline)
    measurement_record = artifact(measurement)
    qualification_value = json.loads(
        _read_sql_guardrail_evidence(qualification))
    if (
        qualification_value.get("measurement_sha256")
            != measurement_record["file_sha256"]
        or qualification_value.get("baseline_sha256")
            != baseline_record["file_sha256"]
        or qualification_value.get("qualification_request") != {
            "file_sha256": request["request_file_sha256"],
            "content_sha256": request["request_content_sha256"],
            "normalized_sha256": request_snapshot_sha256,
        }
    ):
        raise RuntimeError(
            "live SQL guardrail qualification/request/artifact joins "
            "are invalid")
    authority: dict[str, object] = {
        "schema_version": 1,
        "artifact_kind":
            "doc0388_sql_guardrail_live_mcp_authority",
        "phase": "candidate_qualification",
        "worker_mode": "bench-e2e",
        "worker_pid": os.getpid(),
        "worker_session_id": worker_session_id,
        "worker_started_at": entry["started_at"],
        "state_file": str(STATE_FILE),
        "broker_socket": str(broker_socket),
        "workflow_request": {
            "file_sha256": request["request_file_sha256"],
            "content_sha256": request["request_content_sha256"],
            "normalized_sha256": request_snapshot_sha256,
        },
        "run_nonce": run_nonce,
        "qualification": qualification_record,
        "baseline": baseline_record,
        "measurement": measurement_record,
        "baseline_publication": {
            "receipt_path": str(receipt_path.resolve(strict=True)),
            "receipt_file_sha256": _sha256_bytes(receipt_body),
            "publication_id": receipt["publication_id"],
        },
        "issued_monotonic_ns": time.monotonic_ns(),
    }
    authority["content_sha256"] = _canonical_json_sha256(authority)
    _write_sql_guardrail_evidence_exclusive(
        output,
        (json.dumps(
            authority, indent=2, sort_keys=True
        ) + "\n").encode("utf-8"),
    )
    return authority


def _sql_guardrail_capture_bundle_record(
        value: object,
        field: str) -> tuple[Path, bytes]:
    if not isinstance(value, dict) or set(value) != {
        "path", "bytes", "sha256"
    }:
        raise RuntimeError(
            f"capture bundle artifact {field} is malformed")
    relative = value.get("path")
    digest = value.get("sha256")
    size = value.get("bytes")
    if (
        not isinstance(relative, str)
        or not relative
        or Path(relative).is_absolute()
        or ".." in Path(relative).parts
        or not isinstance(digest, str)
        or re.fullmatch(r"[0-9a-f]{64}", digest) is None
        or isinstance(size, bool)
        or not isinstance(size, int)
        or size < 0
    ):
        raise RuntimeError(
            f"capture bundle artifact {field} has invalid fields")
    path = WORKSPACE_ROOT / relative
    body = _read_sql_guardrail_evidence(
        path, expected_sha256=digest)
    if len(body) != size:
        raise RuntimeError(
            f"capture bundle artifact {field} byte count changed")
    return path.resolve(strict=True), body


def _validate_sql_guardrail_host_state_capture(
        body: bytes,
        *,
        expected_phase: str,
        expected_run_nonce: int,
        measurement: dict[str, object],
        measurement_file_sha256: str,
        field: str) -> dict[str, object]:
    try:
        value = json.loads(body)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"capture bundle {field} is not valid JSON") from exc
    expected_fields = {
        "schema_version",
        "artifact_kind",
        "phase",
        "run_nonce",
        "measurement_sha256",
        "measurement_content_sha256",
        "source",
        "environment",
        "snapshot",
        "content_sha256",
    }
    if (
        not isinstance(value, dict)
        or set(value) != expected_fields
        or value.get("schema_version") != 1
        or value.get("artifact_kind") != "runtime_host_state"
        or value.get("phase") != expected_phase
        or value.get("run_nonce") != expected_run_nonce
        or value.get("measurement_sha256") != measurement_file_sha256
        or value.get("measurement_content_sha256")
            != measurement.get("content_sha256")
        or value.get("source") != measurement.get("source")
        or value.get("environment") != measurement.get("environment")
    ):
        raise RuntimeError(
            f"capture bundle {field} identity/binding is invalid")
    try:
        _validate_json_content_seal(value, f"capture bundle {field}")
    except ValueError as exc:
        raise RuntimeError(str(exc)) from exc
    snapshot = value.get("snapshot")
    snapshot_fields = {
        "captured_monotonic_ns",
        "load_1",
        "load_5",
        "load_15",
        "logical_cpu_count",
        "normalized_load_1",
        "available_memory_bytes",
        "cpu_governor_states",
    }
    if not isinstance(snapshot, dict) or set(snapshot) != snapshot_fields:
        raise RuntimeError(
            f"capture bundle {field} snapshot fields are not exact")
    captured = snapshot.get("captured_monotonic_ns")
    logical_cpus = snapshot.get("logical_cpu_count")
    memory = snapshot.get("available_memory_bytes")
    if (
        isinstance(captured, bool)
        or not isinstance(captured, int)
        or captured <= 0
        or isinstance(logical_cpus, bool)
        or not isinstance(logical_cpus, int)
        or logical_cpus <= 0
        or logical_cpus
            != measurement.get("environment", {}).get(
                "logical_cpu_count")
        or isinstance(memory, bool)
        or not isinstance(memory, int)
        or memory <= 0
    ):
        raise RuntimeError(
            f"capture bundle {field} snapshot integer fields are invalid")
    numeric_loads: dict[str, float] = {}
    for metric in ("load_1", "load_5", "load_15", "normalized_load_1"):
        item = snapshot.get(metric)
        if (
            isinstance(item, bool)
            or not isinstance(item, (int, float))
            or not math.isfinite(float(item))
            or float(item) < 0.0
        ):
            raise RuntimeError(
                f"capture bundle {field} snapshot {metric} is invalid")
        numeric_loads[metric] = float(item)
    if not math.isclose(
        numeric_loads["normalized_load_1"],
        numeric_loads["load_1"] / logical_cpus,
        rel_tol=1e-12,
        abs_tol=1e-15,
    ):
        raise RuntimeError(
            f"capture bundle {field} normalized load is inconsistent")
    governors = snapshot.get("cpu_governor_states")
    if (
        not isinstance(governors, list)
        or not governors
        or len(set(governors)) != len(governors)
        or any(
            not isinstance(item, str) or not item.strip()
            for item in governors
        )
    ):
        raise RuntimeError(
            f"capture bundle {field} governor states are invalid")
    return value


def _validate_sql_guardrail_paired_capture(
        body: bytes,
        *,
        expected_run_nonce: int,
        expected_report_sha256: str,
        measurement: dict[str, object],
        measurement_file_sha256: str,
        field: str) -> dict[str, object]:
    """Validate the server-visible structure of one retained pair artifact."""
    try:
        value = json.loads(body)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"capture bundle {field} is not valid JSON") from exc
    expected_fields = {
        "schema_version",
        "artifact_kind",
        "run_nonce",
        "measurement_sha256",
        "measurement_content_sha256",
        "report_sha256",
        "raw_file_sha256",
        "source",
        "environment",
        "algorithms",
        "content_sha256",
    }
    if (
        not isinstance(value, dict)
        or set(value) != expected_fields
        or value.get("schema_version") != 1
        or value.get("artifact_kind") != "paired_runtime_samples"
        or value.get("run_nonce") != expected_run_nonce
        or value.get("measurement_sha256") != measurement_file_sha256
        or value.get("measurement_content_sha256")
            != measurement.get("content_sha256")
        or value.get("report_sha256") != expected_report_sha256
        or value.get("source") != measurement.get("source")
        or value.get("environment") != measurement.get("environment")
        or re.fullmatch(
            r"[0-9a-f]{64}", str(value.get("raw_file_sha256"))
        ) is None
    ):
        raise RuntimeError(
            f"capture bundle {field} identity/binding is invalid")
    try:
        _validate_json_content_seal(value, f"capture bundle {field}")
    except ValueError as exc:
        raise RuntimeError(str(exc)) from exc
    algorithms = value.get("algorithms")
    expected_algorithms = {
        "typed_wrapper_generated_vs_handwritten",
        "row_codec_generated_vs_handwritten",
    }
    if (
        not isinstance(algorithms, dict)
        or set(algorithms) != expected_algorithms
    ):
        raise RuntimeError(
            f"capture bundle {field} algorithm set is not exact")
    for algorithm in sorted(expected_algorithms):
        records = algorithms.get(algorithm)
        if (
            not isinstance(records, list)
            or len(records) != 15
            or [record.get("sample_ordinal") for record in records]
                != list(range(1, 16))
        ):
            raise RuntimeError(
                f"capture bundle {field} sample IDs are not exact")
        for index, record in enumerate(records):
            if not isinstance(record, dict) or set(record) != {
                "sample_ordinal",
                "generated_query_ns",
                "handwritten_query_ns",
                "generated_to_handwritten_ratio",
            }:
                raise RuntimeError(
                    f"capture bundle {field} record {index} is malformed")
            generated = record.get("generated_query_ns")
            handwritten = record.get("handwritten_query_ns")
            ratio = record.get("generated_to_handwritten_ratio")
            if (
                isinstance(generated, bool)
                or not isinstance(generated, int)
                or generated <= 0
                or generated >= (1 << 53)
                or isinstance(handwritten, bool)
                or not isinstance(handwritten, int)
                or handwritten <= 0
                or handwritten >= (1 << 53)
                or isinstance(ratio, bool)
                or not isinstance(ratio, (int, float))
                or not math.isfinite(float(ratio))
                or not math.isclose(
                    float(ratio),
                    generated / handwritten,
                    rel_tol=1e-15,
                    abs_tol=1e-15,
                )
            ):
                raise RuntimeError(
                    f"capture bundle {field} record {index} ratio "
                    "does not recompute")
    return value


def _load_sql_guardrail_capture_bundle(
        path: Path,
        *,
        expected_sha256: str) -> tuple[dict[str, object], dict[str, bytes]]:
    body = _read_sql_guardrail_evidence(
        path, expected_sha256=expected_sha256)
    try:
        value = json.loads(body)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"capture bundle is not valid JSON: {path}: {exc}") from exc
    expected_fields = {
        "schema_version",
        "artifact_kind",
        "status",
        "baseline_eligibility",
        "capture_request",
        "canonical_environment_key",
        "source",
        "environment",
        "measurement",
        "runtime_samples",
        "calibration_reports",
        "benchmark_configuration",
        "calibration_configuration",
        "artifact_inventory",
        "generator_output_inventory",
        "tool",
        "generator_executable",
        "production_dso",
        "generated_artifacts",
        "benchmark_executable",
        "comparator_dso",
        "source_input_bindings",
        "untracked_source_files",
        "content_sha256",
    }
    if (
        not isinstance(value, dict)
        or set(value) != expected_fields
        or value.get("schema_version") != 1
        or value.get("artifact_kind")
            != "doc0388_sql_codegen_guardrail_capture_bundle"
        or value.get("status") != "unreviewed_calibration"
        or value.get("baseline_eligibility")
            != "recovered_post_cutover_only"
    ):
        raise RuntimeError("SQL guardrail capture bundle schema is invalid")
    try:
        _validate_json_content_seal(value, "capture bundle")
    except ValueError as exc:
        raise RuntimeError(str(exc)) from exc
    environment_key = value.get("canonical_environment_key")
    if (
        not isinstance(environment_key, str)
        or re.fullmatch(r"[0-9a-f]{64}", environment_key) is None
    ):
        raise RuntimeError(
            "capture bundle canonical environment key is invalid")
    capture_request = value.get("capture_request")
    if not isinstance(capture_request, dict) or set(capture_request) != {
        "path",
        "file_sha256",
        "content_sha256",
        "normalized_sha256",
        "normalized_request",
    }:
        raise RuntimeError("capture bundle request provenance is invalid")
    if (
        not isinstance(capture_request.get("path"), str)
        or any(
            re.fullmatch(
                r"[0-9a-f]{64}", str(capture_request.get(field))
            ) is None
            for field in (
                "file_sha256",
                "content_sha256",
                "normalized_sha256",
            )
        )
    ):
        raise RuntimeError("capture bundle request hashes are invalid")
    normalized_request = capture_request.get("normalized_request")
    if (
        not isinstance(normalized_request, dict)
        or _canonical_json_sha256(normalized_request)
            != capture_request["normalized_sha256"]
        or normalized_request.get("phase") != "calibration_capture"
        or normalized_request.get("request_file_sha256")
            != capture_request["file_sha256"]
        or normalized_request.get("request_content_sha256")
            != capture_request["content_sha256"]
    ):
        raise RuntimeError(
            "capture bundle normalized request provenance is invalid")
    artifact_bodies: dict[str, bytes] = {}
    singleton_fields = (
        "measurement",
        "runtime_samples",
        "benchmark_configuration",
        "calibration_configuration",
        "artifact_inventory",
        "generator_output_inventory",
        "tool",
        "generator_executable",
        "production_dso",
        "benchmark_executable",
        "comparator_dso",
    )
    resolved_paths: set[Path] = set()
    for field in singleton_fields:
        artifact_path, artifact_body = (
            _sql_guardrail_capture_bundle_record(value[field], field)
        )
        if artifact_path in resolved_paths:
            raise RuntimeError(
                f"capture bundle reuses artifact path: {artifact_path}")
        resolved_paths.add(artifact_path)
        artifact_bodies[field] = artifact_body
    reports = value.get("calibration_reports")
    if not isinstance(reports, list) or len(reports) != 3:
        raise RuntimeError(
            "capture bundle must contain exactly three calibration reports")
    report_nonces: set[int] = set()
    for index, report in enumerate(reports):
        if not isinstance(report, dict) or set(report) != {
            "path",
            "bytes",
            "sha256",
            "run_nonce",
            "host_state_before",
            "host_state_after",
            "paired_runtime_samples",
        }:
            raise RuntimeError(
                f"capture bundle report {index} is malformed")
        nonce = report.get("run_nonce")
        if (
            isinstance(nonce, bool)
            or not isinstance(nonce, int)
            or nonce <= 0
            or nonce >= (1 << 53)
            or nonce in report_nonces
        ):
            raise RuntimeError(
                "capture bundle report nonces are not independent")
        report_nonces.add(nonce)
        artifact_path, artifact_body = (
            _sql_guardrail_capture_bundle_record(
                {
                    key: report[key]
                    for key in ("path", "bytes", "sha256")
                },
                f"report[{index}]",
            )
        )
        if artifact_path in resolved_paths:
            raise RuntimeError(
                f"capture bundle reuses report path: {artifact_path}")
        resolved_paths.add(artifact_path)
        artifact_bodies[f"report[{index}]"] = artifact_body
        for boundary in ("before", "after"):
            state_path, state_body = (
                _sql_guardrail_capture_bundle_record(
                    report[f"host_state_{boundary}"],
                    f"report[{index}].host_state_{boundary}",
                )
            )
            if state_path in resolved_paths:
                raise RuntimeError(
                    "capture bundle reuses runtime host-state path: "
                    f"{state_path}")
            resolved_paths.add(state_path)
            artifact_bodies[
                f"report[{index}].host_state_{boundary}"
            ] = state_body
        pair_path, pair_body = _sql_guardrail_capture_bundle_record(
            report["paired_runtime_samples"],
            f"report[{index}].paired_runtime_samples",
        )
        if pair_path in resolved_paths:
            raise RuntimeError(
                "capture bundle reuses paired runtime-sample path: "
                f"{pair_path}")
        resolved_paths.add(pair_path)
        artifact_bodies[
            f"report[{index}].paired_runtime_samples"
        ] = pair_body
    try:
        measurement = json.loads(artifact_bodies["measurement"])
        runtime_samples = json.loads(artifact_bodies["runtime_samples"])
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            "capture bundle measurement/runtime sample JSON is invalid"
        ) from exc
    if (
        not isinstance(measurement, dict)
        or not isinstance(runtime_samples, dict)
        or measurement.get("role") != "candidate"
        or measurement.get("environment", {}).get("sha256")
            != environment_key
        or runtime_samples.get("environment", {}).get("sha256")
            != environment_key
        or runtime_samples.get("measurement_sha256")
            != value["measurement"]["sha256"]
    ):
        raise RuntimeError(
            "capture bundle environment/measurement joins are invalid")
    if (
        value.get("source") != measurement.get("source")
        or value.get("environment") != measurement.get("environment")
        or runtime_samples.get("source") != measurement.get("source")
        or runtime_samples.get("environment") != measurement.get("environment")
    ):
        raise RuntimeError(
            "capture bundle source/environment records are inconsistent")
    if (
        measurement.get("measurement_context", {}).get(
            "generator_boundary", {}
        ).get("output_inventory_file_sha256")
        != value["generator_output_inventory"]["sha256"]
    ):
        raise RuntimeError(
            "capture bundle generator-output inventory is not joined "
            "to the measured cold-generator boundary")
    runtime_artifacts = measurement.get("runtime_artifacts", {})
    for bundle_field, measurement_field in (
        ("generator_executable", "generator_executable"),
        ("production_dso", "production_dso"),
        ("benchmark_executable", "benchmark_executable"),
        ("comparator_dso", "comparator_dso"),
    ):
        measured = runtime_artifacts.get(measurement_field, {})
        retained = value[bundle_field]
        if (
            retained.get("bytes") != measured.get("bytes")
            or retained.get("sha256") != measured.get("sha256")
        ):
            raise RuntimeError(
                f"capture bundle retained {bundle_field} differs from "
                "the measured runtime artifact")
    built_dso = runtime_artifacts.get("production_dso", {})
    installed_dso = runtime_artifacts.get(
        "installed_production_dso", {})
    if (
        built_dso.get("path") == installed_dso.get("path")
        or built_dso.get("bytes") != installed_dso.get("bytes")
        or built_dso.get("sha256") != installed_dso.get("sha256")
    ):
        raise RuntimeError(
            "capture bundle measurement does not prove installed/built "
            "production DSO equality")

    source_value = measurement.get("source", {})
    tracked_diff_text = source_value.get("tracked_diff_base64")
    try:
        tracked_diff = base64.b64decode(
            str(tracked_diff_text).encode("ascii"), validate=True)
    except (UnicodeEncodeError, ValueError) as exc:
        raise RuntimeError(
            "capture bundle tracked source diff is not valid base64"
        ) from exc
    if _sha256_bytes(tracked_diff) != source_value.get(
        "tracked_diff_sha256"
    ):
        raise RuntimeError(
            "capture bundle tracked source diff hash is invalid")
    source_entries = {
        entry.get("path"): entry
        for entry in source_value.get("inventory_entries", [])
        if isinstance(entry, dict)
    }
    expected_source_inputs = {
        "tool": "main/bench/sql_codegen_guardrail.py",
        "benchmark_configuration":
            "main/bench/bench_sql_codegen_guardrail_configuration.json",
        "calibration_configuration":
            "main/bench/"
            "bench_sql_codegen_guardrail_calibration_configuration.json",
    }
    source_bindings = value.get("source_input_bindings")
    if source_bindings != expected_source_inputs:
        raise RuntimeError(
            "capture bundle source-input binding map is not exact")
    for field, original_path in expected_source_inputs.items():
        entry = source_entries.get(original_path, {})
        retained = value[field]
        if (
            entry.get("bytes") != retained.get("bytes")
            or entry.get("sha256") != retained.get("sha256")
        ):
            raise RuntimeError(
                f"capture bundle {field} is not joined to the measured "
                "source inventory")
    untracked_records = value.get("untracked_source_files")
    expected_untracked = source_value.get("untracked_files")
    if (
        not isinstance(untracked_records, list)
        or not isinstance(expected_untracked, list)
        or len(untracked_records) != len(expected_untracked)
    ):
        raise RuntimeError(
            "capture bundle untracked-source retention is incomplete")
    observed_untracked: list[str] = []
    for index, record in enumerate(untracked_records):
        if not isinstance(record, dict) or set(record) != {
            "original_path", "path", "bytes", "sha256"
        }:
            raise RuntimeError(
                f"capture bundle untracked source {index} is malformed")
        original_path = record.get("original_path")
        entry = source_entries.get(original_path, {})
        if (
            not isinstance(original_path, str)
            or original_path not in expected_untracked
            or entry.get("bytes") != record.get("bytes")
            or entry.get("sha256") != record.get("sha256")
        ):
            raise RuntimeError(
                f"capture bundle untracked source {index} is not joined "
                "to the measured inventory")
        artifact_path, artifact_body = (
            _sql_guardrail_capture_bundle_record(
                {
                    key: record[key]
                    for key in ("path", "bytes", "sha256")
                },
                f"untracked_source[{index}]",
            )
        )
        if artifact_path in resolved_paths:
            raise RuntimeError(
                f"capture bundle reuses untracked path: {artifact_path}")
        resolved_paths.add(artifact_path)
        artifact_bodies[f"untracked_source[{index}]"] = artifact_body
        observed_untracked.append(original_path)
    if observed_untracked != expected_untracked:
        raise RuntimeError(
            "capture bundle untracked source ordering is not exact")
    runtime_reports = runtime_samples.get(
        "provenance", {}
    ).get("reports", [])
    if (
        not isinstance(runtime_reports, list)
        or [item.get("sha256") for item in runtime_reports]
            != [item["sha256"] for item in reports]
        or [item.get("run_nonce") for item in runtime_reports]
            != [item["run_nonce"] for item in reports]
    ):
        raise RuntimeError(
            "capture bundle runtime report joins are invalid")
    previous_host_capture: int | None = None
    for index, (report, runtime_report) in enumerate(zip(
        reports, runtime_reports, strict=True
    )):
        runtime_host_state = runtime_report.get("host_state")
        if not isinstance(runtime_host_state, dict) or set(
            runtime_host_state
        ) != {
            "before_file_sha256",
            "before_content_sha256",
            "before",
            "after_file_sha256",
            "after_content_sha256",
            "after",
        }:
            raise RuntimeError(
                f"capture bundle runtime report {index} host state "
                "is malformed")
        runtime_pairs = runtime_report.get("paired_runtime_samples")
        pair_record = report["paired_runtime_samples"]
        pair_document = _validate_sql_guardrail_paired_capture(
            artifact_bodies[
                f"report[{index}].paired_runtime_samples"
            ],
            expected_run_nonce=report["run_nonce"],
            expected_report_sha256=report["sha256"],
            measurement=measurement,
            measurement_file_sha256=value["measurement"]["sha256"],
            field=f"report[{index}].paired_runtime_samples",
        )
        if (
            not isinstance(runtime_pairs, dict)
            or set(runtime_pairs) != {
                "file_sha256", "content_sha256"
            }
            or runtime_pairs.get("file_sha256")
                != pair_record["sha256"]
            or runtime_pairs.get("content_sha256")
                != pair_document["content_sha256"]
        ):
            raise RuntimeError(
                f"capture bundle runtime report {index} paired "
                "runtime-sample join is invalid")
        for boundary in ("before", "after"):
            record = report[f"host_state_{boundary}"]
            document = _validate_sql_guardrail_host_state_capture(
                artifact_bodies[
                    f"report[{index}].host_state_{boundary}"
                ],
                expected_phase=boundary,
                expected_run_nonce=report["run_nonce"],
                measurement=measurement,
                measurement_file_sha256=value["measurement"]["sha256"],
                field=f"report[{index}].host_state_{boundary}",
            )
            if (
                runtime_host_state[f"{boundary}_file_sha256"]
                    != record["sha256"]
                or runtime_host_state[f"{boundary}_content_sha256"]
                    != document["content_sha256"]
                or runtime_host_state[boundary] != document["snapshot"]
            ):
                raise RuntimeError(
                    f"capture bundle runtime report {index} {boundary} "
                    "host-state join is invalid")
            captured = document["snapshot"]["captured_monotonic_ns"]
            if (
                previous_host_capture is not None
                and captured <= previous_host_capture
            ):
                raise RuntimeError(
                    "capture bundle runtime host-state captures are not "
                    "strictly monotonic")
            previous_host_capture = captured
    generated_records = value.get("generated_artifacts")
    if not isinstance(generated_records, list) or not generated_records:
        raise RuntimeError(
            "capture bundle generated artifact list is empty")
    expected_generated = []
    for group in ("sql", "json", "cpp"):
        group_value = measurement.get("artifacts", {}).get(group, {})
        entries = group_value.get("entries", [])
        if not isinstance(entries, list):
            raise RuntimeError(
                "capture bundle measurement artifact inventory is invalid")
        for entry in entries:
            expected_generated.append((
                group,
                entry.get("path"),
                entry.get("bytes"),
                entry.get("sha256"),
            ))
    observed_generated = []
    for index, record in enumerate(generated_records):
        if not isinstance(record, dict) or set(record) != {
            "group", "original_path", "path", "bytes", "sha256"
        }:
            raise RuntimeError(
                f"capture bundle generated artifact {index} is malformed")
        group = record.get("group")
        original_path = record.get("original_path")
        if (
            group not in {"sql", "json", "cpp"}
            or not isinstance(original_path, str)
            or not original_path
        ):
            raise RuntimeError(
                f"capture bundle generated artifact {index} identity is invalid")
        artifact_path, artifact_body = (
            _sql_guardrail_capture_bundle_record(
                {
                    key: record[key]
                    for key in ("path", "bytes", "sha256")
                },
                f"generated_artifact[{index}]",
            )
        )
        if artifact_path in resolved_paths:
            raise RuntimeError(
                f"capture bundle reuses generated path: {artifact_path}")
        resolved_paths.add(artifact_path)
        artifact_bodies[f"generated_artifact[{index}]"] = artifact_body
        observed_generated.append((
            group,
            original_path,
            record["bytes"],
            record["sha256"],
        ))
    if observed_generated != expected_generated:
        raise RuntimeError(
            "capture bundle generated artifacts do not match measurement")
    return value, artifact_bodies


def _run_sql_guardrail_workflow(
        *,
        request: dict[str, object],
        active_build_dir: Path,
        active_preset: str,
        env: dict[str, str],
        log_handle,
        deadline: float,
) -> tuple[Path | None, dict[str, object]]:
    """Execute DOC0388 capture/qualification inside the MCP lane."""
    phase = str(request["phase"])
    if phase == "baseline_promotion":
        raise RuntimeError(
            "baseline promotion must not enter the build workflow")
    manifest = _sql_guardrail_workflow_manifest(active_build_dir)
    tool_prefix = [sys.executable, manifest["tool"]]

    def run_checked(
            argv: list[str],
            *,
            child_env: dict[str, str] | None = None,
            step: str) -> None:
        _update_entry("bench-e2e", step=step)
        rc = _stream_subprocess(
            argv,
            log_handle,
            deadline,
            cwd=active_build_dir,
            env=child_env or env,
        )
        if rc != 0:
            raise RuntimeError(
                f"SQL guardrail workflow step '{step}' exited with code {rc}")

    benchmark_path = Path(manifest["benchmark_executable"]).resolve(
        strict=True)
    comparator_path = Path(manifest["comparator_dso"]).resolve(strict=True)
    generator_path = Path(manifest["generator_executable"]).resolve(
        strict=True)
    production_path = Path(manifest["production_dso"]).resolve(strict=True)
    active_tree = active_build_dir.resolve(strict=True)
    benchmark_path.relative_to(active_tree)
    comparator_path.relative_to(active_tree)
    generator_path.relative_to(active_tree)
    production_path.relative_to(active_tree)
    if (
        len({
            benchmark_path,
            comparator_path,
            generator_path,
            production_path,
        }) != 4
        or not os.access(benchmark_path, os.X_OK)
        or not os.access(generator_path, os.X_OK)
    ):
        raise RuntimeError(
            "SQL guardrail runtime target paths are invalid")
    if (
        "bioseq_sql_guardrail_comparators" not in comparator_path.name
        or "postgresql_bioseq" in comparator_path.name
    ):
        raise RuntimeError(
            "SQL guardrail comparator is not the exact test-only MODULE DSO")

    output_anchor = Path(
        request[
            "capture_bundle_output"
            if phase == "calibration_capture"
            else "qualification_output"
        ]
    )
    if output_anchor.exists():
        raise RuntimeError(
            f"SQL guardrail evidence output already exists: {output_anchor}")
    artifact_directory = output_anchor.with_name(
        output_anchor.name + ".artifacts")
    artifact_directory.parent.resolve(strict=True).relative_to(
        WORKSPACE_ROOT.resolve(strict=True))
    artifact_directory.mkdir(mode=0o700, exist_ok=False)
    measurement_preinstall = (
        artifact_directory / "measurement_preinstall.json"
    )
    measurement = artifact_directory / "measurement.json"

    baseline_snapshot: Path | None = None
    if phase == "candidate_qualification":
        baseline_body = _read_sql_guardrail_evidence(
            Path(str(request["baseline"])),
            expected_sha256=str(request["baseline_sha256"]),
        )
        try:
            baseline_value = json.loads(baseline_body)
        except json.JSONDecodeError as exc:
            raise RuntimeError("candidate baseline is not valid JSON") from exc
        if (
            not isinstance(baseline_value, dict)
            or baseline_value.get("artifact_kind") != "reviewed_baseline"
            or baseline_value.get("canonical_environment_key")
                != request["canonical_environment_key"]
        ):
            raise RuntimeError(
                "candidate baseline canonical environment binding is invalid")
        baseline_snapshot = artifact_directory / "reviewed_baseline.json"
        _write_sql_guardrail_evidence_exclusive(
            baseline_snapshot, baseline_body)
        publication_path, publication = (
            _validate_sql_guardrail_baseline_publication(
                baseline=Path(str(request["baseline"])),
                baseline_sha256=str(request["baseline_sha256"]),
                canonical_environment_key=str(
                    request["canonical_environment_key"]),
            )
        )
        if (
            str(publication_path)
                != request["baseline_publication_receipt"]
            or _sha256_path(publication_path)
                != request["baseline_publication_receipt_sha256"]
            or publication["publication_id"]
                != request["baseline_publication_id"]
        ):
            raise RuntimeError(
                "candidate baseline publication changed after scheduling")
        promotion_request = publication["promotion_request"]
        capture_bundle_path = Path(
            str(promotion_request["capture_bundle"]))
        capture_bundle, captured_bodies = (
            _load_sql_guardrail_capture_bundle(
                capture_bundle_path,
                expected_sha256=str(
                    promotion_request["capture_bundle_sha256"]),
            )
        )
        limits_body = _read_sql_guardrail_evidence(
            Path(str(promotion_request["limits"])),
            expected_sha256=str(promotion_request["limits_sha256"]),
        )
        with tempfile.TemporaryDirectory(
                prefix="bioseq-sql-baseline-rederive-") as temporary:
            rederive_directory = Path(temporary)
            captured_measurement = (
                rederive_directory / "measurement.json")
            captured_runtime = (
                rederive_directory / "runtime_samples.json")
            captured_limits = rederive_directory / "limits.json"
            captured_tool = (
                rederive_directory / "sql_codegen_guardrail.py")
            rederived_baseline = (
                rederive_directory / "reviewed_baseline.json")
            captured_measurement.write_bytes(
                captured_bodies["measurement"])
            captured_runtime.write_bytes(
                captured_bodies["runtime_samples"])
            captured_limits.write_bytes(limits_body)
            captured_tool.write_bytes(captured_bodies["tool"])
            for index, report_record in enumerate(
                    capture_bundle["calibration_reports"]):
                captured_report = (
                    rederive_directory / f"report_{index}.json")
                captured_pairs = (
                    rederive_directory / f"paired_{index}.json")
                captured_report.write_bytes(
                    captured_bodies[f"report[{index}]"])
                captured_pairs.write_bytes(
                    captured_bodies[
                        f"report[{index}].paired_runtime_samples"
                    ])
                run_checked(
                    [
                        sys.executable,
                        str(captured_tool),
                        "validate-paired-runtime-samples",
                        "--paired-runtime-samples",
                        str(captured_pairs),
                        "--report", str(captured_report),
                        "--measurement", str(captured_measurement),
                        "--expected-run-nonce",
                        str(report_record["run_nonce"]),
                    ],
                    step=(
                        "revalidating published baseline paired evidence "
                        f"{index + 1}"
                    ),
                )
            run_checked(
                [
                    sys.executable,
                    str(captured_tool),
                    "review",
                    "--measurement", str(captured_measurement),
                    "--runtime-samples", str(captured_runtime),
                    "--limits", str(captured_limits),
                    "--capture-bundle-sha256",
                    str(promotion_request["capture_bundle_sha256"]),
                    "--review-request-sha256",
                    str(promotion_request["request_file_sha256"]),
                    "--review-request-content-sha256",
                    str(promotion_request[
                        "request_content_sha256"]),
                    "--baseline-kind", "recovered_post_cutover",
                    "--reviewer", str(promotion_request["reviewer"]),
                    "--evidence-reference",
                    str(promotion_request["evidence_reference"]),
                    "--spec-deviation",
                    str(promotion_request["spec_deviation"]),
                    "--approval-reference",
                    str(promotion_request["approval_reference"]),
                    "--output", str(rederived_baseline),
                ],
                step=(
                    "independently rederiving published SQL guardrail "
                    "baseline from immutable capture"
                ),
            )
            run_checked(
                [
                    sys.executable,
                    str(captured_tool),
                    "validate-baseline",
                    "--baseline", str(rederived_baseline),
                ],
                step="validating independently rederived SQL baseline",
            )
            if rederived_baseline.read_bytes() != baseline_body:
                raise RuntimeError(
                    "candidate baseline does not exactly rederive from its "
                    "immutable capture, limits, request, and retained tool")

    run_checked(
        [
            *tool_prefix,
            "mark-build",
            "--repository-root", manifest["repository_root"],
            "--cmake-source-dir", manifest["cmake_source_dir"],
            "--build-dir", manifest["build_dir"],
        ],
        step="marking dedicated SQL guardrail build tree",
    )

    run_checked(
        [
            *tool_prefix,
            "measure",
            "--repository-root", manifest["repository_root"],
            "--cmake-source-dir", manifest["cmake_source_dir"],
            "--build-dir", manifest["build_dir"],
            "--role", "candidate",
            "--affected-target", "postgresql_bioseq",
            "--affected-target", "bioseq_bench_e2e",
            "--artifact-inventory", manifest["artifact_inventory"],
            "--generator-output-inventory",
            manifest["generator_output_inventory"],
            "--generator-executable", manifest["generator_executable"],
            "--production-dso", manifest["production_dso"],
            "--benchmark-executable", manifest["benchmark_executable"],
            "--comparator-dso", manifest["comparator_dso"],
            "--output", str(measurement_preinstall),
        ],
        step="measuring SQL guardrail candidate build evidence",
    )

    # Measurement deliberately cleans and rebuilds exact targets. Reinstall
    # that final measured extension and restart PostgreSQL before any SQL
    # sample, closing stale-installed-DSO and dyld/dlopen cache gaps.
    run_checked(
        _cmake_install_cmd(active_build_dir, active_preset),
        step="installing measured SQL guardrail extension",
    )
    run_checked(
        [
            *tool_prefix,
            "bind-installed-dso",
            "--measurement", str(measurement_preinstall),
            "--installed-production-dso",
            manifest["installed_production_dso"],
            "--output", str(measurement),
        ],
        step="binding installed SQL guardrail production DSO",
    )
    measurement_value = json.loads(
        _read_sql_guardrail_evidence(measurement))
    environment_key = (
        measurement_value.get("environment", {}).get("sha256")
        if isinstance(measurement_value, dict) else None
    )
    if (
        not isinstance(environment_key, str)
        or re.fullmatch(r"[0-9a-f]{64}", environment_key) is None
    ):
        raise RuntimeError(
            "SQL guardrail measurement has no canonical environment key")
    if (
        phase == "candidate_qualification"
        and environment_key != request["canonical_environment_key"]
    ):
        raise RuntimeError(
            "candidate measurement canonical environment differs from request")
    _restart_postgresql(
        active_build_dir, active_preset, log_handle, deadline)
    _prepare_aws_postgresql_test_database(
        active_preset, log_handle, deadline)

    exact_benchmark_args = [
        manifest["benchmark_executable"],
        "--filter",
        "benchmark=bench_sql_codegen_guardrail",
    ]
    issued_run_nonces: set[int] = set()
    live_authorities: dict[int, Path] = {}

    def run_benchmark(
            *, calibration: bool) -> tuple[Path, int, Path, Path, Path]:
        reports_before = _report_paths(MODES["bench-e2e"].reporting_dir)
        run_env = env.copy()
        run_env["BIOSEQ_SQL_GUARDRAIL_BENCHMARK_EXECUTABLE"] = (
            manifest["benchmark_executable"])
        while True:
            run_nonce = secrets.randbelow((1 << 53) - 1) + 1
            if run_nonce not in issued_run_nonces:
                issued_run_nonces.add(run_nonce)
                break
        run_env["BIOSEQ_SQL_GUARDRAIL_RUN_NONCE"] = str(run_nonce)
        host_state_before = (
            artifact_directory
            / f"runtime_host_state_before_{run_nonce}.json"
        )
        host_state_after = (
            artifact_directory
            / f"runtime_host_state_after_{run_nonce}.json"
        )
        paired_samples_raw = (
            artifact_directory
            / f"paired_runtime_samples_raw_{run_nonce}.jsonl"
        )
        paired_samples = (
            artifact_directory
            / f"paired_runtime_samples_{run_nonce}.json"
        )
        run_env[
            "BIOSEQ_SQL_GUARDRAIL_PAIRED_SAMPLES_RAW_PATH"
        ] = str(paired_samples_raw)
        authority_broker: (
            _SqlGuardrailLiveAuthorityBroker | None
        ) = None
        if calibration:
            run_env.pop(
                "BIOSEQ_SQL_GUARDRAIL_MCP_AUTHORITY_PATH", None)
            run_env.pop(
                "BIOSEQ_SQL_GUARDRAIL_MCP_AUTHORITY_SOCKET", None)
            run_env["BIOSEQ_SQL_GUARDRAIL_CALIBRATION"] = "1"
            run_env["BIOSEQ_SQL_GUARDRAIL_CALIBRATION_MEASUREMENT"] = (
                measurement)
        else:
            run_env.pop("BIOSEQ_SQL_GUARDRAIL_CALIBRATION", None)
            run_env.pop(
                "BIOSEQ_SQL_GUARDRAIL_CALIBRATION_MEASUREMENT", None)
            if baseline_snapshot is None:
                raise RuntimeError(
                    "qualification benchmark has no immutable baseline snapshot")
            run_env["BIOSEQ_SQL_GUARDRAIL_QUALIFICATION_PATH"] = str(
                qualification_candidate)
            run_env[
                "BIOSEQ_SQL_GUARDRAIL_REVIEWED_BASELINE_PATH"
            ] = str(baseline_snapshot)
            authority_path = (
                artifact_directory
                / f"live_mcp_authority_{run_nonce}.json"
            )
            broker_path = (
                artifact_directory
                / f"live_mcp_authority_{run_nonce}.sock"
            )
            _create_sql_guardrail_live_authority(
                output=authority_path,
                request=request,
                run_nonce=run_nonce,
                qualification=qualification_candidate,
                baseline=baseline_snapshot,
                measurement=measurement,
                broker_socket=broker_path,
            )
            authority_broker = _SqlGuardrailLiveAuthorityBroker(
                path=broker_path,
                run_nonce=run_nonce,
                authority_file_sha256=_sha256_path(authority_path),
                request_snapshot_sha256=_canonical_json_sha256(request),
            )
            run_env[
                "BIOSEQ_SQL_GUARDRAIL_MCP_AUTHORITY_PATH"
            ] = str(authority_path)
            run_env[
                "BIOSEQ_SQL_GUARDRAIL_MCP_AUTHORITY_SOCKET"
            ] = str(broker_path)
            live_authorities[run_nonce] = authority_path
        run_checked(
            [
                *tool_prefix,
                "capture-runtime-host-state",
                "--measurement", str(measurement),
                "--phase", "before",
                "--run-nonce", str(run_nonce),
                "--output", str(host_state_before),
            ],
            step="capturing pre-report SQL guardrail host state",
        )
        if authority_broker is not None:
            authority_broker.start()
        try:
            run_checked(
                exact_benchmark_args,
                child_env=run_env,
                step=(
                    "running SQL guardrail calibration report"
                    if calibration
                    else "running SQL guardrail qualification report"
                ),
            )
        finally:
            if authority_broker is not None:
                authority_broker.stop()
        if (
            authority_broker is not None
            and not authority_broker.consumed
        ):
            raise RuntimeError(
                "qualification benchmark did not consume its one-use "
                "live MCP authority")
        run_checked(
            [
                *tool_prefix,
                "capture-runtime-host-state",
                "--measurement", str(measurement),
                "--phase", "after",
                "--run-nonce", str(run_nonce),
                "--output", str(host_state_after),
            ],
            step="capturing post-report SQL guardrail host state",
        )
        report = _newest_report(reports_before, "bench-e2e")
        if report is None:
            raise RuntimeError(
                "SQL guardrail benchmark produced no final framework report")
        run_checked(
            [
                *tool_prefix,
                "seal-paired-runtime-samples",
                "--raw-jsonl", str(paired_samples_raw),
                "--report", str(report),
                "--measurement", str(measurement),
                "--run-nonce", str(run_nonce),
                "--output", str(paired_samples),
            ],
            step="sealing exact paired SQL guardrail runtime samples",
        )
        return (
            report,
            run_nonce,
            host_state_before,
            host_state_after,
            paired_samples,
        )

    evidence: dict[str, object] = {
        "workflow_request": request["request_path"],
        "workflow_request_sha256": request["request_file_sha256"],
        "workflow_request_snapshot_sha256":
            _canonical_json_sha256(request),
        "measurement": str(measurement),
        "measurement_sha256": _sha256_path(measurement),
        "canonical_environment_key": environment_key,
        "environment_specific": True,
    }
    if phase == "calibration_capture":
        live_reports = [
            run_benchmark(calibration=True)
            for _ in range(3)
        ]
        reports: list[Path] = []
        report_records: list[dict[str, object]] = []
        for index, (
            live_report,
            run_nonce,
            host_state_before,
            host_state_after,
            paired_samples,
        ) in enumerate(
                live_reports, start=1):
            report_copy = (
                artifact_directory / f"calibration_report_{index}.json"
            )
            _write_sql_guardrail_evidence_exclusive(
                report_copy,
                _read_sql_guardrail_evidence(live_report),
            )
            record = _sql_guardrail_artifact_record(report_copy)
            record["run_nonce"] = run_nonce
            record["host_state_before"] = (
                _sql_guardrail_artifact_record(host_state_before)
            )
            record["host_state_after"] = (
                _sql_guardrail_artifact_record(host_state_after)
            )
            record["paired_runtime_samples"] = (
                _sql_guardrail_artifact_record(paired_samples)
            )
            reports.append(report_copy)
            report_records.append(record)
        runtime_samples = artifact_directory / "runtime_samples.json"
        extract_args = [
            *tool_prefix,
            "extract-runtime-samples",
            "--measurement", str(measurement),
            "--output", str(runtime_samples),
        ]
        for report, live_report in zip(
            reports, live_reports, strict=True
        ):
            extract_args.extend([
                "--report", str(report),
                "--host-state-before", str(live_report[2]),
                "--host-state-after", str(live_report[3]),
                "--paired-runtime-samples", str(live_report[4]),
            ])
        run_checked(
            extract_args,
            step="extracting independent SQL guardrail runtime samples",
        )
        retained_inputs: dict[str, Path] = {}
        for field in (
            "tool",
            "benchmark_configuration",
            "calibration_configuration",
            "artifact_inventory",
            "generator_output_inventory",
            "generator_executable",
            "production_dso",
            "benchmark_executable",
            "comparator_dso",
        ):
            source = Path(manifest[field])
            retained = artifact_directory / (
                f"retained_{field}{source.suffix}"
            )
            _write_sql_guardrail_evidence_exclusive(
                retained, _read_sql_guardrail_evidence(source))
            retained_inputs[field] = retained
        source_input_bindings = {
            "tool": "main/bench/sql_codegen_guardrail.py",
            "benchmark_configuration":
                "main/bench/"
                "bench_sql_codegen_guardrail_configuration.json",
            "calibration_configuration":
                "main/bench/"
                "bench_sql_codegen_guardrail_calibration_configuration.json",
        }
        source_entries = {
            entry["path"]: entry
            for entry in measurement_value["source"][
                "inventory_entries"
            ]
        }
        for field, original_path in source_input_bindings.items():
            retained_record = _sql_guardrail_artifact_record(
                retained_inputs[field])
            source_entry = source_entries.get(original_path)
            if (
                source_entry is None
                or source_entry.get("bytes")
                    != retained_record["bytes"]
                or source_entry.get("sha256")
                    != retained_record["sha256"]
            ):
                raise RuntimeError(
                    f"retained {field} differs from measured source "
                    "inventory")
        untracked_source_records: list[dict[str, object]] = []
        for index, original_path in enumerate(
            measurement_value["source"]["untracked_files"]
        ):
            source = WORKSPACE_ROOT / original_path
            retained = artifact_directory / (
                f"untracked_source_{index:04d}_{source.name}"
            )
            _write_sql_guardrail_evidence_exclusive(
                retained, source.read_bytes())
            record = _sql_guardrail_artifact_record(retained)
            source_entry = source_entries.get(original_path)
            if (
                source_entry is None
                or source_entry.get("bytes") != record["bytes"]
                or source_entry.get("sha256") != record["sha256"]
            ):
                raise RuntimeError(
                    f"untracked source changed after measurement: "
                    f"{original_path}")
            record["original_path"] = original_path
            untracked_source_records.append(record)
        generated_records: list[dict[str, object]] = []
        for group in ("sql", "json", "cpp"):
            entries = measurement_value["artifacts"][group]["entries"]
            for index, entry in enumerate(entries):
                source = Path(entry["path"])
                retained = artifact_directory / (
                    f"generated_{group}_{index:04d}_{source.name}"
                )
                _write_sql_guardrail_evidence_exclusive(
                    retained, _read_sql_guardrail_evidence(source))
                record = _sql_guardrail_artifact_record(retained)
                record.update({
                    "group": group,
                    "original_path": entry["path"],
                })
                generated_records.append(record)
        capture_bundle = {
            "schema_version": 1,
            "artifact_kind":
                "doc0388_sql_codegen_guardrail_capture_bundle",
            "status": "unreviewed_calibration",
            "baseline_eligibility": "recovered_post_cutover_only",
            "capture_request": {
                "path": str(request["request_path"]),
                "file_sha256": str(request["request_file_sha256"]),
                "content_sha256":
                    str(request["request_content_sha256"]),
                "normalized_sha256":
                    _canonical_json_sha256(request),
                "normalized_request": request,
            },
            "canonical_environment_key": environment_key,
            "source": measurement_value["source"],
            "environment": measurement_value["environment"],
            "measurement":
                _sql_guardrail_artifact_record(measurement),
            "runtime_samples":
                _sql_guardrail_artifact_record(runtime_samples),
            "calibration_reports": report_records,
            "generated_artifacts": generated_records,
            "source_input_bindings": source_input_bindings,
            "untracked_source_files": untracked_source_records,
            **{
                field: _sql_guardrail_artifact_record(path)
                for field, path in retained_inputs.items()
            },
        }
        capture_bundle["content_sha256"] = _canonical_json_sha256(
            capture_bundle)
        capture_output = Path(str(request["capture_bundle_output"]))
        _write_sql_guardrail_evidence_exclusive(
            capture_output,
            (json.dumps(
                capture_bundle, indent=2, sort_keys=True
            ) + "\n").encode("utf-8"),
        )
        _load_sql_guardrail_capture_bundle(
            capture_output,
            expected_sha256=_sha256_path(capture_output),
        )
        evidence.update({
            "calibration_reports": [str(report) for report in reports],
            "runtime_samples": str(runtime_samples),
            "capture_bundle": str(capture_output),
            "capture_bundle_sha256": _sha256_path(capture_output),
            "baseline_eligibility": "recovered_post_cutover_only",
            "reviewed_baseline": None,
            "qualification": None,
        })
        return live_reports[-1][0], evidence

    qualification_candidate = artifact_directory / "qualification.json"
    if baseline_snapshot is None:
        raise RuntimeError("candidate qualification baseline snapshot is absent")
    run_checked(
        [
            *tool_prefix,
            "qualify",
            "--baseline", str(baseline_snapshot),
            "--measurement", str(measurement),
            "--qualification-request-sha256",
            str(request["request_file_sha256"]),
            "--qualification-request-content-sha256",
            str(request["request_content_sha256"]),
            "--qualification-request-normalized-sha256",
            _canonical_json_sha256(request),
            "--output", str(qualification_candidate),
        ],
        step="sealing SQL guardrail candidate qualification",
    )
    qualification_body = _read_sql_guardrail_evidence(
        qualification_candidate)
    (
        live_report,
        report_nonce,
        report_host_state_before,
        report_host_state_after,
        report_paired_runtime_samples,
    ) = run_benchmark(calibration=False)
    report = artifact_directory / "qualification_report.json"
    _write_sql_guardrail_evidence_exclusive(
        report, _read_sql_guardrail_evidence(live_report))
    run_checked(
        [
            *tool_prefix,
            "validate-qualification-report",
            "--report", str(report),
            "--measurement", str(measurement),
            "--qualification", str(qualification_candidate),
            "--baseline", str(baseline_snapshot),
            "--host-state-before", str(report_host_state_before),
            "--host-state-after", str(report_host_state_after),
            "--paired-runtime-samples",
            str(report_paired_runtime_samples),
            "--expected-run-nonce", str(report_nonce),
            "--qualification-request-sha256",
            str(request["request_file_sha256"]),
            "--qualification-request-content-sha256",
            str(request["request_content_sha256"]),
            "--qualification-request-normalized-sha256",
            _canonical_json_sha256(request),
        ],
        step="independently validating final SQL guardrail report",
    )
    # Publish the qualification only after the nonce-bound final report and
    # its content-sealed host-state boundaries have independently passed.
    # A failed candidate run must never leave a pass-looking public artifact.
    _write_sql_guardrail_evidence_exclusive(
        output_anchor, qualification_body)
    qualification = output_anchor
    active_qualification = Path(manifest["qualification"])
    temporary_active = active_qualification.with_name(
        active_qualification.name + f".tmp-{os.getpid()}")
    _write_sql_guardrail_evidence_exclusive(
        temporary_active, qualification_body)
    os.replace(temporary_active, active_qualification)
    evidence.update({
        "reviewed_baseline": str(baseline_snapshot),
        "reviewed_baseline_sha256": request["baseline_sha256"],
        "qualification": str(qualification),
        "qualification_sha256": _sha256_path(qualification),
        "qualification_report": str(report),
        "qualification_report_sha256": _sha256_path(report),
        "qualification_report_host_state_before":
            str(report_host_state_before),
        "qualification_report_host_state_before_sha256":
            _sha256_path(report_host_state_before),
        "qualification_report_host_state_after":
            str(report_host_state_after),
        "qualification_report_host_state_after_sha256":
            _sha256_path(report_host_state_after),
        "qualification_report_paired_runtime_samples":
            str(report_paired_runtime_samples),
        "qualification_report_paired_runtime_samples_sha256":
            _sha256_path(report_paired_runtime_samples),
        "qualification_live_mcp_authority":
            str(live_authorities[report_nonce]),
        "qualification_live_mcp_authority_sha256":
            _sha256_path(live_authorities[report_nonce]),
        "baseline_publication_receipt":
            request["baseline_publication_receipt"],
        "baseline_publication_receipt_sha256":
            request["baseline_publication_receipt_sha256"],
        "baseline_publication_id":
            request["baseline_publication_id"],
    })
    return report, evidence


def _run_sql_guardrail_promotion(
        *,
        request: dict[str, object],
        log_handle,
        deadline: float,
) -> tuple[None, dict[str, object]]:
    """Promote one immutable capture in a build-free reviewer-owned call."""
    if request.get("phase") != "baseline_promotion":
        raise RuntimeError("invalid SQL guardrail promotion phase")
    bundle_path = Path(str(request["capture_bundle"]))
    bundle, artifact_bodies = _load_sql_guardrail_capture_bundle(
        bundle_path,
        expected_sha256=str(request["capture_bundle_sha256"]),
    )
    limits_body = _read_sql_guardrail_evidence(
        Path(str(request["limits"])),
        expected_sha256=str(request["limits_sha256"]),
    )
    output = Path(str(request["baseline_output"]))
    if output.exists():
        raise RuntimeError(
            f"reviewed baseline output already exists: {output}")

    def run_checked(argv: list[str], step: str) -> None:
        _update_entry("bench-e2e", step=step)
        rc = _stream_subprocess(
            argv,
            log_handle,
            deadline,
            cwd=WORKSPACE_ROOT,
            env=_build_worker_descendant_environment(),
        )
        if rc != 0:
            raise RuntimeError(
                f"SQL guardrail promotion step '{step}' exited with code {rc}")

    with tempfile.TemporaryDirectory(
            prefix="bioseq-sql-guardrail-promotion-") as temporary:
        directory = Path(temporary)
        measurement = directory / "measurement.json"
        runtime_samples = directory / "runtime_samples.json"
        limits = directory / "limits.json"
        tool = directory / "sql_codegen_guardrail.py"
        candidate = directory / "reviewed_baseline.json"
        measurement.write_bytes(artifact_bodies["measurement"])
        runtime_samples.write_bytes(artifact_bodies["runtime_samples"])
        limits.write_bytes(limits_body)
        # Promotion is an interpretation of the immutable capture. Execute
        # the exact tool bytes measured and retained by that capture, never
        # a mutable workspace copy that may have drifted after calibration.
        tool.write_bytes(artifact_bodies["tool"])
        for index, report_record in enumerate(
                bundle["calibration_reports"]):
            retained_report = directory / f"report_{index}.json"
            retained_pairs = directory / f"paired_{index}.json"
            retained_report.write_bytes(
                artifact_bodies[f"report[{index}]"])
            retained_pairs.write_bytes(
                artifact_bodies[
                    f"report[{index}].paired_runtime_samples"
                ])
            run_checked(
                [
                    sys.executable,
                    str(tool),
                    "validate-paired-runtime-samples",
                    "--paired-runtime-samples", str(retained_pairs),
                    "--report", str(retained_report),
                    "--measurement", str(measurement),
                    "--expected-run-nonce",
                    str(report_record["run_nonce"]),
                ],
                (
                    "independently validating retained paired SQL "
                    f"runtime samples {index + 1}"
                ),
            )
        run_checked(
            [
                sys.executable,
                str(tool),
                "review",
                "--measurement", str(measurement),
                "--runtime-samples", str(runtime_samples),
                "--limits", str(limits),
                "--capture-bundle-sha256",
                str(request["capture_bundle_sha256"]),
                "--review-request-sha256",
                str(request["request_file_sha256"]),
                "--review-request-content-sha256",
                str(request["request_content_sha256"]),
                "--baseline-kind", "recovered_post_cutover",
                "--reviewer", str(request["reviewer"]),
                "--evidence-reference",
                str(request["evidence_reference"]),
                "--spec-deviation", str(request["spec_deviation"]),
                "--approval-reference",
                str(request["approval_reference"]),
                "--output", str(candidate),
            ],
            "validating and sealing reviewed SQL guardrail baseline",
        )
        run_checked(
            [
                sys.executable,
                str(tool),
                "validate-baseline",
                "--baseline", str(candidate),
            ],
            "independently validating reviewed SQL guardrail baseline",
        )
        candidate_body = candidate.read_bytes()
        candidate_value = json.loads(candidate_body)
        if (
            candidate_value.get("canonical_environment_key")
                != bundle["canonical_environment_key"]
            or candidate_value.get("capture_bundle_sha256")
                != request["capture_bundle_sha256"]
        ):
            raise RuntimeError(
                "promoted baseline does not bind the capture environment/bundle")
        _write_sql_guardrail_evidence_exclusive(output, candidate_body)

    publication_path, publication = _publish_sql_guardrail_baseline(
        baseline=output,
        baseline_value=candidate_value,
        request=request,
    )
    evidence = {
        "workflow_request": request["request_path"],
        "workflow_request_sha256": request["request_file_sha256"],
        "workflow_request_snapshot_sha256":
            _canonical_json_sha256(request),
        "capture_bundle": str(bundle_path),
        "capture_bundle_sha256": request["capture_bundle_sha256"],
        "canonical_environment_key":
            bundle["canonical_environment_key"],
        "reviewed_baseline": str(output),
        "reviewed_baseline_sha256": _sha256_path(output),
        "baseline_publication_receipt": str(publication_path),
        "baseline_publication_receipt_sha256":
            _sha256_path(publication_path),
        "baseline_publication_id": publication["publication_id"],
        "qualification": None,
        "environment_specific": True,
    }
    return None, evidence


def _run_command_argv(mode: str, active_bin_dir: Path,
                      command: List[str], bench_flags=None,
                      test_flags=None,
                      report_path: Path | None = None) -> List[str]:
    """Resolve one configured command and apply its mode-scoped arguments."""
    if not command:
        raise ValueError("configured run command may not be empty")
    if Path(command[0]).suffix.lower() in {".py", ".pl", ".pm"}:
        raise ValueError(
            f"cache/build mode runner must be native: {command[0]}")

    artifact_values: dict[str, str] | None = None
    resolved_arguments: list[str] = []
    for argument in command[1:]:
        if argument == "{report}":
            if report_path is None:
                raise ValueError(
                    f"mode {mode!r} requires a generated report path")
            resolved_arguments.append(str(report_path))
            continue
        if argument.startswith("{") and argument.endswith("}"):
            key = argument[1:-1]
            if key not in _CACHE_NATIVE_ARTIFACT_KEYS:
                raise ValueError(
                    f"unknown native cache command placeholder: {argument}")
            if artifact_values is None:
                artifact_values = _cache_native_artifacts(active_bin_dir)
            value = artifact_values[key]
            if not value:
                raise RuntimeError(
                    f"native cache artifact {key!r} is unavailable in "
                    f"the {mode!r} build tree")
            artifact_path = Path(value)
            directory_key = key.endswith(
                ("_postgres_bin_dir", "_postgres_stage_root"))
            usable = (
                artifact_path.is_dir()
                if directory_key
                else artifact_path.is_file()
            )
            if not usable:
                expected_kind = (
                    "directory" if directory_key
                    else "file"
                )
                raise RuntimeError(
                    f"native cache artifact {key!r} is not a usable "
                    f"{expected_kind}: {artifact_path}")
            resolved_arguments.append(value)
            continue
        resolved_arguments.append(argument)

    full_command = [
        str(active_bin_dir / command[0]),
        *resolved_arguments,
    ]
    if MODES[mode].is_bench_mode and bench_flags:
        full_command += list(bench_flags)
    elif mode == "unit+integration" and test_flags:
        # Every qualification executable receives the same Catch2 test-spec
        # and allow-no-tests policy. A focused tag may live in only one binary.
        full_command += list(test_flags)
    return full_command


_POSTGRES_ERROR_SAFETY_BOOLEAN_CACHE_KEYS = frozenset({
    "BIOSEQ_BUILD_TESTS",
    "BIOSEQ_TEST_INDEXES",
    "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS",
    "BIOSEQ_BUILD_WEBSITE",
})
_POSTGRES_ERROR_SAFETY_PATH_CACHE_KEYS = frozenset({
    "BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE",
    POSTGRES_ERROR_SAFETY_PEER_BUILD_DIR_CACHE_KEY,
})


def _postgres_error_safety_cache_contract(
        preset: str,
        *,
        environment: Mapping[str, str] | None = None) -> dict[str, str]:
    common = {
        "BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE":
            str(POSTGRES_ERROR_SAFETY_CASE_TEMPLATE),
        "BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE": "isolated",
        "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
        "BIOSEQ_BUILD_WEBSITE": "OFF",
    }
    common.update(_cmake_preset_cache_contract(
        preset,
        ("PostgreSQL_CONFIG",),
        environment=environment,
    ))
    if preset == POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET:
        return {
            "BIOSEQ_BUILD_TESTS": "ON",
            "BIOSEQ_TEST_INDEXES": "ON",
            **common,
        }
    if preset == POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET:
        return {
            "BIOSEQ_BUILD_TESTS": "OFF",
            "BIOSEQ_TEST_INDEXES": "OFF",
            **common,
            POSTGRES_ERROR_SAFETY_PEER_BUILD_DIR_CACHE_KEY: str(
                PROJECT_ROOT
                / "build"
                / POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET),
        }
    raise ValueError(
        f"unsupported PostgreSQL error-safety preset {preset!r}")


def _postgres_error_safety_build_sequence(
        build_only: bool) -> tuple[tuple[str, str], ...]:
    sequence = [
        (
            POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET,
            POSTGRES_ERROR_SAFETY_RUNTIME_PROFILES_TARGET,
        ),
        (
            POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
            POSTGRES_ERROR_SAFETY_RUNTIME_PROFILES_TARGET,
        ),
    ]
    if not build_only:
        sequence.extend((
            (
                POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
                POSTGRES_ERROR_SAFETY_CANDIDATE_TARGET,
            ),
            (
                POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
                POSTGRES_ERROR_SAFETY_EVIDENCE_TARGET,
            ),
        ))
    return tuple(sequence)


def _postgres_error_safety_cache_matches(
        build_dir: Path, key: str, expected: str) -> bool:
    if key in _POSTGRES_ERROR_SAFETY_BOOLEAN_CACHE_KEYS:
        expected_bool = expected == "ON"
        return _cmake_cache_bool(build_dir, key) is expected_bool

    cached = _cmake_cache_value(build_dir, key)
    if cached is None:
        return False
    if key in _POSTGRES_ERROR_SAFETY_PATH_CACHE_KEYS:
        try:
            return (
                Path(cached).resolve(strict=False)
                == Path(expected).resolve(strict=False)
            )
        except (OSError, RuntimeError):
            return False
    return cached == expected


def _checked_postgres_error_safety_case_template() -> Path:
    template = POSTGRES_ERROR_SAFETY_CASE_TEMPLATE
    try:
        metadata = template.lstat()
    except OSError as exc:
        raise RuntimeError(
            "checked PostgreSQL error-safety runtime case template is "
            f"missing: {template}") from exc
    if template.is_symlink() or not stat.S_ISREG(metadata.st_mode):
        raise RuntimeError(
            "checked PostgreSQL error-safety runtime case template must be "
            f"a regular non-symlink file: {template}")
    return template.resolve(strict=True)


def _configure_postgres_error_safety_tree(
        preset: str, build_dir: Path, log_handle, deadline: float,
        env: dict[str, str], rt_priority: int | None) -> None:
    expected_cache = _postgres_error_safety_cache_contract(
        preset,
        environment=env,
    )
    initial_clang_rt_args = _initial_clang_rt_cmake_args(
        preset,
        rt_priority,
    )
    cmake_identity = _cmake_executable_identity(env)
    configure_stamp = _cmake_configure_stamp_document(
        preset,
        mode_cache_variables=expected_cache,
        managed_arguments=initial_clang_rt_args,
        environment=env,
        cmake_executable_identity=cmake_identity,
    )
    build_ninja = build_dir / "build.ninja"
    configured_fresh = _ensure_cmake_preset_tree_fresh(
        build_dir=build_dir,
        preset=preset,
        configure_stamp=configure_stamp,
        configure_arguments=[
            "cmake",
            "--preset",
            preset,
            *(f"-D{key}={value}"
              for key, value in expected_cache.items()),
            *initial_clang_rt_args,
        ],
        log_handle=log_handle,
        deadline=deadline,
        environment=env,
        failure_label=f"configure of preset {preset!r}",
    )

    repairs = {
        key: expected
        for key, expected in expected_cache.items()
        if not _postgres_error_safety_cache_matches(
            build_dir, key, expected)
    }
    postgresql_reset_args = _postgresql_discovery_cache_reset_args(
        build_dir,
        preset,
        environment=env,
    )
    clang_rt_repairs, clang_rt_reasons = _clang_rt_cmake_cache_repairs(
        build_dir, preset, rt_priority)
    if (
        repairs
        or postgresql_reset_args
        or clang_rt_repairs
    ):
        if repairs:
            log_handle.write(
                "# PostgreSQL error-safety cache repair: "
                + ", ".join(
                    f"{key}={value}" for key, value in repairs.items())
                + ".\n")
        if clang_rt_reasons:
            log_handle.write(
                "# clang RT cache repair: "
                + "; ".join(clang_rt_reasons)
                + ".\n")
        log_handle.flush()
        configure_cache = dict(repairs)
        if postgresql_reset_args:
            configure_cache["PostgreSQL_CONFIG"] = expected_cache[
                "PostgreSQL_CONFIG"
            ]
        rc = _run_cmake_preset_configure(
            [
                "cmake",
                "--preset",
                preset,
                *postgresql_reset_args,
                *(f"-D{key}={value}"
                  for key, value in configure_cache.items()),
                *initial_clang_rt_args,
                *clang_rt_repairs,
            ],
            build_dir=build_dir,
            log_handle=log_handle,
            deadline=deadline,
            environment=env,
        )
        if rc != 0:
            raise RuntimeError(
                f"configure of preset {preset!r} exited with code {rc}")
    elif not configured_fresh:
        log_handle.write(
            f"# build tree already configured ({build_ninja}); all "
            "PostgreSQL error-safety cache invariants are correct.\n")
        log_handle.flush()

    if not build_ninja.is_file():
        raise RuntimeError(
            f"preset {preset!r} did not produce {build_ninja}")
    failed_invariants = [
        f"{key}={expected}"
        for key, expected in expected_cache.items()
        if not _postgres_error_safety_cache_matches(
            build_dir, key, expected)
    ]
    if failed_invariants:
        raise RuntimeError(
            "PostgreSQL error-safety cache invariant failed closed for "
            f"preset {preset!r}: {', '.join(failed_invariants)}")
    final_configure_stamp = _cmake_configure_stamp_document(
        preset,
        mode_cache_variables=expected_cache,
        managed_arguments=initial_clang_rt_args,
        environment=env,
        cmake_executable_identity=_cmake_executable_identity(env),
    )
    configure_seal = _finalize_cmake_configure_stamp(
        build_dir,
        configure_stamp,
        final_configure_stamp,
        context=f"PostgreSQL error-safety preset {preset!r}",
    )
    log_handle.write(
        "# verified CMake configure input seal="
        f"{configure_seal}.\n")
    log_handle.flush()


def _resolve_postgres_error_safety_dso(build_dir: Path) -> Path:
    artifact_dir = build_dir / "bioseq"
    candidates = tuple(
        artifact_dir / name
        for name in ("bioseq.dylib", "bioseq.so", "bioseq.dll")
    )
    symlinks = [path for path in candidates if path.is_symlink()]
    matches = [
        path.resolve(strict=True)
        for path in candidates
        if path.is_file() and not path.is_symlink()
    ]
    if symlinks:
        raise RuntimeError(
            "PostgreSQL error-safety DSO candidates must not be symlinks: "
            + ", ".join(str(path) for path in symlinks))
    if len(matches) != 1:
        raise RuntimeError(
            "expected exactly one PostgreSQL error-safety DSO in "
            f"{artifact_dir}; found {len(matches)} among "
            + ", ".join(path.name for path in candidates))
    return matches[0]


def _checked_postgres_error_safety_review_waiver_snapshot(
        entry: dict, build_only: bool,
        review_waiver_json: str | None) -> dict[str, object] | None:
    if build_only:
        if review_waiver_json:
            raise RuntimeError(
                "postgres-error-safety build_only does not accept a review "
                "waiver")
        if entry.get(
                "postgres_error_safety_review_waiver_sha256") is not None:
            raise RuntimeError(
                "postgres-error-safety build_only state unexpectedly contains "
                "review-waiver authority")
        return None
    if not review_waiver_json:
        raise RuntimeError(
            "full postgres-error-safety qualification requires the explicit "
            "postgres_error_safety_review_waiver input")
    expected_sha256 = entry.get(
        "postgres_error_safety_review_waiver_sha256")
    actual_sha256 = _sha256_bytes(review_waiver_json.encode("utf-8"))
    if expected_sha256 != actual_sha256:
        raise RuntimeError(
            "PostgreSQL error-safety review-waiver snapshot does not match "
            "the scheduling state")
    try:
        value = json.loads(review_waiver_json)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"invalid immutable review-waiver snapshot: {exc}") from exc
    try:
        normalized = _validate_safety_waiver_snapshot(
            value,
            WORKSPACE_ROOT,
        )
    except PostgresErrorSafetyPromotionError as exc:
        raise RuntimeError(
            f"invalid immutable review-waiver snapshot: {exc}") from exc
    canonical = json.dumps(
        normalized,
        sort_keys=True,
        separators=(",", ":"),
    )
    if canonical != review_waiver_json:
        raise RuntimeError(
            "PostgreSQL error-safety review-waiver snapshot is not canonical")
    return normalized


def _run_postgres_error_safety_worker(
        entry: dict, build_only: bool, debug: bool,
        bench_flags: list[str], test_flags: list[str],
        bench_e2e_config: str | None,
        review_waiver_json: str | None) -> int:
    mode = POSTGRES_ERROR_SAFETY_MODE
    started_at = float(entry["started_at"])
    deadline = started_at + HARD_TIMEOUT_SEC
    log_path = Path(entry["log_file"])
    production_build_dir = (
        PROJECT_ROOT / "build" / POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET)
    test_indexes_build_dir = (
        PROJECT_ROOT / "build" / POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET)
    qualification_report: Path | None = None

    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_handle = log_path.open("w", buffering=1)
    log_handle.write(
        f"# bioseq build worker — mode={mode} pid={os.getpid()} "
        f"started={time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(started_at))}\n"
        f"# project_root={PROJECT_ROOT}\n"
        f"# production_preset={POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET}\n"
        f"# production_build_dir={production_build_dir}\n"
        f"# test_indexes_preset={POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET}\n"
        f"# test_indexes_build_dir={test_indexes_build_dir}\n"
        f"# build_only={build_only}\n"
        "# postgres_error_safety_review_waiver_snapshot="
        f"{_sha256_bytes((review_waiver_json or '').encode('utf-8'))}\n"
        f"# hard_timeout_min={HARD_TIMEOUT_MIN}\n"
    )
    log_handle.flush()

    try:
        if debug:
            raise RuntimeError(
                "postgres-error-safety does not support debug build input")
        if bench_flags or test_flags:
            raise RuntimeError(
                "postgres-error-safety does not support filter or CLI inputs")
        if bench_e2e_config is not None:
            raise RuntimeError(
                "postgres-error-safety does not support bench_e2e_config")
        review_waiver = (
            _checked_postgres_error_safety_review_waiver_snapshot(
                entry,
                build_only,
                review_waiver_json,
            )
        )

        case_template = _checked_postgres_error_safety_case_template()
        env = _build_worker_descendant_environment()
        env.pop("BIOSEQ_E2E_CONFIG_OVERRIDE", None)
        env["CMAKE_EXPORT_COMPILE_COMMANDS"] = "1"
        _ensure_local_toolchain_environment(env, test_indexes_build_dir)

        machine_rt_priority = _configured_local_build_rt_priority()
        effective_rt_priority = (
            machine_rt_priority
            if _local_clang_rt_is_enabled(
                POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
                machine_rt_priority)
            else None
        )
        if entry.get("rt_priority") != effective_rt_priority:
            raise RuntimeError(
                "clang realtime machine policy changed between scheduling "
                "and worker start "
                f"(scheduled={entry.get('rt_priority')}, "
                f"current={effective_rt_priority}); refusing to build")

        _update_entry(
            mode,
            step="configuring test-index profile",
            build_only=build_only,
            debug=False,
            preset=POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
            build_dir=str(production_build_dir),
            build_dirs=[
                str(production_build_dir),
                str(test_indexes_build_dir),
            ],
            secondary_preset=POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET,
            secondary_build_dir=str(test_indexes_build_dir),
            runtime_case_template=str(case_template),
            uses_private_cluster=True,
        )

        build_sequence = _postgres_error_safety_build_sequence(build_only)
        profile_dsos: dict[str, Path] = {}
        profile_trees = (
            (
                "test_indexes",
                build_sequence[0][0],
                test_indexes_build_dir,
                build_sequence[0][1],
            ),
            (
                "production",
                build_sequence[1][0],
                production_build_dir,
                build_sequence[1][1],
            ),
        )
        for profile, preset, build_dir, build_target in profile_trees:
            _update_entry(mode, step=f"configuring {profile} profile")
            _configure_postgres_error_safety_tree(
                preset,
                build_dir,
                log_handle,
                deadline,
                env,
                machine_rt_priority,
            )
            _update_entry(
                mode,
                step=f"building sealed {profile} profile",
            )
            rc = _stream_subprocess(
                [
                    "cmake",
                    "--build",
                    str(build_dir),
                    "--target",
                    build_target,
                    "--parallel",
                    str(CMAKE_PARALLEL_JOBS),
                ],
                log_handle,
                deadline,
                cwd=PROJECT_ROOT,
                env=env,
            )
            if rc != 0:
                raise RuntimeError(
                    "build of target "
                    f"{build_target!r} "
                    f"for {profile} exited with code {rc}")
            profile_dsos[profile] = _resolve_postgres_error_safety_dso(
                build_dir)
            _update_entry(
                mode,
                **{f"{profile}_dso": str(profile_dsos[profile])},
            )

        if not build_only:
            candidate_target = build_sequence[2][1]
            _update_entry(
                mode,
                step=(
                    "generating private PostgreSQL error-safety evidence "
                    f"candidate with target {candidate_target}"),
            )
            rc = _stream_subprocess(
                [
                    "cmake",
                    "--build",
                    str(production_build_dir),
                    "--target",
                    candidate_target,
                    "--parallel",
                    str(CMAKE_PARALLEL_JOBS),
                ],
                log_handle,
                deadline,
                cwd=PROJECT_ROOT,
                env=env,
            )
            if rc != 0:
                raise RuntimeError(
                    "private-cluster candidate target "
                    f"{candidate_target!r} exited with code {rc}")
            if review_waiver is None:
                raise RuntimeError(
                    "full PostgreSQL error-safety qualification lost its "
                    "validated review-waiver snapshot")
            candidate_root = (
                production_build_dir
                / POSTGRES_ERROR_SAFETY_CANDIDATE_RELATIVE_ROOT
            )
            _update_entry(
                mode,
                step=(
                    "promoting byte-identical PostgreSQL error-safety "
                    "evidence under explicit user review waiver"),
            )
            try:
                promotion_receipt, promotion = (
                    _promote_safety_reviewed_evidence(
                        candidate_root,
                        review_waiver,
                        WORKSPACE_ROOT,
                    )
                )
            except PostgresErrorSafetyPromotionError as exc:
                raise RuntimeError(
                    "PostgreSQL error-safety candidate promotion failed "
                    f"closed: {exc}") from exc
            log_handle.write(
                "# promoted PostgreSQL error-safety candidate as an atomic "
                "byte copy under explicit user waiver\n"
                f"# promotion_receipt={promotion_receipt}\n"
                "# independent_review_performed=false\n"
            )
            log_handle.flush()
            _update_entry(
                mode,
                postgres_error_safety_candidate_root=str(candidate_root),
                postgres_error_safety_promotion_receipt=str(
                    promotion_receipt),
                postgres_error_safety_independent_review_performed=(
                    promotion["independent_review_performed"]),
            )

            evidence_target = build_sequence[3][1]
            _update_entry(
                mode,
                step=(
                    "running private PostgreSQL error-safety evidence "
                    f"target {evidence_target}"),
            )
            rc = _stream_subprocess(
                [
                    "cmake",
                    "--build",
                    str(production_build_dir),
                    "--target",
                    evidence_target,
                    "--parallel",
                    str(CMAKE_PARALLEL_JOBS),
                ],
                log_handle,
                deadline,
                cwd=PROJECT_ROOT,
                env=env,
            )
            if rc != 0:
                raise RuntimeError(
                    "private-cluster evidence target "
                    f"{evidence_target!r} exited "
                    f"with code {rc}")
            qualification_report = (
                candidate_root / "evidence-validation.json")
            try:
                report_metadata = qualification_report.lstat()
            except OSError as exc:
                raise RuntimeError(
                    "PostgreSQL error-safety evidence target published no "
                    f"validation report: {qualification_report}") from exc
            if (
                qualification_report.is_symlink()
                or not stat.S_ISREG(report_metadata.st_mode)
                or report_metadata.st_nlink != 1
            ):
                raise RuntimeError(
                    "PostgreSQL error-safety validation report must be a "
                    "regular single-link non-symlink file: "
                    f"{qualification_report}")

        completed_at = time.time()
        _update_entry(
            mode,
            status="completed",
            step="completed",
            completed_at=completed_at,
            exit_code=0,
            report_file=(
                str(qualification_report)
                if qualification_report is not None else None
            ),
            report_files=(
                [str(qualification_report)]
                if qualification_report is not None else []
            ),
            build_only=build_only,
            debug=False,
        )
        log_handle.write(
            "\n# worker exiting cleanly at "
            f"{time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(completed_at))}\n"
        )
        return 0
    except TimeoutError as exc:
        completed_at = time.time()
        _update_entry(
            mode,
            status="killed_timeout",
            step="killed_timeout",
            completed_at=completed_at,
            exit_code=None,
            notes=str(exc),
            build_only=build_only,
            debug=False,
        )
        log_handle.write(
            f"\n# killed by {HARD_TIMEOUT_MIN}-min hard timeout: {exc}\n")
        return 124
    except Exception as exc:
        completed_at = time.time()
        _update_entry(
            mode,
            status="failed",
            step="failed",
            completed_at=completed_at,
            exit_code=None,
            notes=str(exc),
            build_only=build_only,
            debug=False,
        )
        log_handle.write(f"\n# worker failed: {exc}\n")
        return 1
    finally:
        log_handle.close()


def _run_build_worker(mode: str, build_only: bool = False,
                      debug: bool = False, bench_flags=None,
                      test_flags=None, bench_e2e_config=None,
                      sql_guardrail_request_json=None,
                      postgres_error_safety_review_waiver_json=None) -> int:
    """Worker entry point — runs detached from the MCP server.

    Reads the slot the server reserved in state, walks configure → build →
    run, updating `step`/`status` as it goes. The hard timeout is enforced
    by `_stream_subprocess` against `started_at + HARD_TIMEOUT_SEC`.

    Unit/integration mode resolves to the isolated test preset family; benchmark
    modes resolve to the production preset family; a mode with `preset_override`
    (e.g. external-api's host-valid test/toolchain tree) pins its own preset
    outright. `debug=True` selects that family's debug/inspect tree with per-TU
    `.s`/`.bc` and DWARF artefacts; `debug=False` selects its lean everyday tree.

    `bench_flags` is a list of extra argv tokens (e.g. ['--filter','tags=gpu',
    '--debug']) appended only to bench-mode commands (§5.4). `test_flags` is a
    validated Catch2 argument list appended identically to every executable in
    the unit+integration qualification for a focused test run.

    `bench_e2e_config` is an absolute, server-validated path inside this
    workspace. Ordinary configurations are exposed to the bench-e2e
    executable. DOC0388 workflow requests arrive separately as one immutable
    canonical JSON snapshot so the detached worker never rereads mutable
    request intent.
    """
    identity_error = _refuse_root_build_identity("detached build worker")
    if identity_error is not None:
        _log(identity_error)
        return 1
    try:
        _ensure_build_worker_timesharing_policy()
    except RuntimeError as exc:
        _log(f"detached build worker scheduling error: {exc}")
        return 1
    bench_flags = bench_flags or []
    test_flags = test_flags or []
    state = _load_state()
    entry = state.get(mode)
    if not entry:
        _log(f"worker for {mode}: no state entry, exiting")
        return 1
    if mode == POSTGRES_ERROR_SAFETY_MODE:
        return _run_postgres_error_safety_worker(
            entry,
            build_only,
            debug,
            bench_flags,
            test_flags,
            bench_e2e_config,
            postgres_error_safety_review_waiver_json,
        )
    if postgres_error_safety_review_waiver_json:
        _log(
            "review-waiver snapshot was supplied outside "
            "postgres-error-safety mode")
        return 1

    mode_details = MODES[mode]
    started_at = float(entry["started_at"])
    log_path = Path(entry["log_file"])
    mode_timeout_sec = (
        HARD_TIMEOUT_SEC
        if mode_details.hard_timeout_min is None
        else min(
            HARD_TIMEOUT_SEC,
            mode_details.hard_timeout_min * 60.0,
        )
    )
    deadline = started_at + mode_timeout_sec

    active_preset = preset_for(mode, debug)
    active_build_dir = build_dir_for(mode, debug)
    active_bin_dir = bin_dir_for(mode, debug)

    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_handle = log_path.open("w", buffering=1)
    log_handle.write(
        f"# bioseq build worker — mode={mode} pid={os.getpid()} "
        f"started={time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(started_at))}\n"
        f"# project_root={PROJECT_ROOT}\n"
        f"# build_dir={active_build_dir}\n"
        f"# preset={active_preset}\n"
        f"# debug={debug}\n"
        f"# bench_flags={json.dumps(bench_flags, ensure_ascii=True)}\n"
        f"# focused_test_args={json.dumps(test_flags, ensure_ascii=True)}\n"
        f"# bench_e2e_config={bench_e2e_config or ''}\n"
        f"# sql_guardrail_request_snapshot="
        f"{_sha256_bytes((sql_guardrail_request_json or '').encode())}\n"
        f"# postgres_error_safety_review_waiver_snapshot="
        f"{_sha256_bytes((postgres_error_safety_review_waiver_json or '').encode())}\n"
        f"# clang_rt_priority={entry.get('rt_priority')}\n"
        f"# hard_timeout_min={mode_timeout_sec / 60.0}\n"
    )
    log_handle.flush()

    if mode_details.uses_private_cluster and mode_details.install_extension:
        raise AssertionError(
            "private-cluster mode reached shared extension install routing")

    pre_reports: set[Path] = set()
    if (mode_details.reporting_subdir_name
            and mode_details.reporting_dir.exists()):
        pre_reports = _report_paths(mode_details.reporting_dir)

    try:
        # Bind qualification to the source that enters configure/build, then
        # compare again after every native command. Development runs in
        # mapped/mixed phases remain usable from a dirty tree.
        cache_provenance_before = _cache_qualification_provenance(
            mode, build_only)
        sql_guardrail_workflow = None
        if sql_guardrail_request_json:
            try:
                sql_guardrail_workflow = json.loads(
                    sql_guardrail_request_json)
            except json.JSONDecodeError as exc:
                raise RuntimeError(
                    f"invalid immutable SQL guardrail request snapshot: {exc}"
                ) from exc
            if (
                not isinstance(sql_guardrail_workflow, dict)
                or sql_guardrail_workflow.get("artifact_kind")
                    != _SQL_GUARDRAIL_WORKFLOW_KIND
                or sql_guardrail_workflow.get("phase")
                    not in _SQL_GUARDRAIL_WORKFLOW_PHASES
            ):
                raise RuntimeError(
                    "immutable SQL guardrail request snapshot is malformed")
        env = _build_worker_descendant_environment()
        # Never let ambient MCP-server state select an E2E configuration. The
        # explicit start_build argument below is the sole supported override.
        env.pop("BIOSEQ_E2E_CONFIG_OVERRIDE", None)
        env["CMAKE_EXPORT_COMPILE_COMMANDS"] = "1"
        if (
            sql_guardrail_workflow is not None
            and sql_guardrail_workflow["phase"] == "baseline_promotion"
        ):
            report_path, workflow_evidence = _run_sql_guardrail_promotion(
                request=sql_guardrail_workflow,
                log_handle=log_handle,
                deadline=deadline,
            )
            completed_at = time.time()
            _update_entry(
                mode,
                status="completed",
                step="completed",
                completed_at=completed_at,
                exit_code=0,
                report_file=None,
                report_files=[],
                sql_guardrail_workflow=workflow_evidence,
                build_only=False,
                debug=debug,
            )
            return 0
        # The local MCP may itself run on the provisioned Linux build host.
        # Keep the provisioned environment when present, but also recover
        # VCPKG_ROOT from an existing configured tree for a fresh sibling
        # test/debug tree.
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

        machine_rt_priority = _configured_local_build_rt_priority()
        effective_rt_priority = (
            machine_rt_priority
            if _local_clang_rt_is_enabled(
                active_preset, machine_rt_priority)
            else None
        )
        if entry.get("rt_priority") != effective_rt_priority:
            raise RuntimeError(
                "clang realtime machine policy changed between scheduling "
                f"and worker start (scheduled={entry.get('rt_priority')}, "
                f"current={effective_rt_priority}); refusing to build")
        _preflight_local_clang_rt_launcher(
            active_preset, machine_rt_priority)
        _update_entry(mode, step="configuring", build_only=build_only,
                      debug=debug, preset=active_preset,
                      build_dir=str(active_build_dir),
                      preset_family=mode_details.preset_family,
                      binary_subdir=mode_details.binary_subdir,
                      uses_private_cluster=mode_details.uses_private_cluster)
        # Configure an existing C++-modules tree only when its sealed effective
        # preset inputs changed. CMakePresets.json is not part of Ninja's
        # RERUN_CMAKE dependency graph, so relying on build.ninja alone can
        # silently retain stale include/toolchain cache values. Conversely,
        # configuring every run regenerates all module dyndep/modmap edges.
        required_cache = mode_details.required_cache_variables or {}
        preset_cache_contract = _cmake_preset_cache_contract(
            active_preset,
            ("PostgreSQL_CONFIG",),
            environment=env,
        )
        configure_cache_contract = {
            **required_cache,
            **preset_cache_contract,
        }
        required_cache_args = [
            f"-D{name}={value}"
            for name, value in configure_cache_contract.items()
        ]
        initial_clang_rt_args = _initial_clang_rt_cmake_args(
            active_preset, machine_rt_priority)
        cmake_identity = _cmake_executable_identity(env)
        configure_stamp = _cmake_configure_stamp_document(
            active_preset,
            mode_cache_variables=configure_cache_contract,
            managed_arguments=initial_clang_rt_args,
            environment=env,
            cmake_executable_identity=cmake_identity,
        )
        build_ninja = active_build_dir / "build.ninja"
        configured_fresh = _ensure_cmake_preset_tree_fresh(
            build_dir=active_build_dir,
            preset=active_preset,
            configure_stamp=configure_stamp,
            configure_arguments=[
                "cmake",
                "--preset",
                active_preset,
                *required_cache_args,
                *initial_clang_rt_args,
            ],
            log_handle=log_handle,
            deadline=deadline,
            environment=env,
        )
        expected_test_indexes = mode_details.preset_family == "test"
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
            rc = _run_cmake_preset_configure(
                ["cmake", "--preset", active_preset,
                 *required_cache_args, *initial_clang_rt_args],
                build_dir=active_build_dir,
                log_handle=log_handle,
                deadline=deadline,
                environment=env,
            )
            if rc != 0:
                raise RuntimeError(
                    "CMake hook-isolation repair exited with code "
                    f"{rc}")
        else:
            if not configured_fresh:
                log_handle.write(
                    f"# build tree already configured ({build_ninja}); "
                    "sealed configure inputs and BIOSEQ_TEST_INDEXES "
                    "invariant are correct; skipping redundant "
                    "`cmake --preset`.\n")
                log_handle.flush()

        cache_repairs = {
            name: value
            for name, value in required_cache.items()
            if _cmake_cache_value(active_build_dir, name) != value
        }
        cache_repairs.update(_cmake_preset_cache_repairs(
            active_build_dir,
            active_preset,
            environment=env,
        ))
        postgresql_reset_args = _postgresql_discovery_cache_reset_args(
            active_build_dir,
            active_preset,
            environment=env,
        )
        if postgresql_reset_args:
            cache_repairs.update(preset_cache_contract)
        if cache_repairs or postgresql_reset_args:
            repair_description = ", ".join(
                f"{name}={value}" for name, value in cache_repairs.items())
            log_handle.write(
                "# mode cache repair: "
                + (
                    "resetting derived PostgreSQL discovery; "
                    if postgresql_reset_args
                    else ""
                )
                + f"setting {repair_description}.\n")
            log_handle.flush()
            _invalidate_cmake_configure_stamp(active_build_dir)
            rc = _run_cmake_preset_configure(
                ["cmake", "--preset", active_preset,
                 *postgresql_reset_args,
                 *(f"-D{name}={value}" for name, value in cache_repairs.items())],
                build_dir=active_build_dir,
                log_handle=log_handle,
                deadline=deadline,
                environment=env,
            )
            if rc != 0:
                raise RuntimeError(
                    f"mode cache repair exited with code {rc}")

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
            desired_heavy_jobs = (
                AWS_DEBUG_HEAVY_JOBS
                if _is_debug_preset(active_preset)
                else AWS_HEAVY_JOBS
            )
            current_heavy_jobs = _cmake_cache_value(active_build_dir, "BIOSEQ_HEAVY_JOBS")
            repair_args = [
                f"-DBIOSEQ_HEAVY_JOBS={desired_heavy_jobs}",
            ]
            repair_unset_args = []
            repair_reasons = []
            if current_heavy_jobs != desired_heavy_jobs:
                repair_reasons.append(
                    f"BIOSEQ_HEAVY_JOBS={desired_heavy_jobs} "
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
                _invalidate_cmake_configure_stamp(active_build_dir)
                rc = _run_cmake_preset_configure(
                    ["cmake", "--preset", active_preset,
                     *dict.fromkeys(repair_unset_args), *repair_args],
                    build_dir=active_build_dir,
                    log_handle=log_handle,
                    deadline=deadline,
                    environment=env,
                )
                if rc != 0:
                    raise RuntimeError(
                        f"AWS cache repair exited with code {rc}")

        # Run this after every cache-clearing/toolchain repair above so the
        # final generated compile rules cannot lose (or retain, when disabled)
        # our exact managed launcher value.
        clang_rt_repairs, clang_rt_reasons = (
            _clang_rt_cmake_cache_repairs(
                active_build_dir, active_preset, machine_rt_priority)
        )
        if clang_rt_repairs:
            log_handle.write(
                "# clang RT cache repair: "
                + "; ".join(clang_rt_reasons) + ".\n")
            log_handle.flush()
            _invalidate_cmake_configure_stamp(active_build_dir)
            rc = _run_cmake_preset_configure(
                ["cmake", "--preset", active_preset, *clang_rt_repairs],
                build_dir=active_build_dir,
                log_handle=log_handle,
                deadline=deadline,
                environment=env,
            )
            if rc != 0:
                raise RuntimeError(
                    "clang RT compiler-launcher cache repair exited with "
                    f"code {rc}")

        postgresql_config_repairs = _cmake_preset_cache_repairs(
            active_build_dir,
            active_preset,
            environment=env,
        )
        if postgresql_config_repairs:
            expected = postgresql_config_repairs["PostgreSQL_CONFIG"]
            observed = _cmake_cache_value(
                active_build_dir, "PostgreSQL_CONFIG")
            raise RuntimeError(
                "PostgreSQL_CONFIG cache invariant failed closed: "
                f"preset={active_preset} expected={expected!r} "
                f"observed={observed!r}")

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
        final_configure_stamp = _cmake_configure_stamp_document(
            active_preset,
            mode_cache_variables=configure_cache_contract,
            managed_arguments=initial_clang_rt_args,
            environment=env,
            cmake_executable_identity=_cmake_executable_identity(env),
        )
        configure_seal = _finalize_cmake_configure_stamp(
            active_build_dir,
            configure_stamp,
            final_configure_stamp,
            context=f"preset {active_preset!r}",
        )
        log_handle.write(
            "# verified CMake configure input seal="
            f"{configure_seal}.\n")
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
                report_files=[],
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

            for post_target in post_build_targets_for(mode, build_only):
                _update_entry(
                    mode,
                    step=f"running CMake target {post_target}",
                    uses_private_cluster=mode_details.uses_private_cluster,
                )
                rc = _stream_subprocess(
                    ["cmake", "--build", str(active_build_dir),
                     "--target", post_target,
                     "--parallel", str(CMAKE_PARALLEL_JOBS)],
                    log_handle, deadline, cwd=PROJECT_ROOT, env=env,
                )
                if rc != 0:
                    raise RuntimeError(
                        f"post-build target '{post_target}' exited with code {rc}")

            if sql_guardrail_workflow is not None:
                env.setdefault(
                    "BIOSEQ_E2E_PG_RESTART_CMD",
                    _postgresql_e2e_restart_shell_command(
                        active_build_dir, active_preset),
                )
                report_path, workflow_evidence = (
                    _run_sql_guardrail_workflow(
                        request=sql_guardrail_workflow,
                        active_build_dir=active_build_dir,
                        active_preset=active_preset,
                        env=env,
                        log_handle=log_handle,
                        deadline=deadline,
                    )
                )
                completed_at = time.time()
                _update_entry(
                    mode,
                    status="completed",
                    step="completed",
                    completed_at=completed_at,
                    exit_code=0,
                    report_file=(
                        str(report_path) if report_path else None),
                    report_files=(
                        [str(report_path)] if report_path else []),
                    build_only=False,
                    debug=debug,
                    sql_guardrail_workflow=workflow_evidence,
                )
                log_handle.write(
                    "\n# SQL guardrail workflow exiting cleanly at "
                    f"{time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(completed_at))}\n"
                )
                return 0

            _update_entry(
                mode,
                step="running",
                uses_private_cluster=mode_details.uses_private_cluster,
            )
            if mode == "bench-e2e":
                env.setdefault(
                    "BIOSEQ_E2E_PG_RESTART_CMD",
                    _postgresql_e2e_restart_shell_command(active_build_dir, active_preset),
                )
                log_handle.write(
                    "# BIOSEQ_E2E_PG_RESTART_CMD="
                    f"{env['BIOSEQ_E2E_PG_RESTART_CMD']}\n")
                log_handle.flush()
            run_env = env
            if mode == "bench-e2e" and bench_e2e_config is not None:
                run_env = env.copy()
                run_env["BIOSEQ_E2E_CONFIG_OVERRIDE"] = bench_e2e_config
                log_handle.write(
                    "# BIOSEQ_E2E_CONFIG_OVERRIDE="
                    f"{bench_e2e_config}\n")
                log_handle.flush()
            (
                report_path,
                report_files,
                report_invocations,
                unit_command_records,
            ) = _run_mode_commands(
                mode,
                active_bin_dir,
                active_build_dir,
                run_env,
                log_handle,
                deadline,
                pre_reports,
                bench_flags=bench_flags,
                test_flags=test_flags,
            )
            cache_completion_fields: dict[str, object] = {}
            if cache_provenance_before is not None:
                cache_provenance_after = _cache_run_provenance()
                _require_same_cache_run_provenance(
                    cache_provenance_before,
                    cache_provenance_after,
                )
                _update_entry(
                    mode, step="sealing cache qualification receipts")
                retirement_gate = None
                if (
                    mode == "cache-unit"
                    and cache_provenance_after["migration_phase"]
                        == "retired_with_proof"
                ):
                    # A failed historical-proof/deletion transition must not
                    # leave a discoverable current cache-unit receipt that a
                    # later retired-final run could mistake for authorization.
                    retirement_gate = (
                        _run_cache_retirement_lifecycle_gate(
                            mode,
                            active_bin_dir,
                            active_build_dir,
                            run_env,
                            cache_provenance_after,
                            log_handle,
                            deadline,
                        )
                    )
                unit_receipt, evidence_receipts = (
                    _seal_cache_mode_receipts(
                        mode,
                        active_bin_dir,
                        report_files,
                        report_invocations,
                        unit_command_records,
                        cache_provenance_after,
                    )
                )
                if retirement_gate is None:
                    retirement_gate = (
                        _run_cache_retirement_lifecycle_gate(
                            mode,
                            active_bin_dir,
                            active_build_dir,
                            run_env,
                            cache_provenance_after,
                            log_handle,
                            deadline,
                        )
                    )
                cache_completion_fields = {
                    "cache_run_provenance": cache_provenance_after,
                    "cache_unit_receipt": (
                        str(unit_receipt)
                        if unit_receipt is not None else None
                    ),
                    "cache_evidence_receipts": [
                        str(path) for path in evidence_receipts
                    ],
                    "cache_retirement_gate": retirement_gate,
                }
            completed_at = time.time()
            _update_entry(
                mode,
                status="completed",
                step="completed",
                completed_at=completed_at,
                exit_code=0,
                report_file=str(report_path) if report_path else None,
                report_files=[str(path) for path in report_files],
                build_only=False,
                debug=debug,
                **cache_completion_fields,
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
        log_handle.write(
            f"\n# killed by {mode_timeout_sec / 60.0}-min "
            f"mode hard timeout: {e}\n")
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
        state: dict, mode: str, build_only: bool,
        requested_build_dir: Path) -> tuple[str | None, dict | None]:
    """Find any currently-running run that conflicts with `mode`.

    Returns (running_mode, entry) or (None, None). Conflicts are decided by
    lock groups, every resolved CMake build directory, and the shared
    PostgreSQL install/restart resource. Build-directory comparison uses all
    paths recorded when a worker was reserved; legacy entries fall back to
    their recorded debug selection. An unreadable or malformed running path
    fails closed.
    """
    mode_details = MODES[mode]
    try:
        requested_build_dirs = (
            build_dirs_for(mode, debug=False)
            if mode == POSTGRES_ERROR_SAFETY_MODE
            else (requested_build_dir,)
        )
        resolved_requested_build_dirs = {
            path.resolve(strict=False)
            for path in requested_build_dirs
        }
    except (OSError, RuntimeError, TypeError):
        # If any worker is live below, an indeterminate requested tree must
        # conflict rather than risk racing that worker's configure.
        resolved_requested_build_dirs = None
    for other_mode in VALID_MODES:
        entry = _refresh(state, other_mode)
        if not entry:
            continue
        if entry.get("status") != "running":
            continue
        other_details = MODES[other_mode]
        same_lock_group = other_details.lock_group == mode_details.lock_group
        try:
            if resolved_requested_build_dirs is None:
                same_build_dir = True
            else:
                recorded_build_dirs = entry.get("build_dirs")
                if recorded_build_dirs is None:
                    recorded_build_dir = entry.get("build_dir")
                    if recorded_build_dir is None:
                        recorded_build_dirs = build_dirs_for(
                            other_mode,
                            bool(entry.get("debug", False)),
                        )
                    else:
                        recorded_build_dirs = (recorded_build_dir,)
                if (
                    not isinstance(recorded_build_dirs, (list, tuple))
                    or not recorded_build_dirs
                ):
                    raise TypeError(
                        "running build directories are malformed")
                resolved_other_build_dirs: set[Path] = set()
                for path in recorded_build_dirs:
                    if (
                        not isinstance(path, (str, os.PathLike))
                        or not os.fspath(path)
                    ):
                        raise TypeError(
                            "running build directory is malformed")
                    resolved_other_build_dirs.add(
                        Path(path).resolve(strict=False))
                same_build_dir = bool(
                    resolved_other_build_dirs
                    & resolved_requested_build_dirs)
        except (OSError, RuntimeError, TypeError):
            # A live worker with an indeterminate tree must block scheduling;
            # allowing it through could race a configure in that same tree.
            same_build_dir = True
        shared_postgresql_runtime = (
            not build_only
            and not bool(entry.get("build_only", False))
            and mode_details.install_extension
            and other_details.install_extension
        )
        if same_lock_group or same_build_dir or shared_postgresql_runtime:
            return other_mode, entry
    return None, None


# (object key in the MCP `filter`, bench CLI key). Note plural->singular for
# benchmarks: the MCP surface is `benchmarks` (matching the prompt's
# FilterConditions.benchmarks) but the bench CLI key is the singular `benchmark`.
_FILTER_AXES = (("tags", "tags"), ("benchmarks", "benchmark"), ("cases", "cases"))

# The SQL-codegen guardrail is a workflow-only sixth E2E benchmark.  Its
# calibration and qualification runs receive a nonce and one-use authority
# from a sealed bench-e2e request.  An ordinary unfiltered E2E run retains the
# five pre-existing benchmarks instead of entering that privileged lane
# without its required authority.  Explicit caller filters remain untouched.
_ORDINARY_BENCH_E2E_BENCHMARKS = (
    "bench_e2e",
    "bench_e2e_signoff",
    "bench_e2e_gpu_batch_proof",
    "bench_e2e_routing_poc",
    "bench_sort_scan",
)

# Focused unit/integration runs use Catch2's positional test-spec syntax, not
# the benchmark runner's repeatable ``--filter`` option.  Keep the limits here
# as well as in the MCP JSON schemas because the remote/CLI entry points can be
# called without a schema-validating client.
_MAX_CATCH2_TEST_SPECS = 32
_MAX_CATCH2_TEST_SPEC_LENGTH = 512
_MAX_CATCH2_COMBINED_TEST_SPEC_LENGTH = 4096


def _ordinary_bench_e2e_default_filter(
        mode: str,
        bench_filter,
        *,
        build_only: bool,
        debug_report: bool,
        filter_supplied: bool,
        sql_guardrail_workflow):
    """Keep ordinary E2E runs out of the sealed guardrail-only lane."""
    if (
        mode == "bench-e2e"
        and not build_only
        and not debug_report
        and sql_guardrail_workflow is None
        and not filter_supplied
        and bench_filter is None
    ):
        return {
            "benchmarks": list(_ORDINARY_BENCH_E2E_BENCHMARKS),
        }
    return bench_filter


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


def _unit_test_filter_to_cli_args(test_filter):
    """Translate ``filter.tags`` into one safe Catch2 OR test-spec.

    Catch2 treats commas inside one positional test-spec as OR.  We therefore
    append the same deterministic argument to every unit+integration
    qualification executable. ``--allow-running-no-tests`` is also appended
    because a tag commonly exists in only one binary; absence from the others
    is not a failure of the focused workflow.

    Returns ``(extra_argv, error)``.  An omitted/empty filter deliberately
    returns no argv so the historical full-suite command lines are unchanged.
    """
    if test_filter is None or test_filter == {}:
        return [], None
    if not isinstance(test_filter, dict):
        return [], "filter must be an object"

    unknown = sorted(set(test_filter) - {"tags", "benchmarks", "cases"})
    if unknown:
        return [], "filter contains unknown field(s): " + ", ".join(unknown)
    disallowed = [axis for axis in ("benchmarks", "cases") if axis in test_filter]
    if disallowed:
        return [], (
            "unit+integration accepts only filter.tags Catch2 test-spec "
            "expressions; unsupported field(s): " + ", ".join(disallowed)
        )
    if "tags" not in test_filter:
        return [], None

    values = test_filter["tags"]
    if not isinstance(values, list) or not values:
        return [], "filter.tags must be a non-empty array"
    if len(values) > _MAX_CATCH2_TEST_SPECS:
        return [], (
            f"filter.tags accepts at most {_MAX_CATCH2_TEST_SPECS} Catch2 "
            "test-spec expressions"
        )

    # Stable first-occurrence de-duplication keeps the effective argv compact
    # without reordering user-written Catch2 expressions.
    expressions: list[str] = []
    seen: set[str] = set()
    for index, value in enumerate(values):
        if not isinstance(value, str) or not value:
            return [], "filter.tags entries must be non-empty strings"
        if value != value.strip():
            return [], (
                f"filter.tags[{index}] may not have leading or trailing whitespace"
            )
        if len(value) > _MAX_CATCH2_TEST_SPEC_LENGTH:
            return [], (
                f"filter.tags[{index}] exceeds the "
                f"{_MAX_CATCH2_TEST_SPEC_LENGTH}-character limit"
            )
        if value.startswith("-"):
            return [], (
                f"filter.tags[{index}] may not begin with '-' (Catch2 option "
                "injection is not allowed)"
            )
        if any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in value):
            return [], f"filter.tags[{index}] may not contain control characters"
        if value not in seen:
            seen.add(value)
            expressions.append(value)

    combined = ",".join(expressions)
    if len(combined) > _MAX_CATCH2_COMBINED_TEST_SPEC_LENGTH:
        return [], (
            "combined Catch2 test-spec exceeds the "
            f"{_MAX_CATCH2_COMBINED_TEST_SPEC_LENGTH}-character limit"
        )
    return ["--allow-running-no-tests", combined], None


def _validate_bench_e2e_config_override(mode: str, requested_path):
    """Resolve one explicit local bench-e2e config without path escape.

    The public surface accepts either an absolute path or a workspace-root
    relative path. ``resolve(strict=True)`` follows symlinks before the
    containment check, so a workspace symlink cannot smuggle an outside file
    into the worker. Returning the canonical absolute path also avoids making
    interpretation depend on the detached worker's current directory.
    """
    if requested_path is None:
        return None, None
    if mode != "bench-e2e":
        return None, "bench_e2e_config is accepted only for mode='bench-e2e'"
    if not isinstance(requested_path, str) or not requested_path.strip():
        return None, "bench_e2e_config must be a non-empty path string"
    if requested_path != requested_path.strip():
        return None, "bench_e2e_config may not have surrounding whitespace"
    if any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in requested_path):
        return None, "bench_e2e_config may not contain control characters"

    candidate = Path(requested_path).expanduser()
    if not candidate.is_absolute():
        candidate = WORKSPACE_ROOT / candidate
    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(WORKSPACE_ROOT.resolve(strict=True))
    except (OSError, RuntimeError, ValueError) as exc:
        return None, (
            "bench_e2e_config must resolve to an existing file inside the "
            f"workspace {WORKSPACE_ROOT}: {exc}"
        )
    if not resolved.is_file():
        return None, f"bench_e2e_config is not a regular file: {resolved}"
    if resolved.suffix.lower() != ".json":
        return None, f"bench_e2e_config must name a .json file: {resolved}"
    return str(resolved), None


_SQL_GUARDRAIL_WORKFLOW_KIND = (
    "doc0388_sql_codegen_guardrail_workflow_request"
)
_SQL_GUARDRAIL_WORKFLOW_PHASES = {
    "calibration_capture",
    "baseline_promotion",
    "candidate_qualification",
}


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _sha256_path(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _canonical_json_sha256(value: dict[str, object]) -> str:
    return _sha256_bytes(json.dumps(
        value, sort_keys=True, separators=(",", ":")
    ).encode("utf-8"))


def _validate_json_content_seal(
        value: dict[str, object], context: str) -> str:
    seal = value.get("content_sha256")
    if (
        not isinstance(seal, str)
        or re.fullmatch(r"[0-9a-f]{64}", seal) is None
    ):
        raise ValueError(f"{context} content_sha256 is invalid")
    unsealed = dict(value)
    del unsealed["content_sha256"]
    if _canonical_json_sha256(unsealed) != seal:
        raise ValueError(f"{context} content seal does not match")
    return seal


def _sql_guardrail_baseline_receipt_path(
        baseline: Path) -> Path:
    return baseline.with_name(
        baseline.name + ".mcp-publication.json")


def _load_sql_guardrail_publication_ledger(
        *, required: bool) -> dict[str, object]:
    if not SQL_GUARDRAIL_PUBLICATION_LEDGER.exists():
        if required:
            raise RuntimeError(
                "SQL guardrail MCP publication ledger is missing")
        value: dict[str, object] = {
            "schema_version": 1,
            "artifact_kind":
                "doc0388_sql_guardrail_publication_ledger",
            "publications": {},
        }
        value["content_sha256"] = _canonical_json_sha256(value)
        return value
    try:
        value = json.loads(
            SQL_GUARDRAIL_PUBLICATION_LEDGER.read_text(
                encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(
            "SQL guardrail MCP publication ledger is unreadable") from exc
    if (
        not isinstance(value, dict)
        or set(value) != {
            "schema_version",
            "artifact_kind",
            "publications",
            "content_sha256",
        }
        or value.get("schema_version") != 1
        or value.get("artifact_kind")
            != "doc0388_sql_guardrail_publication_ledger"
        or not isinstance(value.get("publications"), dict)
    ):
        raise RuntimeError(
            "SQL guardrail MCP publication ledger schema is invalid")
    try:
        _validate_json_content_seal(
            value, "SQL guardrail MCP publication ledger")
    except ValueError as exc:
        raise RuntimeError(str(exc)) from exc
    return value


def _save_sql_guardrail_publication_ledger(
        value: dict[str, object]) -> None:
    unsealed = {
        key: item
        for key, item in value.items()
        if key != "content_sha256"
    }
    unsealed["content_sha256"] = _canonical_json_sha256(unsealed)
    temporary = SQL_GUARDRAIL_PUBLICATION_LEDGER.with_name(
        SQL_GUARDRAIL_PUBLICATION_LEDGER.name
        + f".tmp-{os.getpid()}")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(temporary, flags, 0o600)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8",
                       closefd=False) as output:
            json.dump(unsealed, output, indent=2, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
    finally:
        os.close(descriptor)
    os.replace(temporary, SQL_GUARDRAIL_PUBLICATION_LEDGER)


def _publish_sql_guardrail_baseline(
        *,
        baseline: Path,
        baseline_value: dict[str, object],
        request: dict[str, object]) -> tuple[Path, dict[str, object]]:
    """Publish the only server-recognized authority for one baseline."""
    state = _load_state()
    entry = state.get("bench-e2e")
    request_snapshot_sha256 = _canonical_json_sha256(request)
    worker_session_id = os.getsid(0)
    if (
        not isinstance(entry, dict)
        or entry.get("status") != "running"
        or entry.get("pid") != os.getpid()
        or entry.get("started_at") is None
        or entry.get("sql_guardrail_request_sha256")
            != request_snapshot_sha256
        or os.getsid(os.getpid()) != worker_session_id
    ):
        raise RuntimeError(
            "baseline publication is not owned by the exact running MCP "
            "worker/request/session")
    publication_id = secrets.token_hex(32)
    receipt_path = _sql_guardrail_baseline_receipt_path(baseline)
    if receipt_path.exists():
        raise RuntimeError(
            f"baseline publication receipt already exists: {receipt_path}")
    receipt: dict[str, object] = {
        "schema_version": 1,
        "artifact_kind":
            "doc0388_sql_guardrail_baseline_publication",
        "publication_id": publication_id,
        "baseline_path": str(baseline.resolve(strict=True)),
        "baseline_file_sha256": _sha256_path(baseline),
        "baseline_content_sha256":
            baseline_value["content_sha256"],
        "capture_bundle_sha256":
            request["capture_bundle_sha256"],
        "canonical_environment_key":
            baseline_value["canonical_environment_key"],
        "promotion_request_file_sha256":
            request["request_file_sha256"],
        "promotion_request_content_sha256":
            request["request_content_sha256"],
        "promotion_request_snapshot_sha256":
            request_snapshot_sha256,
        "promotion_request": request,
        "worker_pid": os.getpid(),
        "worker_session_id": worker_session_id,
        "worker_started_at": entry["started_at"],
        "published_monotonic_ns": time.monotonic_ns(),
    }
    receipt["content_sha256"] = _canonical_json_sha256(receipt)
    receipt_body = (
        json.dumps(receipt, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    _write_sql_guardrail_evidence_exclusive(
        receipt_path, receipt_body)
    receipt_sha256 = _sha256_path(receipt_path)
    with _global_lock():
        ledger = _load_sql_guardrail_publication_ledger(
            required=False)
        publications = dict(ledger["publications"])
        if publication_id in publications:
            raise RuntimeError(
                "SQL guardrail publication identifier collision")
        publications[publication_id] = {
            "receipt_path": str(receipt_path.resolve(strict=True)),
            "receipt_file_sha256": receipt_sha256,
            "baseline_path": str(baseline.resolve(strict=True)),
            "baseline_file_sha256": _sha256_path(baseline),
            "canonical_environment_key":
                baseline_value["canonical_environment_key"],
            "promotion_request_snapshot_sha256":
                request_snapshot_sha256,
        }
        ledger["publications"] = publications
        _save_sql_guardrail_publication_ledger(ledger)
    return receipt_path, receipt


def _validate_sql_guardrail_baseline_publication(
        *,
        baseline: Path,
        baseline_sha256: str,
        canonical_environment_key: str,
) -> tuple[Path, dict[str, object]]:
    """Require a receipt+ledger join that no direct guardrail CLI emits."""
    receipt_path = _sql_guardrail_baseline_receipt_path(baseline)
    try:
        receipt_body = _read_sql_guardrail_evidence(receipt_path)
        receipt = json.loads(receipt_body)
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(
            "candidate baseline has no valid MCP publication receipt"
        ) from exc
    expected_fields = {
        "schema_version",
        "artifact_kind",
        "publication_id",
        "baseline_path",
        "baseline_file_sha256",
        "baseline_content_sha256",
        "capture_bundle_sha256",
        "canonical_environment_key",
        "promotion_request_file_sha256",
        "promotion_request_content_sha256",
        "promotion_request_snapshot_sha256",
        "promotion_request",
        "worker_pid",
        "worker_session_id",
        "worker_started_at",
        "published_monotonic_ns",
        "content_sha256",
    }
    if (
        not isinstance(receipt, dict)
        or set(receipt) != expected_fields
        or receipt.get("schema_version") != 1
        or receipt.get("artifact_kind")
            != "doc0388_sql_guardrail_baseline_publication"
        or re.fullmatch(
            r"[0-9a-f]{64}", str(receipt.get("publication_id"))
        ) is None
        or receipt.get("baseline_path")
            != str(baseline.resolve(strict=True))
        or receipt.get("baseline_file_sha256") != baseline_sha256
        or receipt.get("canonical_environment_key")
            != canonical_environment_key
        or not isinstance(receipt.get("promotion_request"), dict)
        or _canonical_json_sha256(receipt["promotion_request"])
            != receipt.get("promotion_request_snapshot_sha256")
        or receipt["promotion_request"].get("phase")
            != "baseline_promotion"
    ):
        raise RuntimeError(
            "candidate baseline MCP publication receipt binding is invalid")
    try:
        _validate_json_content_seal(
            receipt, "candidate baseline MCP publication receipt")
    except ValueError as exc:
        raise RuntimeError(str(exc)) from exc
    baseline_value = json.loads(
        _read_sql_guardrail_evidence(
            baseline, expected_sha256=baseline_sha256))
    if (
        not isinstance(baseline_value, dict)
        or baseline_value.get("content_sha256")
            != receipt["baseline_content_sha256"]
    ):
        raise RuntimeError(
            "candidate baseline differs from its MCP publication receipt")
    ledger = _load_sql_guardrail_publication_ledger(required=True)
    expected_record = {
        "receipt_path": str(receipt_path.resolve(strict=True)),
        "receipt_file_sha256": _sha256_path(receipt_path),
        "baseline_path": str(baseline.resolve(strict=True)),
        "baseline_file_sha256": baseline_sha256,
        "canonical_environment_key": canonical_environment_key,
        "promotion_request_snapshot_sha256":
            receipt["promotion_request_snapshot_sha256"],
    }
    if ledger["publications"].get(
            receipt["publication_id"]) != expected_record:
        raise RuntimeError(
            "candidate baseline publication receipt is absent from the "
            "MCP ledger")
    return receipt_path, receipt


def _workspace_input_json(value: object, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{field} must be a non-empty path")
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = WORKSPACE_ROOT / candidate
    resolved = candidate.resolve(strict=True)
    resolved.relative_to(WORKSPACE_ROOT.resolve(strict=True))
    if not resolved.is_file() or resolved.suffix.lower() != ".json":
        raise ValueError(
            f"{field} must be an existing workspace-contained JSON file")
    return str(resolved)


def _workspace_hashed_input_json(
        path_value: object,
        digest_value: object,
        field: str) -> tuple[str, str]:
    path = _workspace_input_json(path_value, field)
    if (
        not isinstance(digest_value, str)
        or re.fullmatch(r"[0-9a-f]{64}", digest_value) is None
    ):
        raise ValueError(f"{field}_sha256 must be a lowercase SHA-256")
    actual = _sha256_path(Path(path))
    if actual != digest_value:
        raise ValueError(
            f"{field}_sha256 does not match {path}: "
            f"{digest_value} != {actual}")
    return path, digest_value


def _workspace_output_json(value: object, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{field} must be a non-empty path")
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = WORKSPACE_ROOT / candidate
    if candidate.suffix.lower() != ".json":
        raise ValueError(f"{field} must name a JSON file")
    ancestor = candidate.parent
    while not ancestor.exists():
        if ancestor == ancestor.parent:
            raise ValueError(f"{field} has no existing parent")
        ancestor = ancestor.parent
    ancestor.resolve(strict=True).relative_to(
        WORKSPACE_ROOT.resolve(strict=True))
    resolved = candidate.resolve(strict=False)
    resolved.relative_to(WORKSPACE_ROOT.resolve(strict=True))
    if resolved.exists():
        raise ValueError(
            f"{field} refuses to overwrite existing evidence: {resolved}")
    return str(resolved)


def _sql_guardrail_workflow_request(
        config_path: str | None) -> dict[str, object] | None:
    """Return one validated DOC0388 workflow request, or ``None``.

    Ordinary benchmark configurations remain opaque to the worker. A file
    opts into orchestration only with the exact artifact kind, after which
    unknown/missing fields fail closed before any build is scheduled.
    """
    if config_path is None:
        return None
    try:
        request_bytes = Path(config_path).read_bytes()
        value = json.loads(request_bytes)
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read bench_e2e_config JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError("bench_e2e_config root must be an object")
    if value.get("artifact_kind") != _SQL_GUARDRAIL_WORKFLOW_KIND:
        return None
    phase = value.get("phase")
    if value.get("schema_version") != 1:
        raise ValueError("SQL guardrail workflow schema_version must be 1")
    if phase not in _SQL_GUARDRAIL_WORKFLOW_PHASES:
        raise ValueError("SQL guardrail workflow phase is invalid")
    _validate_json_content_seal(value, "SQL guardrail workflow request")
    common = {
        "schema_version", "artifact_kind", "phase", "content_sha256"
    }
    result: dict[str, object] = {
        "schema_version": 1,
        "artifact_kind": _SQL_GUARDRAIL_WORKFLOW_KIND,
        "phase": phase,
        "request_path": str(Path(config_path).resolve(strict=True)),
        "request_file_sha256": _sha256_bytes(request_bytes),
        "request_content_sha256": value["content_sha256"],
    }
    if phase == "calibration_capture":
        allowed = common | {"capture_bundle_output"}
        if set(value) != allowed:
            raise ValueError(
                "calibration_capture request fields must be exactly: "
                + ", ".join(sorted(allowed)))
        result["capture_bundle_output"] = _workspace_output_json(
            value.get("capture_bundle_output"),
            "capture_bundle_output",
        )
        return result

    if phase == "candidate_qualification":
        allowed = common | {
            "baseline",
            "baseline_sha256",
            "canonical_environment_key",
            "qualification_output",
        }
        if set(value) != allowed:
            raise ValueError(
                "candidate_qualification request fields must be exactly: "
                + ", ".join(sorted(allowed)))
        baseline, baseline_sha = _workspace_hashed_input_json(
            value.get("baseline"),
            value.get("baseline_sha256"),
            "baseline",
        )
        environment_key = value.get("canonical_environment_key")
        if (
            not isinstance(environment_key, str)
            or re.fullmatch(r"[0-9a-f]{64}", environment_key) is None
        ):
            raise ValueError(
                "canonical_environment_key must be a lowercase SHA-256")
        try:
            publication_path, publication = (
                _validate_sql_guardrail_baseline_publication(
                    baseline=Path(baseline),
                    baseline_sha256=baseline_sha,
                    canonical_environment_key=environment_key,
                )
            )
        except RuntimeError as exc:
            raise ValueError(str(exc)) from exc
        result.update({
            "baseline": baseline,
            "baseline_sha256": baseline_sha,
            "canonical_environment_key": environment_key,
            "baseline_publication_receipt":
                str(publication_path),
            "baseline_publication_receipt_sha256":
                _sha256_path(publication_path),
            "baseline_publication_id":
                publication["publication_id"],
            "qualification_output": _workspace_output_json(
                value.get("qualification_output"),
                "qualification_output",
            ),
        })
        return result

    allowed = common | {
        "capture_bundle",
        "capture_bundle_sha256",
        "limits",
        "limits_sha256",
        "baseline_output",
        "baseline_kind",
        "reviewer",
        "evidence_reference",
        "spec_deviation",
        "approval_reference",
    }
    if set(value) != allowed:
        raise ValueError(
            f"{phase} request fields must be exactly: "
            + ", ".join(sorted(allowed)))
    if value.get("baseline_kind") != "recovered_post_cutover":
        raise ValueError(
            "baseline_promotion accepts only recovered_post_cutover; "
            "the required pre-cutover Stage-0 evidence is absent")
    for field in (
        "reviewer",
        "evidence_reference",
        "spec_deviation",
        "approval_reference",
    ):
        text = value.get(field)
        if (
            not isinstance(text, str)
            or not text.strip()
            or any(ord(ch) < 0x20 or ord(ch) == 0x7f for ch in text)
        ):
            raise ValueError(f"{field} must be a non-empty string")
        result[field] = text.strip()
    capture_bundle, capture_sha = _workspace_hashed_input_json(
        value.get("capture_bundle"),
        value.get("capture_bundle_sha256"),
        "capture_bundle",
    )
    limits, limits_sha = _workspace_hashed_input_json(
        value.get("limits"),
        value.get("limits_sha256"),
        "limits",
    )
    result.update({
        "baseline_kind": "recovered_post_cutover",
        "capture_bundle": capture_bundle,
        "capture_bundle_sha256": capture_sha,
        "limits": limits,
        "limits_sha256": limits_sha,
    })
    result["baseline_output"] = _workspace_output_json(
        value.get("baseline_output"), "baseline_output")
    return result


def _configured_local_build_rt_priority() -> int | None:
    """Return the root-owned machine opt-in for compiler-only SCHED_RR/1.

    Process environment is deliberately ignored: it is controlled by the MCP
    launcher and must not override the machine's exact priority-one ceiling.
    """
    if not sys.platform.startswith("linux"):
        return None
    if not LOCAL_BUILD_ENVIRONMENT_FILE.exists():
        return None
    _require_trusted_root_owned_path(LOCAL_BUILD_ENVIRONMENT_FILE)
    raw = ""
    try:
        lines = LOCAL_BUILD_ENVIRONMENT_FILE.read_text(
            encoding="utf-8").splitlines()
    except OSError as exc:
        raise RuntimeError(
            f"cannot read {LOCAL_BUILD_ENVIRONMENT_FILE}: {exc}") from exc
    for line in lines:
        key, separator, value = line.partition("=")
        if separator and key.strip() == LOCAL_CLANG_RT_PRIORITY_KEY:
            raw = value.strip()
            if (len(raw) >= 2
                    and raw[0] == raw[-1]
                    and raw[0] in {"'", '"'}):
                raw = raw[1:-1].strip()
            break
    if not raw:
        return None
    try:
        priority = int(raw, 10)
    except ValueError as exc:
        raise ValueError(
            f"{LOCAL_CLANG_RT_PRIORITY_KEY} must be exactly "
            f"{LOCAL_CLANG_RT_PRIORITY}") from exc
    if priority != LOCAL_CLANG_RT_PRIORITY:
        raise ValueError(
            f"{LOCAL_CLANG_RT_PRIORITY_KEY} must be exactly "
            f"{LOCAL_CLANG_RT_PRIORITY}")
    return priority


def _local_clang_rt_is_enabled(
        active_preset: str, rt_priority: int | None) -> bool:
    return (
        rt_priority == LOCAL_CLANG_RT_PRIORITY
        and _is_aws_clang_preset(active_preset)
    )


def _preflight_local_clang_rt_launcher(
        active_preset: str, rt_priority: int | None) -> None:
    """Prove the trusted launcher can create an unprivileged RR/1 probe."""
    if not _local_clang_rt_is_enabled(active_preset, rt_priority):
        return
    for path in (
            LOCAL_CLANG_RT_LAUNCHER,
            LOCAL_LLVM22_CLANG,
            LOCAL_LLVM22_CLANGXX,
            Path("/usr/bin/chrt"),
            Path("/usr/bin/setpriv"),
            Path("/usr/bin/sudo")):
        _trusted_root_owned_executable(str(path))
    sudo_metadata = Path("/usr/bin/sudo").stat()
    if not (sudo_metadata.st_mode & stat.S_ISUID):
        raise RuntimeError("/usr/bin/sudo is not setuid; clang RT is unavailable")
    try:
        probe = subprocess.run(
            [str(LOCAL_CLANG_RT_LAUNCHER), "--preflight"],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(
            "failed to invoke the clang realtime launcher preflight"
        ) from exc
    if probe.returncode != 0:
        detail = (probe.stderr or probe.stdout).strip()
        raise RuntimeError(
            "clang realtime launcher preflight failed: "
            f"{detail or f'exit {probe.returncode}'}")


def _initial_clang_rt_cmake_args(
        active_preset: str, rt_priority: int | None) -> list[str]:
    if not _local_clang_rt_is_enabled(active_preset, rt_priority):
        return []
    launcher = str(LOCAL_CLANG_RT_LAUNCHER)
    return [
        f"-D{key}:FILEPATH={launcher}"
        for key in LOCAL_CLANG_RT_CMAKE_KEYS
    ]


def _clang_rt_cmake_cache_repairs(
        active_build_dir: Path,
        active_preset: str,
        rt_priority: int | None) -> tuple[list[str], list[str]]:
    """Return CMake argv/reasons to enforce or remove our managed launcher.

    Disabling RT or selecting a non-Clang preset removes only the exact
    launcher value managed here; unrelated user-provided compiler launchers
    remain untouched.
    """
    managed = str(LOCAL_CLANG_RT_LAUNCHER)
    enabled = _local_clang_rt_is_enabled(active_preset, rt_priority)
    args: list[str] = []
    reasons: list[str] = []
    for key in LOCAL_CLANG_RT_CMAKE_KEYS:
        current = _cmake_cache_value(active_build_dir, key)
        if enabled and current != managed:
            args.append(f"-D{key}:FILEPATH={managed}")
            reasons.append(f"{key}={managed} (was {current or 'unset'})")
        elif not enabled and current == managed:
            args.append(f"-U{key}")
            reasons.append(f"clearing managed {key}")
    return args, reasons


def tool_start_build(mode: str, build_only: bool = False,
                     debug: bool = False, bench_filter=None,
                     debug_report: bool = False,
                     debug_report_supplied: bool = False,
                     debug_supplied: bool = False,
                     filter_supplied: bool = False,
                     bench_e2e_config=None,
                     postgres_error_safety_review_waiver=None) -> str:
    identity_error = _refuse_root_build_identity("start_build")
    if identity_error is not None:
        return f"Build worker identity error: {identity_error}"
    # Linux scheduling policy is per-thread and inherited across fork/exec.
    # Demote only this MCP request thread before it spawns the detached worker,
    # closing the gap where a server left at a sticky RT policy could otherwise
    # hand that policy to the worker before the worker's own fail-closed check.
    try:
        _ensure_build_worker_timesharing_policy()
    except RuntimeError as exc:
        return f"Build worker scheduling configuration error: {exc}"
    if mode not in VALID_MODES:
        return (f"Invalid mode '{mode}'. Valid modes are: "
                f"{', '.join(VALID_MODES)}.")

    mode_details = MODES[mode]
    resolved_e2e_config, config_error = _validate_bench_e2e_config_override(
        mode, bench_e2e_config)
    if config_error is not None:
        return f"Invalid bench_e2e_config: {config_error}"
    try:
        sql_guardrail_workflow = _sql_guardrail_workflow_request(
            resolved_e2e_config)
    except ValueError as exc:
        return f"Invalid SQL guardrail workflow request: {exc}"
    if sql_guardrail_workflow is not None:
        if build_only:
            return (
                "SQL guardrail workflows own capture/promotion/qualification "
                "semantics and do not accept build_only=true.")
        if debug_report_supplied or debug_report:
            return (
                "SQL guardrail workflows own final framework reports and do "
                "not accept debug_report.")
        if filter_supplied or bench_filter is not None:
            return (
                "SQL guardrail workflows pin the exact guardrail benchmark "
                "matrix and do not accept filter.")
    bench_filter = _ordinary_bench_e2e_default_filter(
        mode,
        bench_filter,
        build_only=build_only,
        debug_report=debug_report,
        filter_supplied=filter_supplied,
        sql_guardrail_workflow=sql_guardrail_workflow,
    )
    postgres_error_safety_review_waiver_snapshot: (
        dict[str, object] | None
    ) = None
    if mode == POSTGRES_ERROR_SAFETY_MODE:
        if debug_supplied or debug:
            return (
                "postgres-error-safety owns two fixed isolated presets and "
                "does not accept the `debug` input.")
        if filter_supplied or bench_filter is not None:
            return (
                "postgres-error-safety is a complete CMake-owned evidence "
                "run and does not accept the `filter` input.")
        if debug_report_supplied or debug_report:
            return (
                "postgres-error-safety does not execute a benchmark CLI and "
                "does not accept the `debug_report` input.")
        try:
            _checked_postgres_error_safety_case_template()
        except RuntimeError as exc:
            return (
                "PostgreSQL error-safety configuration error: "
                f"{exc}")
        if build_only:
            if postgres_error_safety_review_waiver is not None:
                return (
                    "postgres-error-safety build_only generates no runtime "
                    "candidate and does not accept "
                    "postgres_error_safety_review_waiver.")
        else:
            if postgres_error_safety_review_waiver is None:
                return (
                    "Full postgres-error-safety qualification requires "
                    "postgres_error_safety_review_waiver naming an explicit "
                    "content-sealed user waiver.")
            try:
                postgres_error_safety_review_waiver_snapshot = (
                    _load_postgres_error_safety_review_waiver(
                        postgres_error_safety_review_waiver,
                        WORKSPACE_ROOT,
                    )
                )
            except PostgresErrorSafetyPromotionError as exc:
                return (
                    "Invalid PostgreSQL error-safety review waiver: "
                    f"{exc}")
    elif postgres_error_safety_review_waiver is not None:
        return (
            "postgres_error_safety_review_waiver is accepted only for mode "
            f"{POSTGRES_ERROR_SAFETY_MODE!r}.")
    try:
        selected_preset = preset_for(mode, debug)
    except RuntimeError as exc:
        return f"Build preset isolation error: {exc}"

    # Validate + translate runtime selection before spawning the long-running
    # worker. CLI benchmark modes retain their filter CLI. unit+integration
    # accepts only filter.tags, interpreted as Catch2 test-spec expressions.
    # Modes driven by CMake-owned private-cluster targets reject CLI-only
    # options because there is no command argv to receive them.
    bench_flags: list[str] = []
    test_flags: list[str] = []
    if mode == "unit+integration":
        if debug_report_supplied or debug_report:
            return ("`debug_report` is bench-only and is not accepted for "
                    "unit+integration runs.")
        test_flags, ferr = _unit_test_filter_to_cli_args(bench_filter)
        if ferr is not None:
            return f"Invalid unit+integration filter: {ferr}"
    else:
        if (bench_filter or debug_report) and not supports_bench_cli_options(mode):
            return ("`filter`/`debug_report` require a benchmark CLI that accepts "
                    "those options (or unit+integration focused tags); valid benchmark "
                    f"modes are {', '.join(BENCH_CLI_MODES)}; got '{mode}'.")
        filter_args, ferr = _filter_to_cli_args(bench_filter)
        if ferr is not None:
            return f"Invalid filter: {ferr}"
        if debug_report and not filter_args:
            return ("`debug_report` requires a non-empty `filter` — debug is for targeted "
                    "runs, not for instrumenting an entire benchmark sweep.")
        bench_flags = list(filter_args)
        if debug_report:
            bench_flags.append("--debug")

    with _global_lock():
        state = _load_state()
        running_mode, running_entry = _running_in_same_group(
            state, mode, build_only,
            PROJECT_ROOT / "build" / selected_preset)
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

        try:
            worker_env = _build_worker_descendant_environment()
        except RuntimeError as exc:
            return f"Build worker environment error: {exc}"

        # Validate the machine-owned compiler policy only after request
        # validation and conflict detection.  The preflight creates one
        # short-lived unprivileged RR/1 probe; it never changes this persistent
        # MCP server's scheduling policy.
        promotion_only = (
            sql_guardrail_workflow is not None
            and sql_guardrail_workflow["phase"] == "baseline_promotion"
        )
        if promotion_only:
            configured_rt_priority = None
        else:
            try:
                machine_rt_priority = _configured_local_build_rt_priority()
                _preflight_local_clang_rt_launcher(
                    selected_preset, machine_rt_priority)
            except (RuntimeError, ValueError) as exc:
                return f"Build worker scheduling configuration error: {exc}"
            configured_rt_priority = (
                machine_rt_priority
                if _local_clang_rt_is_enabled(
                    selected_preset, machine_rt_priority)
                else None
            )

        # Reserve the slot atomically before spawning the worker so a
        # concurrent start_build observes "running" immediately and
        # doesn't race past the same check.
        started_at = time.time()
        timestamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(started_at))
        log_path = LOG_DIR / (
            f"bioseq_build_mcp_{mode.replace('+', '_')}_"
            f"{'debug_' if debug else ''}{timestamp}.log"
        )

        sql_guardrail_snapshot = (
            json.dumps(
                sql_guardrail_workflow,
                sort_keys=True,
                separators=(",", ":"),
            )
            if sql_guardrail_workflow is not None else ""
        )
        postgres_error_safety_review_waiver_json = (
            json.dumps(
                postgres_error_safety_review_waiver_snapshot,
                sort_keys=True,
                separators=(",", ":"),
            )
            if postgres_error_safety_review_waiver_snapshot is not None
            else ""
        )
        worker_e2e_config = (
            None
            if sql_guardrail_workflow is not None
            else resolved_e2e_config
        )
        state[mode] = {
            "mode": mode,
            "status": "running",
            "step": "spawning",
            "started_at": started_at,
            "completed_at": None,
            # Reserve with this live server PID before spawn. The global lock
            # prevents observers and the worker from seeing an absent slot;
            # the exact detached PID replaces it below before lock release.
            "pid": os.getpid(),
            "log_file": str(log_path),
            "exit_code": None,
            "report_file": None,
            "report_files": [],
            "notes": None,
            "debug": debug,
            "build_only": build_only,
            "preset": selected_preset,
            "build_dir": str(PROJECT_ROOT / "build" / selected_preset),
            "build_dirs": [
                str(path) for path in build_dirs_for(mode, debug)
            ],
            "preset_family": mode_details.preset_family,
            "binary_subdir": mode_details.binary_subdir,
            "uses_private_cluster": mode_details.uses_private_cluster,
            "bench_flags": bench_flags,
            "focused_test_args": test_flags,
            "bench_e2e_config": worker_e2e_config,
            "sql_guardrail_request_sha256": (
                _sha256_bytes(sql_guardrail_snapshot.encode("utf-8"))
                if sql_guardrail_snapshot else None
            ),
            "postgres_error_safety_review_waiver_sha256": (
                _sha256_bytes(
                    postgres_error_safety_review_waiver_json.encode("utf-8"))
                if postgres_error_safety_review_waiver_json else None
            ),
            "rt_priority": configured_rt_priority,
            "effective_run_args":
                test_flags if mode == "unit+integration" else bench_flags,
        }
        _save_state(state)

        server_path = Path(__file__).resolve()
        try:
            proc = subprocess.Popen(
                [sys.executable, str(server_path),
                 "--run-build", mode,
                 "--build-only", ("true" if build_only else "false"),
                 "--debug", ("true" if debug else "false"),
                 "--bench-flags", json.dumps(bench_flags),
                 "--test-flags", json.dumps(test_flags),
                 "--bench-e2e-config", worker_e2e_config or "",
                 "--sql-guardrail-request", sql_guardrail_snapshot,
                 "--postgres-error-safety-review-waiver-snapshot",
                 postgres_error_safety_review_waiver_json],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
                close_fds=True,
                env=worker_env,
            )
        except OSError as exc:
            current = _load_state()
            current.pop(mode, None)
            _save_state(current)
            return f"Build worker spawn error: {exc}"

        state = _load_state()
        state[mode]["pid"] = proc.pid
        _save_state(state)
        _log(f"started worker pid={proc.pid} mode={mode} debug={debug} "
             f"rt_priority={configured_rt_priority} "
             f"bench_flags={bench_flags} focused_test_args={test_flags} "
             f"bench_e2e_config={resolved_e2e_config} "
             "postgres_error_safety_review_waiver_sha256="
             f"{state[mode].get('postgres_error_safety_review_waiver_sha256')} "
             f"log={log_path}")

    return (f"{mode_details.label} {mode_details.verb_is} scheduled "
            f"(preset={selected_preset})"
            + (f" with bench flags {' '.join(bench_flags)}" if bench_flags else "")
            + (f" with focused Catch2 args {shlex.join(test_flags)}"
               if test_flags else "")
            + (f" with E2E config {resolved_e2e_config}"
               if resolved_e2e_config else "")
            + f". Predicted finish in {_format_minutes((mode_details.build_only_expected_duration_min if build_only else  mode_details.expected_duration_min), plural_int=True)}.")


def _run_build_inline(mode: str, build_only: bool = False,
                      debug: bool = False, bench_flags=None,
                      test_flags=None, bench_e2e_config=None,
                      postgres_error_safety_review_waiver_json=None) -> int:
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
    pre-existing entry) finds its state, and the same-host conflict guard still
    prevents lock-group or shared-build-tree races on the box. pid is this
    process's own pid so `status`/`_refresh` reconcile correctly if we die
    mid-build.

    Returns the worker's exit code, or a small non-worker code on a pre-flight
    refusal (2 = invalid mode, 3 = a conflicting build is already running here).
    """
    identity_error = _refuse_root_build_identity("synchronous build worker")
    if identity_error is not None:
        _log(identity_error)
        return 2
    try:
        _ensure_build_worker_timesharing_policy()
    except RuntimeError as exc:
        _log(f"synchronous build worker scheduling error: {exc}")
        return 2
    if mode not in VALID_MODES:
        _log(f"--run-build-sync: invalid mode '{mode}'")
        return 2
    resolved_e2e_config, config_error = _validate_bench_e2e_config_override(
        mode, bench_e2e_config)
    if config_error is not None:
        _log(f"--run-build-sync: invalid bench_e2e_config: {config_error}")
        return 2
    try:
        sql_guardrail_workflow = _sql_guardrail_workflow_request(
            resolved_e2e_config)
    except ValueError as exc:
        _log(
            "--run-build-sync: invalid SQL guardrail workflow request: "
            f"{exc}")
        return 2
    sql_guardrail_snapshot = (
        json.dumps(
            sql_guardrail_workflow,
            sort_keys=True,
            separators=(",", ":"),
        )
        if sql_guardrail_workflow is not None else None
    )
    worker_e2e_config = (
        None if sql_guardrail_workflow is not None
        else resolved_e2e_config
    )
    if sql_guardrail_workflow is not None and (
        build_only or bench_flags or test_flags
    ):
        _log(
            "--run-build-sync: SQL guardrail workflows reject build-only "
            "and focused runtime flags")
        return 2
    if mode == POSTGRES_ERROR_SAFETY_MODE:
        if build_only:
            if postgres_error_safety_review_waiver_json:
                _log(
                    "--run-build-sync: postgres-error-safety build_only "
                    "rejects review-waiver authority")
                return 2
        else:
            if not postgres_error_safety_review_waiver_json:
                _log(
                    "--run-build-sync: full postgres-error-safety requires "
                    "an immutable review-waiver snapshot")
                return 2
            try:
                review_waiver_value = json.loads(
                    postgres_error_safety_review_waiver_json)
                review_waiver_normalized = (
                    _validate_safety_waiver_snapshot(
                        review_waiver_value,
                        WORKSPACE_ROOT,
                    )
                )
            except (
                json.JSONDecodeError,
                PostgresErrorSafetyPromotionError,
            ) as exc:
                _log(
                    "--run-build-sync: invalid PostgreSQL error-safety "
                    f"review-waiver snapshot: {exc}")
                return 2
            canonical_review_waiver = json.dumps(
                review_waiver_normalized,
                sort_keys=True,
                separators=(",", ":"),
            )
            if (
                canonical_review_waiver
                != postgres_error_safety_review_waiver_json
            ):
                _log(
                    "--run-build-sync: PostgreSQL error-safety "
                    "review-waiver snapshot is not canonical")
                return 2
    elif postgres_error_safety_review_waiver_json:
        _log(
            "--run-build-sync: review-waiver authority is valid only for "
            "postgres-error-safety")
        return 2
    try:
        selected_preset = preset_for(mode, debug)
    except RuntimeError as exc:
        _log(f"--run-build-sync: build preset isolation error: {exc}")
        return 2

    with _global_lock():
        state = _load_state()
        running_mode, running_entry = _running_in_same_group(
            state, mode, build_only,
            PROJECT_ROOT / "build" / selected_preset)
        if running_entry:
            _log(f"--run-build-sync: a conflicting build ({running_mode}) is "
                 f"already running on this host (pid={running_entry.get('pid')}); "
                 f"refusing to start {mode}")
            return 3

        promotion_only = (
            sql_guardrail_workflow is not None
            and sql_guardrail_workflow["phase"] == "baseline_promotion"
        )
        if promotion_only:
            configured_rt_priority = None
        else:
            try:
                machine_rt_priority = _configured_local_build_rt_priority()
                _preflight_local_clang_rt_launcher(
                    selected_preset, machine_rt_priority)
            except (RuntimeError, ValueError) as exc:
                _log(
                    "--run-build-sync: scheduling configuration error: "
                    f"{exc}")
                return 2
            configured_rt_priority = (
                machine_rt_priority
                if _local_clang_rt_is_enabled(
                    selected_preset, machine_rt_priority)
                else None
            )

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
            "report_files": [],
            "notes": None,
            "debug": debug,
            "build_only": build_only,
            "preset": selected_preset,
            "build_dir": str(PROJECT_ROOT / "build" / selected_preset),
            "build_dirs": [
                str(path) for path in build_dirs_for(mode, debug)
            ],
            "preset_family": MODES[mode].preset_family,
            "binary_subdir": MODES[mode].binary_subdir,
            "uses_private_cluster": MODES[mode].uses_private_cluster,
            "bench_flags": list(bench_flags or []),
            "focused_test_args": list(test_flags or []),
            "bench_e2e_config": worker_e2e_config,
            "sql_guardrail_request_sha256": (
                _sha256_bytes(sql_guardrail_snapshot.encode("utf-8"))
                if sql_guardrail_snapshot else None
            ),
            "postgres_error_safety_review_waiver_sha256": (
                _sha256_bytes(
                    postgres_error_safety_review_waiver_json.encode("utf-8"))
                if postgres_error_safety_review_waiver_json else None
            ),
            "rt_priority": configured_rt_priority,
            "effective_run_args": (
                list(test_flags or []) if mode == "unit+integration"
                else list(bench_flags or [])
            ),
        }
        _save_state(state)
        _log(f"--run-build-sync: running {mode} inline (pid={os.getpid()}, "
             f"preset={selected_preset}, build_only={build_only}, "
             f"clang_rt_priority={configured_rt_priority}, "
             f"bench_flags={bench_flags or []}, "
             f"focused_test_args={test_flags or []}, log={log_path})")

    return _run_build_worker(mode, build_only=build_only, debug=debug,
                             bench_flags=bench_flags, test_flags=test_flags,
                             bench_e2e_config=worker_e2e_config,
                             sql_guardrail_request_json=
                             sql_guardrail_snapshot,
                             postgres_error_safety_review_waiver_json=
                             postgres_error_safety_review_waiver_json)


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

    target_definition = INSPECT_TARGETS.get(target)
    if target_definition is None:
        return json.dumps(
            {"error": f"unsupported inspect target {target!r}",
             "supported_targets": sorted(INSPECT_TARGETS)},
            indent=2,
        )

    # Resolve which preset family owns the requested binary. `debug` is an
    # explicit override; without it, prefer that family's debug tree if built.
    inspect_mode = target_definition.mode
    debug_raw = arguments.get("debug")
    try:
        if debug_raw is None:
            debug_chosen = (
                build_dir_for(inspect_mode, True) / "build.ninja"
            ).exists()
        else:
            debug_chosen = parse_bool("debug", debug_raw)
        inspect_build_dir = build_dir_for(inspect_mode, debug_chosen)
        inspect_preset = preset_for(inspect_mode, debug_chosen)
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
        binary_subdir=MODES[inspect_mode].binary_subdir,
        artifact_names=target_definition.artifact_names,
        shared_target_names=target_definition.shared_target_names,
    )
    # Surface which tree was inspected so the AI agent can act on
    # "no .s files" results by re-running with debug=true.
    out.setdefault("build_metadata", {})["debug_tree_used"] = debug_chosen
    out["build_metadata"]["preset_used"] = inspect_preset
    out["build_metadata"]["test_indexes_expected"] = (
        MODES[inspect_mode].preset_family == "test")
    out["build_metadata"]["preset_family"] = (
        MODES[inspect_mode].preset_family)
    return json.dumps(out, indent=2)


def tool_status(mode: str) -> str:
    if mode not in MODES:
        return (f"Invalid mode '{mode}'. Valid modes are: "
                f"{', '.join(VALID_MODES)}.")

    with _global_lock():
        state = _load_state()
        refreshed_entry = _refresh(state, mode)
        entry = (
            dict(refreshed_entry)
            if refreshed_entry is not None else None
        )
    mode_details = MODES[mode]

    if not entry:
        return (f"{mode_details.label} {mode_details.verb_was} never scheduled. "
                f"Use start_build to start a run.")

    log_path = entry.get("log_file") or "(no log file)"
    status = entry.get("status")
    build_only: bool = entry.get("build_only", False)
    debug_used: bool = bool(entry.get("debug", False))
    selected_preset = entry.get(
        "preset", preset_for(mode, debug_used))
    private_cluster = bool(entry.get(
        "uses_private_cluster", mode_details.uses_private_cluster))
    preset_suffix = (
        f" preset={selected_preset}"
        + (" private-cluster=yes" if private_cluster else "")
    )
    focused_test_args = entry.get("focused_test_args") or []
    focus_suffix = (
        f" Focused Catch2 args: {shlex.join(focused_test_args)}."
        if focused_test_args else ""
    )
    report_files = entry.get("report_files")
    if isinstance(report_files, list) and report_files:
        report_suffix = ". Reports: " + ", ".join(
            str(path) for path in report_files)
    elif entry.get("report_file"):
        # Compatibility with state entries created before ordered receipts.
        report_suffix = f". Report: {entry['report_file']}"
    else:
        report_suffix = ""

    if status == "running":
        elapsed_min = (time.time() - entry["started_at"]) / 60.0
        eta_min = max(0.0, (mode_details.build_only_expected_duration_min if build_only else  mode_details.expected_duration_min) - elapsed_min)
        step = entry.get("step", "running")
        return (f"{mode_details.label} {mode_details.verb_is} already scheduled"
                f"{preset_suffix}. "
                f"Current step: {step}. "
                f"Predicted finish in {_format_minutes(eta_min)}. "
                f"{focus_suffix} "
                f"Log: {log_path}{report_suffix}")

    completed_at = entry.get("completed_at") or time.time()
    duration_min = (completed_at - entry["started_at"]) / 60.0

    if status == "completed":
        msg = (f"{mode_details.label} {mode_details.verb_was} already completed"
               f"{preset_suffix} "
               f"(total time: {_format_minutes(duration_min, plural_int=True)}). "
               f"{focus_suffix} "
               f"Log: {log_path}{report_suffix}")
        workflow_evidence = entry.get("sql_guardrail_workflow")
        if isinstance(workflow_evidence, dict):
            surfaced = {
                key: workflow_evidence[key]
                for key in (
                    "capture_bundle",
                    "capture_bundle_sha256",
                    "canonical_environment_key",
                    "reviewed_baseline",
                    "reviewed_baseline_sha256",
                    "qualification",
                    "qualification_sha256",
                )
                if workflow_evidence.get(key) is not None
            }
            msg += (
                ". SQL guardrail evidence: "
                + json.dumps(surfaced, sort_keys=True)
            )
        if entry.get("build_only", False):
            msg += (". This was a build-only job, so compile targets were built "
                    "but commands and post-build/private-cluster targets were not run.")
        if debug_used:
            msg += (f". This was a debug build (preset={selected_preset}); "
                    "DWARF + per-TU .s/.bc artefacts are available under "
                    f"{entry.get('build_dir', str(build_dir_for(mode, True)))} for the "
                    "`inspect` tool.")
        return msg

    if status == "killed_timeout":
        return ("Last run was forcefully killed due to timeout. "
                f"{focus_suffix} "
                f"Logs for inspection are available here: "
                f"{log_path}{report_suffix}")

    if status == "failed":
        notes = entry.get("notes") or "(no notes)"
        private_note = (
            " The native private-cluster executable is responsible for "
            "capturing logs and cleanup."
            if private_cluster else ""
        )
        return (f"{mode_details.label} run failed after "
                f"{_format_minutes(duration_min)}: {notes}. "
                f"{focus_suffix} "
                f"Log: {log_path}{report_suffix}.{private_note}")

    if status == "aborted":
        return (f"{mode_details.label} run was aborted (worker process died "
                f"unexpectedly after {_format_minutes(duration_min)}). "
                f"{focus_suffix} "
                f"Log: {log_path}{report_suffix}")

    return (f"{mode_details.label} status: {status}."
            f"{focus_suffix} Log: {log_path}{report_suffix}")


def _sha256_file(path: Path) -> str:
    """Return a streaming SHA-256 digest for a regular file."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _papers_build_payload(entry: dict) -> dict:
    """Create the stable JSON response shared by start and status actions."""
    payload = {
        "tool": "papers-build",
        "status": entry.get("status", "unknown"),
        "step": entry.get("step", "unknown"),
        "target": PAPERS_TARGET,
        "source_dir": str(PAPERS_DIR),
        "build_dir": str(PAPERS_BUILD_DIR),
        "pdf_path": str(PAPERS_PDF_PATH),
        "log_path": entry.get("log_file"),
        "pid": entry.get("pid"),
        "started_at": entry.get("started_at"),
        "completed_at": entry.get("completed_at"),
        "exit_code": entry.get("exit_code"),
        "notes": entry.get("notes"),
        "configure_command": entry.get("configure_command"),
        "build_command": entry.get("build_command"),
    }
    started_at = entry.get("started_at")
    completed_at = entry.get("completed_at")
    if isinstance(started_at, (int, float)):
        payload["elapsed_seconds"] = round(
            (completed_at or time.time()) - started_at, 3)
    if PAPERS_PDF_PATH.is_file():
        metadata = PAPERS_PDF_PATH.stat()
        payload["pdf"] = {
            "path": str(PAPERS_PDF_PATH),
            "bytes": metadata.st_size,
            "modified_at": metadata.st_mtime,
            "sha256": _sha256_file(PAPERS_PDF_PATH),
            "updated_during_current_run": (
                not isinstance(started_at, (int, float))
                or metadata.st_mtime >= started_at
            ),
            "validated_by_current_run": (
                entry.get("status") == "completed"
                and entry.get("exit_code") == 0
            ),
        }
    if entry.get("status") in {
            "completed", "failed", "aborted", "killed_timeout"}:
        payload["log_tail"] = _tail_path(entry.get("log_file"), 80)
    return payload


def _run_papers_build_worker() -> int:
    """Configure the papers CMake project and build only the thesis target."""
    state = _load_state()
    entry = state.get(PAPERS_BUILD_STATE_KEY)
    if not entry:
        _log("papers worker: no reserved state entry, exiting")
        return 1

    started_at = float(entry["started_at"])
    deadline = started_at + PAPERS_BUILD_HARD_TIMEOUT_SEC
    log_path = Path(entry["log_file"])
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_handle = log_path.open("a", encoding="utf-8")
    cmake = shutil.which("cmake")
    try:
        identity_error = _refuse_root_build_identity(
            "detached papers build worker")
        if identity_error is not None:
            raise RuntimeError(identity_error)
        _ensure_build_worker_timesharing_policy()
        if cmake is None:
            raise RuntimeError("cmake was not found on PATH")
        if not (PAPERS_DIR / "CMakeLists.txt").is_file():
            raise RuntimeError(
                f"paper CMake project is missing: {PAPERS_DIR / 'CMakeLists.txt'}")

        configure_command = [
            cmake,
            "-S", str(PAPERS_DIR),
            "-B", str(PAPERS_BUILD_DIR),
        ]
        build_command = [
            cmake,
            "--build", str(PAPERS_BUILD_DIR),
            "--target", PAPERS_TARGET,
            "--parallel", str(CMAKE_PARALLEL_JOBS),
        ]
        log_handle.write(
            f"# papers-build started_at={started_at}\n"
            f"# source={PAPERS_DIR}\n"
            f"# build={PAPERS_BUILD_DIR}\n"
            f"# target={PAPERS_TARGET}\n"
            f"# configure={shlex.join(configure_command)}\n"
            f"# build_command={shlex.join(build_command)}\n")
        log_handle.flush()

        _update_entry(
            PAPERS_BUILD_STATE_KEY,
            status="running",
            step="configuring",
            configure_command=configure_command,
            build_command=build_command,
        )
        configure_rc = _stream_subprocess(
            configure_command, log_handle, deadline, cwd=PAPERS_DIR)
        if configure_rc != 0:
            raise RuntimeError(
                f"CMake configure exited with code {configure_rc}")

        _update_entry(
            PAPERS_BUILD_STATE_KEY,
            status="running",
            step=f"building target {PAPERS_TARGET}",
        )
        build_rc = _stream_subprocess(
            build_command, log_handle, deadline, cwd=PAPERS_DIR)
        if build_rc != 0:
            raise RuntimeError(
                f"CMake target {PAPERS_TARGET!r} exited with code {build_rc}")
        if not PAPERS_PDF_PATH.is_file():
            raise RuntimeError(
                f"CMake completed but did not produce {PAPERS_PDF_PATH}")
        if PAPERS_PDF_PATH.stat().st_size == 0:
            raise RuntimeError(
                f"CMake produced an empty PDF: {PAPERS_PDF_PATH}")

        completed_at = time.time()
        _update_entry(
            PAPERS_BUILD_STATE_KEY,
            status="completed",
            step="completed",
            completed_at=completed_at,
            exit_code=0,
            notes=None,
            pdf_sha256=_sha256_file(PAPERS_PDF_PATH),
            pdf_bytes=PAPERS_PDF_PATH.stat().st_size,
        )
        log_handle.write(
            f"\n# papers-build completed_at={completed_at}\n"
            f"# pdf={PAPERS_PDF_PATH}\n")
        return 0
    except TimeoutError as exc:
        completed_at = time.time()
        _update_entry(
            PAPERS_BUILD_STATE_KEY,
            status="killed_timeout",
            step="killed_timeout",
            completed_at=completed_at,
            exit_code=124,
            notes=str(exc),
        )
        log_handle.write(f"\n# papers-build timed out: {exc}\n")
        return 124
    except Exception as exc:
        completed_at = time.time()
        _update_entry(
            PAPERS_BUILD_STATE_KEY,
            status="failed",
            step="failed",
            completed_at=completed_at,
            exit_code=1,
            notes=str(exc),
        )
        log_handle.write(f"\n# papers-build failed: {exc}\n")
        return 1
    finally:
        log_handle.close()


def tool_papers_build(arguments: dict) -> str:
    """Start or poll the detached thesis CMake build."""
    action = arguments.get("action", "start")
    if action not in {"start", "status"}:
        return json.dumps({
            "tool": "papers-build",
            "status": "error",
            "error": "action must be 'start' or 'status'",
        }, indent=2)

    with _global_lock():
        state = _load_state()
        entry = _refresh(state, PAPERS_BUILD_STATE_KEY)
        if action == "status":
            if entry is None:
                return json.dumps({
                    "tool": "papers-build",
                    "status": "never_scheduled",
                    "hint": "Call papers-build with action='start'.",
                }, indent=2)
            return json.dumps(_papers_build_payload(entry), indent=2)

        if entry is not None and entry.get("status") == "running":
            return json.dumps(_papers_build_payload(entry), indent=2)

        started_at = time.time()
        timestamp = time.strftime(
            "%Y%m%d-%H%M%S", time.localtime(started_at))
        log_path = LOG_DIR / (
            f"bioseq_build_mcp_papers_{timestamp}_"
            f"{secrets.token_hex(3)}.log")
        state[PAPERS_BUILD_STATE_KEY] = {
            "mode": PAPERS_BUILD_STATE_KEY,
            "status": "running",
            "step": "spawning",
            "started_at": started_at,
            "completed_at": None,
            "pid": os.getpid(),
            "log_file": str(log_path),
            "exit_code": None,
            "notes": None,
            "target": PAPERS_TARGET,
            "source_dir": str(PAPERS_DIR),
            "build_dir": str(PAPERS_BUILD_DIR),
            "pdf_path": str(PAPERS_PDF_PATH),
        }
        _save_state(state)

        server_path = Path(__file__).resolve()
        worker_environment = dict(os.environ)
        worker_environment["BIOSEQ_DOCS_AUTOSTART"] = "0"
        try:
            process = subprocess.Popen(
                [sys.executable, str(server_path), "--run-papers-build"],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
                close_fds=True,
                env=worker_environment,
            )
        except OSError as exc:
            current = _load_state()
            current[PAPERS_BUILD_STATE_KEY] = {
                **state[PAPERS_BUILD_STATE_KEY],
                "status": "failed",
                "step": "spawn_failed",
                "completed_at": time.time(),
                "exit_code": 1,
                "notes": str(exc),
            }
            _save_state(current)
            return json.dumps(
                _papers_build_payload(current[PAPERS_BUILD_STATE_KEY]),
                indent=2,
            )

        current = _load_state()
        current[PAPERS_BUILD_STATE_KEY]["pid"] = process.pid
        _save_state(current)
        entry = current[PAPERS_BUILD_STATE_KEY]
        _log(
            f"started papers worker pid={process.pid} "
            f"target={PAPERS_TARGET} log={log_path}")
        return json.dumps(_papers_build_payload(entry), indent=2)


def _run_capture(command: list[str], *, timeout: float = 60.0) -> dict:
    """Run a bounded read-only PDF helper and retain its exact diagnostics."""
    try:
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {
            "command": command,
            "exit_code": None,
            "stdout": "",
            "stderr": str(exc),
        }
    return {
        "command": command,
        "exit_code": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
    }


def _papers_pdf_info(pdf_path: Path) -> tuple[dict, dict]:
    """Return parsed `pdfinfo` fields and the underlying command result."""
    executable = shutil.which("pdfinfo")
    if executable is None:
        return {}, {
            "command": ["pdfinfo", str(pdf_path)],
            "exit_code": None,
            "stdout": "",
            "stderr": "pdfinfo was not found on PATH",
        }
    result = _run_capture([executable, str(pdf_path)])
    fields = {}
    if result["exit_code"] == 0:
        for line in result["stdout"].splitlines():
            key, separator, value = line.partition(":")
            if separator:
                fields[key.strip()] = value.strip()
    return fields, result


def _papers_log_diagnostics(log_path: Path) -> dict:
    """Extract layout and reference warnings from the final LuaLaTeX log."""
    if not log_path.is_file():
        return {
            "path": str(log_path),
            "available": False,
            "overfull_boxes": [],
            "underfull_box_count": 0,
            "undefined_reference_count": 0,
            "latex_errors": [],
            "warning_count": 0,
        }
    lines = log_path.read_text(
        encoding="utf-8", errors="replace").splitlines()
    overfull_pattern = re.compile(
        r"Overfull \\([hv])box "
        r"\(([-+]?\d+(?:\.\d+)?)pt too (wide|high)\)")
    overfull_boxes = []
    for line_number, line in enumerate(lines, 1):
        match = overfull_pattern.search(line)
        if match is not None:
            overfull_boxes.append({
                "log_line": line_number,
                "axis": match.group(1),
                "overflow_pt": float(match.group(2)),
                "direction": match.group(3),
                "message": line.strip(),
                "context": lines[line_number:line_number + 2],
            })
    warnings = [
        {"log_line": number, "message": line.strip()}
        for number, line in enumerate(lines, 1)
        if "Warning:" in line
    ]
    undefined_references = [
        item for item in warnings
        if "undefined" in item["message"].lower()
    ]
    latex_errors = [
        {"log_line": number, "message": line.strip()}
        for number, line in enumerate(lines, 1)
        if line.startswith("!")
    ]
    return {
        "path": str(log_path),
        "available": True,
        "overfull_boxes": overfull_boxes,
        "underfull_box_count": sum(
            "Underfull \\" in line and "box" in line for line in lines),
        "undefined_reference_count": len(undefined_references),
        "undefined_references": undefined_references[:50],
        "latex_errors": latex_errors[:50],
        "warning_count": len(warnings),
        "warnings": warnings[:100],
    }


def _papers_source_freshness(pdf_path: Path) -> dict:
    """Report source files newer than the generated PDF."""
    if not pdf_path.is_file():
        return {"stale": True, "newer_sources": []}
    pdf_mtime = pdf_path.stat().st_mtime
    candidates = [
        PAPERS_DIR / "CMakeLists.txt",
        PAPERS_DIR / "requirements.txt",
    ]
    source_root = PAPERS_DIR / "src"
    if source_root.is_dir():
        candidates.extend(
            path for path in source_root.rglob("*") if path.is_file())
    newer_sources = sorted(
        str(path)
        for path in candidates
        if path.is_file() and path.stat().st_mtime > pdf_mtime
    )
    return {
        "stale": bool(newer_sources),
        "pdf_modified_at": pdf_mtime,
        "newer_source_count": len(newer_sources),
        "newer_sources": newer_sources[:100],
    }


def _papers_text_bounds(
        pdf_path: Path, output_dir: Path) -> tuple[list[dict], dict]:
    """Use Poppler's XHTML bounding boxes to detect text outside a page."""
    executable = shutil.which("pdftotext")
    bbox_path = output_dir / "text-bounds.html"
    if executable is None:
        return [], {
            "command": ["pdftotext", "-bbox-layout", str(pdf_path),
                        str(bbox_path)],
            "exit_code": None,
            "stdout": "",
            "stderr": "pdftotext was not found on PATH",
        }
    if not bbox_path.is_file() or bbox_path.stat().st_mtime < pdf_path.stat().st_mtime:
        result = _run_capture([
            executable,
            "-bbox-layout",
            str(pdf_path),
            str(bbox_path),
        ])
    else:
        result = {
            "command": [
                executable, "-bbox-layout", str(pdf_path), str(bbox_path)],
            "exit_code": 0,
            "stdout": "",
            "stderr": "",
            "cached": True,
        }
    if result["exit_code"] != 0 or not bbox_path.is_file():
        return [], result

    try:
        root = ElementTree.parse(bbox_path).getroot()
    except (OSError, ElementTree.ParseError) as exc:
        result["exit_code"] = None
        result["stderr"] = f"could not parse bounding boxes: {exc}"
        return [], result

    pages = []
    tolerance = 0.5
    for page_number, page in enumerate(
            (node for node in root.iter()
             if node.tag.rsplit("}", 1)[-1] == "page"), 1):
        width = float(page.attrib.get("width", 0.0))
        height = float(page.attrib.get("height", 0.0))
        word_count = 0
        outside_words = []
        for word in (
                node for node in page.iter()
                if node.tag.rsplit("}", 1)[-1] == "word"):
            word_count += 1
            try:
                box = {
                    key: float(word.attrib[key])
                    for key in ("xMin", "yMin", "xMax", "yMax")
                }
            except (KeyError, ValueError):
                continue
            if (
                box["xMin"] < -tolerance
                or box["yMin"] < -tolerance
                or box["xMax"] > width + tolerance
                or box["yMax"] > height + tolerance
            ):
                outside_words.append({
                    "text": "".join(word.itertext()),
                    **box,
                })
        pages.append({
            "page": page_number,
            "width_pt": width,
            "height_pt": height,
            "word_count": word_count,
            "outside_text_box_count": len(outside_words),
            "outside_text_boxes": outside_words[:20],
        })
    return pages, result


def _png_dimensions(path: Path) -> tuple[int, int] | None:
    """Read PNG dimensions without adding an image-library dependency."""
    try:
        with path.open("rb") as handle:
            header = handle.read(24)
    except OSError:
        return None
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return (
        int.from_bytes(header[16:20], "big"),
        int.from_bytes(header[20:24], "big"),
    )


def _render_papers_pages(
        pdf_path: Path,
        pages: list[int],
        dpi: int,
        output_dir: Path,
) -> tuple[list[dict], list[dict]]:
    """Render requested PDF pages to deterministic PNG paths."""
    executable = shutil.which("pdftoppm")
    if executable is None:
        return [], [{
            "page": None,
            "error": "pdftoppm was not found on PATH",
        }]
    render_dir = output_dir / f"dpi-{dpi}"
    render_dir.mkdir(parents=True, exist_ok=True)
    rendered = []
    errors = []
    for page in pages:
        prefix = render_dir / f"page-{page:04d}"
        image_path = prefix.with_suffix(".png")
        if (
            not image_path.is_file()
            or image_path.stat().st_mtime < pdf_path.stat().st_mtime
        ):
            result = _run_capture([
                executable,
                "-png",
                "-r", str(dpi),
                "-f", str(page),
                "-l", str(page),
                "-singlefile",
                str(pdf_path),
                str(prefix),
            ], timeout=120.0)
            if result["exit_code"] != 0:
                errors.append({
                    "page": page,
                    "error": (
                        result["stderr"].strip()
                        or f"pdftoppm exited with {result['exit_code']}"
                    ),
                    "command": result["command"],
                })
                continue
        dimensions = _png_dimensions(image_path)
        rendered.append({
            "page": page,
            "path": str(image_path),
            "bytes": image_path.stat().st_size,
            "width_px": dimensions[0] if dimensions else None,
            "height_px": dimensions[1] if dimensions else None,
            "dpi": dpi,
        })
    return rendered, errors


def _papers_inspect_error(message: str, **details) -> dict:
    payload = {
        "tool": "papers-inspect",
        "status": "error",
        "error": message,
        **details,
    }
    return {
        "content": [{
            "type": "text",
            "text": json.dumps(payload, indent=2),
        }],
        "isError": True,
    }


def tool_papers_inspect(arguments: dict) -> dict:
    """Render the thesis PDF and return diagnostics plus MCP image content."""
    if not PAPERS_PDF_PATH.is_file():
        return _papers_inspect_error(
            "The thesis PDF does not exist. Run papers-build first.",
            pdf_path=str(PAPERS_PDF_PATH),
        )

    raw_pages = arguments.get("pages")
    if raw_pages is not None:
        if (
            not isinstance(raw_pages, list)
            or not raw_pages
            or len(raw_pages) > 20
            or any(isinstance(page, bool) or not isinstance(page, int)
                   for page in raw_pages)
        ):
            return _papers_inspect_error(
                "pages must be a non-empty array of at most 20 integers")
        requested_pages = list(dict.fromkeys(raw_pages))
    else:
        requested_pages = None

    raw_dpi = arguments.get("dpi")
    if raw_dpi is None:
        dpi = 160 if requested_pages is not None else 48
    elif isinstance(raw_dpi, bool) or not isinstance(raw_dpi, int):
        return _papers_inspect_error("dpi must be an integer")
    else:
        dpi = raw_dpi
    if dpi < 36 or dpi > 300:
        return _papers_inspect_error("dpi must be between 36 and 300")

    try:
        include_images = parse_bool(
            "include_images", arguments.get("include_images", True))
    except RuntimeError as exc:
        return _papers_inspect_error(str(exc))

    pdf_sha256 = _sha256_file(PAPERS_PDF_PATH)
    output_dir = PAPERS_INSPECT_ROOT / pdf_sha256[:16]
    output_dir.mkdir(parents=True, exist_ok=True)
    pdf_info, pdf_info_command = _papers_pdf_info(PAPERS_PDF_PATH)
    try:
        page_count = int(pdf_info.get("Pages", "0"))
    except ValueError:
        page_count = 0
    if pdf_info_command["exit_code"] != 0 or page_count <= 0:
        return _papers_inspect_error(
            "pdfinfo could not read a positive page count",
            pdf_path=str(PAPERS_PDF_PATH),
            pdfinfo=pdf_info_command,
        )

    if requested_pages is None:
        pages = list(range(
            1, min(page_count, PAPERS_INSPECT_MAX_OVERVIEW_PAGES) + 1))
        overview_truncated = page_count > PAPERS_INSPECT_MAX_OVERVIEW_PAGES
    else:
        invalid_pages = [
            page for page in requested_pages
            if page < 1 or page > page_count
        ]
        if invalid_pages:
            return _papers_inspect_error(
                "requested pages are outside the PDF page range",
                page_count=page_count,
                invalid_pages=invalid_pages,
            )
        pages = requested_pages
        overview_truncated = False

    text_bounds, bbox_command = _papers_text_bounds(
        PAPERS_PDF_PATH, output_dir)
    render_manifest, render_errors = _render_papers_pages(
        PAPERS_PDF_PATH, pages, dpi, output_dir)
    log_diagnostics = _papers_log_diagnostics(PAPERS_LATEX_LOG_PATH)
    source_freshness = _papers_source_freshness(PAPERS_PDF_PATH)
    outside_text_pages = [
        page["page"] for page in text_bounds
        if page["outside_text_box_count"] > 0
    ]
    empty_text_pages = [
        page["page"] for page in text_bounds
        if page["word_count"] == 0
    ]
    issue_count = (
        len(log_diagnostics["overfull_boxes"])
        + len(log_diagnostics["latex_errors"])
        + len(outside_text_pages)
        + len(render_errors)
        + int(source_freshness["stale"])
    )
    payload = {
        "tool": "papers-inspect",
        "status": "completed" if not render_errors else "partial",
        "automated_verdict": (
            "issues_detected"
            if issue_count else "no_automated_issues_detected"
        ),
        "visual_review_required": True,
        "agent_instructions": [
            (
                "Inspect every returned overview image for clipping, overlaps, "
                "unexpected blank areas, inconsistent margins, misplaced "
                "figures/tables, and broken glyphs."
            ),
            (
                "For anything suspicious, call papers-inspect again with "
                "pages=[...] and a higher dpi (160-220)."
            ),
            (
                "Automated checks cover LaTeX box warnings and text bounds; "
                "they cannot prove that graphical elements are semantically "
                "positioned correctly."
            ),
        ],
        "pdf": {
            "path": str(PAPERS_PDF_PATH),
            "sha256": pdf_sha256,
            "bytes": PAPERS_PDF_PATH.stat().st_size,
            "page_count": page_count,
            "metadata": pdf_info,
            "source_freshness": source_freshness,
        },
        "render": {
            "mode": "focused" if requested_pages is not None else "overview",
            "dpi": dpi,
            "pages_requested": pages,
            "overview_truncated": overview_truncated,
            "manifest": render_manifest,
            "errors": render_errors,
        },
        "layout_diagnostics": {
            "latex": log_diagnostics,
            "text_bounds": text_bounds,
            "outside_text_pages": outside_text_pages,
            "empty_text_pages": empty_text_pages,
            "bbox_command": bbox_command,
        },
        "pdfinfo_command": pdf_info_command,
    }

    content = [{
        "type": "text",
        "text": json.dumps(payload, indent=2),
    }]
    embedded_bytes = 0
    embedded_pages = []
    if include_images:
        for page in render_manifest:
            image_path = Path(page["path"])
            image_bytes = image_path.read_bytes()
            if (
                embedded_bytes + len(image_bytes)
                > PAPERS_INSPECT_MAX_EMBED_BYTES
            ):
                continue
            content.append({
                "type": "image",
                "data": base64.b64encode(image_bytes).decode("ascii"),
                "mimeType": "image/png",
            })
            embedded_bytes += len(image_bytes)
            embedded_pages.append(page["page"])
    payload["render"]["embedded_pages"] = embedded_pages
    payload["render"]["embedded_bytes"] = embedded_bytes
    payload["render"]["image_content_order"] = embedded_pages
    content[0]["text"] = json.dumps(payload, indent=2)
    return {
        "content": content,
        "isError": bool(render_errors),
    }


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
        test_flags = payload.get("focused_test_args") or []
        bench_e2e_config = payload.get("bench_e2e_config") or ""
        remote_cmd = (
            f"cd {server_dir} && {env_prefix}python3 server.py "
            f"--run-build-sync {shlex.quote(mode)} "
            f"--build-only {'true' if build_only else 'false'} "
            f"--debug {'true' if debug else 'false'} "
            f"--bench-flags {shlex.quote(json.dumps(bench_flags))} "
            f"--test-flags {shlex.quote(json.dumps(test_flags))} "
            f"--bench-e2e-config {shlex.quote(bench_e2e_config)}"
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
        if mode in REMOTE_RUN_REJECTED_MODES:
            return (
                f"Mode '{mode}' is local-only and is not supported by "
                "remote-run. Use the local start_build tool instead.")
        build_only = parse_bool("build_only", arguments.get("build_only", False))
        debug = parse_bool("debug", arguments.get("debug", False))
        bench_filter = arguments.get("filter")
        debug_report = parse_bool("debug_report",
                                  arguments.get("debug_report", False))
        bench_e2e_config = arguments.get("bench_e2e_config")
        if bench_e2e_config is not None:
            if mode != "bench-e2e":
                return (
                    "remote bench_e2e_config is accepted only for "
                    "mode='bench-e2e'.")
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
                    "workspace-relative .json path available in the pushed "
                    "branch/on-box checkout.")
        bench_flags: list[str] = []
        test_flags: list[str] = []
        if mode == "unit+integration":
            if "debug_report" in arguments or debug_report:
                return ("`debug_report` is bench-only and is not accepted for "
                        "unit+integration runs.")
            test_flags, ferr = _unit_test_filter_to_cli_args(bench_filter)
            if ferr is not None:
                return f"Invalid unit+integration filter: {ferr}"
        else:
            if (bench_filter or debug_report) and not supports_bench_cli_options(mode):
                return ("`filter`/`debug_report` require a benchmark CLI that accepts "
                        "those options (or unit+integration focused tags); valid benchmark "
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


# --------------------------- MCP protocol layer ----------------------------

PROTOCOL_VERSION = "2024-11-05"
SERVER_INFO = {"name": "bioseq-build-mcp", "version": "1.1.0"}

TOOL_DEFINITIONS = [
    {
        "name": "start_build",
        "description": (
            "Schedule a build/test/benchmark run. The build runs detached so "
            "it survives MCP server restarts. If a run of the same lock group "
            "or resolved build tree is already in progress (non-SQL benchmarks "
            "may run with tests when their trees differ; "
            "unit+integration and bench-e2e serialize because both install "
            "and restart the shared PostgreSQL extension) "
            "returns the current status instead of starting a duplicate. "
            f"{get_modes_short_documentation()} "
            f"\nNote: If you want to iterate quickly then use build_only parameter."
            f"\n\nBuild tree selection (`debug` parameter, DOC0270/DOC0371):"
            f"\n  - debug=false (DEFAULT): uses the mode family's fast preset "
            f"(core, isolated tests, cache-only, or combined provider). "
            f"-O3 + NDEBUG only, no debug info, no "
            f"per-TU temps. This is the fast everyday build — preferred for "
            f"every normal iteration, including running tests and benches."
            f"\n  - debug=true: uses that family's corresponding isolated "
            f"debug preset. Same -O3 + NDEBUG, "
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
            f"\n\nFocused tests: for mode=unit+integration, pass Catch2 "
            f"test-spec expressions in filter.tags. They are OR-combined and "
            f"applied to both test executables; a binary with no matching test "
            f"is allowed so tags local to one executable work as intended."
            f"\nHard timeout: ~{int(HARD_TIMEOUT_MIN)} min."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "mode": {
                    "type": "string",
                    "enum": sorted(VALID_MODES),
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
                       "Select the mode family's isolated debug build tree "
                       "(core, test, cache-only, or combined). Adds -g + "
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
                        "Mode-dependent subset selection. For unit+integration, only tags is "
                        "accepted: each value is a bounded Catch2 test-spec expression; the "
                        "values are OR-combined and applied to both test executables. For CLI "
                        f"benchmark modes ({', '.join(BENCH_CLI_MODES)}), tags/benchmarks/cases "
                        "map to repeatable --filter flags; arrays are OR-sets and axes AND "
                        "together. Other modes, including CMake-owned private-cluster modes, "
                        "do not accept filter. Omit it for the full run."
                    ),
                    "properties": {
                        "tags":       {"type": "array", "items": {"type": "string", "minLength": 1}, "minItems": 1,
                                       "description": "Unit mode: Catch2 test-spec expressions. Bench modes: benchmark tag OR-set."},
                        "cases":      {"type": "array", "items": {"type": "string", "minLength": 1}, "minItems": 1,
                                       "description": "Within selected benchmarks, run only these matrix.over cases (algorithm names)."},
                        "benchmarks": {"type": "array", "items": {"type": "string", "minLength": 1}, "minItems": 1,
                                       "description": "Run only these benchmark keys. (Maps to the bench CLI's singular `benchmark=`.)"}
                    }
                },
                "bench_e2e_config": {
                    "type": "string",
                    "minLength": 1,
                    "description": (
                        "Local bench-e2e only. Workspace-root-relative or "
                        "absolute path to an existing workspace-contained .json "
                        "configuration. Ordinary benchmark configurations become "
                        "BIOSEQ_E2E_CONFIG_OVERRIDE. A content-sealed "
                        "doc0388_sql_codegen_guardrail_workflow_request instead "
                        "selects the MCP-owned calibration-capture, independent "
                        "baseline-promotion, or candidate-qualification workflow; "
                        "those requests pin their own build/install/report behavior."
                    ),
                },
                "postgres_error_safety_review_waiver": {
                    "type": "string",
                    "minLength": 1,
                    "description": (
                        "Full postgres-error-safety runs only. "
                        "Workspace-root-relative or absolute path to an "
                        "existing workspace-contained, content-sealed "
                        "doc0388_postgres_error_safety_review_waiver JSON "
                        "document. The waiver must explicitly state "
                        "independent_review_performed=false. It authorizes a "
                        "mechanical byte-copy promotion and never creates an "
                        "independent-review claim. build_only rejects it."
                    ),
                },
                "debug_report": {
                    "type": "boolean",
                    "default": False,
                    "description": (
                        "Bench-only. Pass --debug to the bench binary: stream a crash-resilient "
                        "report_debug_<N>.jsonl (one JSON event per case) next to report_<N>.json. "
                        "Requires a non-empty `filter` (debug is for targeted runs). NOTE: this is "
                        "NOT the build-tree `debug` flag above. Supplying this property, even "
                        "as false, is rejected for unit+integration mode."
                    )
                }
            },
            "allOf": [
                {
                    "if": {
                        "properties": {"mode": {"const": "unit+integration"}},
                        "required": ["mode"],
                    },
                    "then": {
                        "not": {"required": ["debug_report"]},
                        "properties": {
                            "filter": {
                                "not": {
                                    "anyOf": [
                                        {"required": ["benchmarks"]},
                                        {"required": ["cases"]},
                                    ]
                                },
                                "properties": {
                                    "tags": {
                                        "maxItems": _MAX_CATCH2_TEST_SPECS,
                                        "items": {
                                            "type": "string",
                                            "minLength": 1,
                                            "maxLength": _MAX_CATCH2_TEST_SPEC_LENGTH,
                                            "pattern": "^(?!-)[^\\u0000-\\u001F\\u007F]+$",
                                        },
                                    }
                                },
                            }
                        },
                    },
                },
                {
                    "if": {
                        "properties": {
                            "mode": {
                                "const": POSTGRES_ERROR_SAFETY_MODE,
                            },
                        },
                        "required": ["mode"],
                    },
                    "then": {
                        "not": {
                            "anyOf": [
                                {"required": ["debug"]},
                                {"required": ["filter"]},
                                {"required": ["debug_report"]},
                                {"required": ["bench_e2e_config"]},
                            ],
                        },
                    },
                },
                {
                    "if": {
                        "properties": {
                            "mode": {
                                "const": POSTGRES_ERROR_SAFETY_MODE,
                            },
                        },
                        "required": ["mode"],
                    },
                    "then": {
                        "if": {
                            "properties": {
                                "build_only": {"const": True},
                            },
                            "required": ["build_only"],
                        },
                        "then": {
                            "not": {
                                "required": [
                                    "postgres_error_safety_review_waiver",
                                ],
                            },
                        },
                        "else": {
                            "required": [
                                "postgres_error_safety_review_waiver",
                            ],
                        },
                    },
                    "else": {
                        "not": {
                            "required": [
                                "postgres_error_safety_review_waiver",
                            ],
                        },
                    },
                },
            ],
            "required": ["mode"],
        },
    },
    {
        "name": "status",
        "description": (
            "Report the status of a previously scheduled run for `mode`. "
            "While running, returns current step + predicted finish. After "
            "completion, returns total duration, log file path, and (for "
            "bench modes) the path to the new framework report_<N>.json under "
            "main/reporting/ (legacy report_build_<N>.json is also accepted). "
            "Focused unit/integration runs also display the "
            "exact Catch2 args persisted for both executables."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "mode": {
                    "type": "string",
                    "enum": sorted(VALID_MODES),
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
            "DOC0270/DOC0371 — fast core/test/cache trees do "
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
                    "enum": sorted(INSPECT_TARGETS),
                    "default": "bioseq_bench",
                    "description":
                        "Which target binary or extension DSO to inspect. The "
                        "target map resolves its core, test, cache-only, or "
                        "combined preset family and binary subdirectory.",
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
                        "target's matching family (DOC0270/DOC0371). Core, "
                        "test, cache-only, and combined targets each retain "
                        "their own fast/debug trees. true selects debug, "
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
        "name": "papers-build",
        "description": (
            "Configure `main/papers/CMakeLists.txt` with CMake and build only "
            "the `bioseqdb_thesis` target sourced from "
            "`main/papers/src/content/bioseqdb/thesis`. The build runs in a "
            "detached worker so a clean TeX environment can finish after the "
            "MCP request returns. Call with action='start' to schedule a new "
            "build (or get the currently-running build), then poll with "
            "action='status'. A completed response includes the generated PDF "
            "path, digest, size, exact CMake commands, log path, and log tail."
        ),
        "inputSchema": {
            "type": "object",
            "additionalProperties": False,
            "properties": {
                "action": {
                    "type": "string",
                    "enum": ["start", "status"],
                    "default": "start",
                    "description": (
                        "Start a new thesis build, or poll the most recently "
                        "scheduled build."
                    ),
                },
            },
            "required": [],
        },
    },
    {
        "name": "papers-inspect",
        "description": (
            "Inspect the PDF produced by papers-build. With no `pages`, renders "
            "up to 80 pages as lightweight overview PNGs and returns them "
            "directly as MCP image content in page order. With `pages=[...]`, "
            "renders up to 20 selected pages at higher resolution for focused "
            "visual review. The leading JSON manifest includes stable local "
            "image paths, PDF metadata, source-staleness detection, LaTeX "
            "overfull/underfull-box and reference warnings, Poppler text "
            "bounding-box checks, empty-text-page hints, and explicit agent "
            "review instructions. Automated diagnostics supplement, but do "
            "not replace, visual confirmation of clipping, overlap, margins, "
            "figure/table placement, blank areas, and broken glyphs."
        ),
        "inputSchema": {
            "type": "object",
            "additionalProperties": False,
            "properties": {
                "pages": {
                    "type": "array",
                    "items": {"type": "integer", "minimum": 1},
                    "minItems": 1,
                    "maxItems": 20,
                    "uniqueItems": True,
                    "description": (
                        "Optional one-based page numbers for focused review. "
                        "Omit to render the whole-document overview."
                    ),
                },
                "dpi": {
                    "type": "integer",
                    "minimum": 36,
                    "maximum": 300,
                    "description": (
                        "Raster resolution. Defaults to 48 for the overview "
                        "and 160 when explicit pages are supplied."
                    ),
                },
                "include_images": {
                    "type": "boolean",
                    "default": True,
                    "description": (
                        "Return rendered PNGs as MCP image content. Stable "
                        "image paths remain in the JSON manifest when false "
                        "or when the 8 MiB response cap is reached."
                    ),
                },
            },
            "required": [],
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
            "  operation='build' — runs any remote-compatible start_build mode "
            f"({', '.join(REMOTE_RUN_BUILD_MODES)}) with the same build_only/debug/"
            "filter/debug_report semantics as the local start_build tool, including "
            "focused Catch2 filter.tags for unit+integration and CMake-owned "
            "private-cluster execution for cache modes.\n"
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
                            "enum": list(REMOTE_RUN_BUILD_MODES),
                            "description": ("Which run to build/execute — same modes "
                                            "as the remote-compatible subset of "
                                            "the local start_build tool."),
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
                                "Mode-dependent subset selection, identical to local "
                                "start_build. unit+integration accepts only tags as "
                                "bounded Catch2 test-spec expressions and applies their "
                                "OR expression to both test executables. CLI benchmark modes "
                                f"({', '.join(BENCH_CLI_MODES)}) translate tags/benchmarks/cases "
                                "to repeatable --filter flags. Other modes, including "
                                "CMake-owned private-cluster modes, reject filter."
                            ),
                            "properties": {
                                "tags": {
                                    "type": "array",
                                    "items": {"type": "string", "minLength": 1},
                                    "minItems": 1,
                                    "description": "Unit mode: Catch2 test-spec expressions. Bench modes: benchmark tag OR-set.",
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
                                "Requires a non-empty filter. Supplying this property is "
                                "rejected for unit+integration mode."
                            ),
                        },
                        "bench_e2e_config": {
                            "type": "string",
                            "minLength": 1,
                            "pattern": "^(?!/)(?!.*(?:^|/)\\.\\.(?:/|$)).+\\.json$",
                            "description": (
                                "bench-e2e only. Workspace-relative JSON path "
                                "present in the pushed branch/on-box checkout. "
                                "Supports ordinary E2E configurations and the "
                                "same content-sealed DOC0388 capture, promotion, "
                                "and environment-specific qualification requests "
                                "as local start_build."
                            ),
                        },
                        **_REMOTE_RUN_COMMON_PROPS,
                    },
                    "allOf": [
                        {
                            "if": {
                                "properties": {"mode": {"const": "unit+integration"}},
                                "required": ["mode"],
                            },
                            "then": {
                                "not": {"required": ["debug_report"]},
                                "properties": {
                                    "filter": {
                                        "not": {
                                            "anyOf": [
                                                {"required": ["benchmarks"]},
                                                {"required": ["cases"]},
                                            ]
                                        },
                                        "properties": {
                                            "tags": {
                                                "maxItems": _MAX_CATCH2_TEST_SPECS,
                                                "items": {
                                                    "type": "string",
                                                    "minLength": 1,
                                                    "maxLength": _MAX_CATCH2_TEST_SPEC_LENGTH,
                                                    "pattern": "^(?!-)[^\\u0000-\\u001F\\u007F]+$",
                                                },
                                            }
                                        },
                                    }
                                },
                            },
                        }
                    ],
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
                            "enum": sorted(INSPECT_TARGETS),
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
        bench_e2e_config = args.get("bench_e2e_config")
        postgres_error_safety_review_waiver = args.get(
            "postgres_error_safety_review_waiver")
        text = tool_start_build(mode, build_only=build_only, debug=debug,
                                bench_filter=bench_filter,
                                debug_report=debug_report,
                                debug_report_supplied="debug_report" in args,
                                debug_supplied="debug" in args,
                                filter_supplied="filter" in args,
                                bench_e2e_config=bench_e2e_config,
                                postgres_error_safety_review_waiver=(
                                    postgres_error_safety_review_waiver))
    elif name == "status":
        mode = args.get("mode", "")
        text = tool_status(mode)
    elif name == "inspect":
        text = tool_inspect(args)
    elif name == "papers-build":
        text = tool_papers_build(args)
    elif name == "papers-inspect":
        return tool_papers_inspect(args)
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
            "and the core/test/cache BIOSEQ_*_BUILD_PRESET variables."
        ),
    )
    parser.add_argument(
        "--run-inspect",
        default=None,
        metavar="JSON",
        help=(
            "(internal) run the inspect tool with the given JSON argument object "
            "and print the JSON result to stdout. Used by remote-run over SSH to "
            "inspect a binary on the box. Honours the core/test/cache "
            "BIOSEQ_*_BUILD_PRESET variables."
        ),
    )
    parser.add_argument(
        "--run-papers-build",
        action="store_true",
        default=False,
        help=(
            "(internal) run the detached CMake worker for the "
            "main/papers bioseqdb_thesis target"
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
        "--test-flags",
        default="[]",
        metavar="JSON",
        help=(
            "(internal) JSON array of already-validated Catch2 argv tokens "
            "appended to every unit+integration qualification executable (for "
            "example [\"--allow-running-no-tests\",\"[index],[postgres]\"]). "
            "unit+integration mode only."
        ),
    )
    parser.add_argument(
        "--bench-e2e-config",
        default="",
        help=(
            "(internal) canonical workspace-contained E2E config path. "
            "Forwarded only to the local bench-e2e executable environment."
        ),
    )
    parser.add_argument(
        "--sql-guardrail-request",
        default="",
        help=(
            "(internal) immutable canonical JSON snapshot of a validated "
            "DOC0388 SQL guardrail workflow request."
        ),
    )
    parser.add_argument(
        "--postgres-error-safety-review-waiver-snapshot",
        default="",
        help=(
            "(internal) immutable canonical JSON snapshot of a validated "
            "DOC0388 PostgreSQL error-safety user review waiver."
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
    if args.run_papers_build:
        return _run_papers_build_worker()
    if args.run_remote is not None:
        return _run_remote_worker(args.run_remote)
    if args.run_build is not None:
        build_only: bool = parse_bool("build_only", args.build_only)
        debug: bool = parse_bool("debug", args.debug)
        try:
            bench_flags = json.loads(args.bench_flags)
            if not isinstance(bench_flags, list) or not all(isinstance(x, str) for x in bench_flags):
                raise ValueError("must be a JSON array of strings")
            test_flags = json.loads(args.test_flags)
            if not isinstance(test_flags, list) or not all(isinstance(x, str) for x in test_flags):
                raise ValueError("--test-flags must be a JSON array of strings")
        except (ValueError, json.JSONDecodeError) as e:
            _log(f"runtime flags invalid: {e}")
            return 2
        return _run_build_worker(args.run_build, build_only=build_only,
                                 debug=debug, bench_flags=bench_flags,
                                 test_flags=test_flags,
                                 bench_e2e_config=(
                                     args.bench_e2e_config or None),
                                 sql_guardrail_request_json=(
                                     args.sql_guardrail_request or None),
                                 postgres_error_safety_review_waiver_json=(
                                     args
                                     .postgres_error_safety_review_waiver_snapshot
                                     or None))
    if args.run_build_sync is not None:
        build_only: bool = parse_bool("build_only", args.build_only)
        debug: bool = parse_bool("debug", args.debug)
        try:
            bench_flags = json.loads(args.bench_flags)
            if not isinstance(bench_flags, list) or not all(isinstance(x, str) for x in bench_flags):
                raise ValueError("must be a JSON array of strings")
            test_flags = json.loads(args.test_flags)
            if not isinstance(test_flags, list) or not all(isinstance(x, str) for x in test_flags):
                raise ValueError("--test-flags must be a JSON array of strings")
        except (ValueError, json.JSONDecodeError) as e:
            _log(f"runtime flags invalid: {e}")
            return 2
        return _run_build_inline(args.run_build_sync, build_only=build_only,
                                 debug=debug, bench_flags=bench_flags,
                                 test_flags=test_flags,
                                 bench_e2e_config=(
                                     args.bench_e2e_config or None),
                                 postgres_error_safety_review_waiver_json=(
                                     args
                                     .postgres_error_safety_review_waiver_snapshot
                                     or None))
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
