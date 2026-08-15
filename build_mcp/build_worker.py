"""Build execution and worker-facing tool operations."""

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
    CMAKE_PRESETS_FILE,
    DEFAULT_BUILD_DIR,
    MODES,
    PACKAGE_LINUX_MODE,
    POSTGRES_ERROR_SAFETY_MODE,
    REPOSITORY_ROOT,
    VALID_MODES,
    _CMAKE_CONFIGURE_STAMP_KIND,
    _CMAKE_CONFIGURE_STAMP_NAME,
    _cmake_cache_bool,
    _cmake_cache_contains,
    _cmake_cache_value,
    _cmake_preset_cache_contract,
    _cmake_preset_cache_repairs,
    _cmake_preset_payload_graph,
    _expand_cmake_preset_include,
    _filter_to_cli_args,
    _is_aws_clang_preset,
    _is_aws_gcc_preset,
    _is_aws_preset,
    _is_debug_preset,
    _resolved_cmake_preset_cache_variables,
    _unit_test_filter_to_cli_args,
    build_dir_for,
    build_dirs_for,
    post_build_targets_for,
    preset_for,
    supports_bench_cli_options,
)
from cache_modes import (
    _CACHE_BIOSEQ_DSO_QUALIFICATION_MODES,
    _CACHE_NATIVE_ARTIFACT_KEYS,
    _CACHE_NATIVE_REPORT_MODES,
    _cache_apply_runtime_budget,
    _cache_bioseq_dso_qualification_evidence,
    _cache_native_artifacts,
    _cache_native_report_contract,
    _cache_command_deadline,
    _report_index,
    _require_cache_cleanup_roots_absent,
    _stable_native_command_identity,
    _validate_failed_cache_terminal_report,
    _verified_native_command_execution,
)
from integrity import (
    canonical_json_sha256,
    sha256_file,
)
from postgres_runtime import (
    _cmake_install_cmd,
    _postgresql_discovery_cache_reset_args,
    _postgresql_e2e_restart_shell_command,
    _prepare_postgresql_test_database,
    _restart_postgresql,
)
from postgres_safety import (
    _checked_postgres_error_safety_case_template,
    _run_postgres_error_safety_worker,
)
from process_state import (
    LOG_DIR,
    REMOTE_STATE_FILE,
    STATE_FILE,
    SUBAGENT_STATE_FILE,
    _ProcessIdentity,
    _await_guarded_worker_arm,
    _build_worker_descendant_environment,
    _capture_process_identity,
    _get_build_receipt,
    _global_lock,
    _guarded_process_snapshot,
    _load_state,
    _load_subagent_state,
    _log,
    _read_activity_registry,
    _refresh,
    _refresh_subagent,
    _require_trusted_root_owned_path,
    _same_process,
    _save_state,
    _save_subagent_state,
    _store_build_receipt,
    _stream_subprocess,
    _subagent_global_lock,
    _tail_path,
    _terminate_unarmed_popen_group,
    _transient_activity,
    _trusted_root_owned_executable,
    _update_entry,
    _update_subagent_entry,
    _valid_detached_worker_identity,
)
from publication import durable_replace_bytes, fsync_directory

import bootstrap
import package_linux
from inspect_tool import (
    run_inspect as _run_inspect_impl, MATCHES_WARN_THRESHOLD,
    MAX_INSPECT_MATCHES, MAX_INSPECT_SNIPPET_LINES,
    MAX_RETAINED_SNIPPET_BYTES, MAX_TOTAL_RETAINED_SNIPPET_BYTES,
)
from sanitizer_runtime import (
    SANITIZER_RUNTIME_ENV, SanitizerRuntimeError,
    postgres_sanitizer_environment, sanitizer_descendant_environment,
)










_CMAKE_HELPER_DIR = REPOSITORY_ROOT / "cmake"

PAPERS_BUILD_STATE_KEY = "papers-build"

PAPERS_DIR = REPOSITORY_ROOT / "main" / "papers"

PAPERS_BUILD_DIR = PAPERS_DIR / "build"

PAPERS_TARGET = "bioseqdb_thesis"

PAPERS_PDF_PATH = PAPERS_DIR / "pdf" / f"{PAPERS_TARGET}.pdf"

PAPERS_LATEX_LOG_PATH = (
    PAPERS_BUILD_DIR / PAPERS_TARGET / "_assembly" / "main.log")

PAPERS_INSPECT_ROOT = Path("/tmp/bioseq_papers_inspect")

PAPERS_BUILD_HARD_TIMEOUT_SEC = 30 * 60.0

PAPERS_INSPECT_MAX_OVERVIEW_PAGES = 80

PAPERS_INSPECT_MAX_EMBED_BYTES = 8 * 1024 * 1024

LOCAL_BUILD_ENVIRONMENT_FILE = Path("/etc/environment")

LOCAL_CLANG_RT_PRIORITY_KEY = "BIOSEQ_BUILD_RT_PRIORITY"

LOCAL_CLANG_RT_PRIORITY = 1

LOCAL_CLANG_RT_LAUNCHER = Path(
    "/usr/local/libexec/bioseq-clang-rt-launcher")

LOCAL_LLVM22_CLANG = Path("/usr/lib/llvm-22/bin/clang")

LOCAL_LLVM22_CLANGXX = Path("/usr/lib/llvm-22/bin/clang++")

HIGHWAY_GCC_CANARY_RUNTIME_ENVIRONMENT = {
    "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
    "LC_ALL": "C",
    "LANG": "C",
}

LOCAL_CLANG_RT_CMAKE_KEYS = (
    "CMAKE_C_COMPILER_LAUNCHER",
    "CMAKE_CXX_COMPILER_LAUNCHER",
)


def _parse_worker_bool(parameter_description: str, value: object) -> bool:
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

    The heavy C++26 template TUs (pipeline/pipe.cppm,
    pg/scheduler/dispatch_buffer/index.cppm,
    pg/storage/manager/sequence_storage.cppm, pipeline/minimap_port.cppm,
    alignment_hwy/ksw2_batched.cppm)
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

MAX_BUILD_PHASE_TIMEOUT_SEC = 55 * 60

BUILD_TIMEOUT_TERM_LEAD_SEC = 60


def _build_phase_deadlines(
        started_at: float, timeout_sec: int) -> tuple[float, float]:
    if (
        not isinstance(timeout_sec, int)
        or isinstance(timeout_sec, bool)
        or not 60 <= timeout_sec <= MAX_BUILD_PHASE_TIMEOUT_SEC
    ):
        raise ValueError("invalid guarded build timeout")
    hard_deadline = started_at + timeout_sec
    return hard_deadline - BUILD_TIMEOUT_TERM_LEAD_SEC, hard_deadline

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

AWS_DEFAULT_VCPKG_TRIPLET = "x64-linux"

AWS_LIBCXX_OVERLAY_TRIPLETS = REPOSITORY_ROOT / "cmake" / "triplets"

AWS_LIBCXX_OVERLAY_PORTS = REPOSITORY_ROOT / "cmake" / "ports"

AWS_LIBCXX_FIND_CACHE_UNSET_PATTERNS = (
    "OPENSSL*",
    "_OPENSSL*",
    "OpenSSL*",
    "PostgreSQL*",
    "FIND_PACKAGE_MESSAGE_DETAILS_OpenSSL",
    "FIND_PACKAGE_MESSAGE_DETAILS_PostgreSQL",
)







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
        "BIOSEQ_TEMPLATE_FLATTEN_PROFILE",
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
        "schema_revision": 1,
        "inputs": inputs,
        "inputs_sha256": hashlib.sha256(canonical_inputs).hexdigest(),
    }

def _cmake_configure_stamp_status(
        build_dir: Path,
        expected: Mapping[str, object]) -> tuple[bool, str]:
    """Return whether an existing tree was configured from ``expected``."""
    required_keys = {
        "artifact_kind", "schema_revision", "inputs", "inputs_sha256",
    }
    if (
        set(expected) != required_keys
        or expected.get("artifact_kind") != _CMAKE_CONFIGURE_STAMP_KIND
        or type(expected.get("schema_revision")) is not int
        or expected.get("schema_revision") != 1
        or not isinstance(expected.get("inputs"), dict)
        or not isinstance(expected.get("inputs_sha256"), str)
    ):
        return False, "expected configure stamp schema is invalid"
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
    if (
        not isinstance(observed, dict)
        or set(observed) != required_keys
        or observed.get("artifact_kind") != _CMAKE_CONFIGURE_STAMP_KIND
        or type(observed.get("schema_revision")) is not int
        or observed.get("schema_revision") != 1
    ):
        return False, "configure stamp schema is invalid"
    if observed != expected:
        return False, "configure inputs changed"
    return True, "configure inputs match"

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
        fsync_directory(build_dir)
    except OSError as exc:
        raise RuntimeError(
            f"cannot invalidate CMake configure stamp {stamp}: {exc}") from exc

def _publish_cmake_configure_stamp(
        build_dir: Path,
        document: Mapping[str, object]) -> None:
    """Atomically publish a retry-safe configure stamp after validation."""
    if (
        set(document) != {
            "artifact_kind", "schema_revision", "inputs", "inputs_sha256",
        }
        or document.get("artifact_kind") != _CMAKE_CONFIGURE_STAMP_KIND
        or type(document.get("schema_revision")) is not int
        or document.get("schema_revision") != 1
    ):
        raise RuntimeError("refusing to publish an invalid configure stamp schema")
    stamp = build_dir / _CMAKE_CONFIGURE_STAMP_NAME
    build_dir.mkdir(parents=True, exist_ok=True)
    body = json.dumps(
        document,
        ensure_ascii=True,
        indent=2,
        sort_keys=True,
    ) + "\n"
    try:
        durable_replace_bytes(stamp, body.encode("utf-8"))
    except OSError as exc:
        with contextlib.suppress(OSError):
            stamp.unlink()
        with contextlib.suppress(OSError):
            fsync_directory(build_dir)
        raise RuntimeError(
            f"cannot publish CMake configure stamp {stamp}: {exc}") from exc

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
        environment: Mapping[str, str],
        hard_kill_deadline: float | None = None,
        state_mode: str | None = None,
        require_verified_termination: bool = False,
        deadline_clock=None) -> int:
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
        cwd=REPOSITORY_ROOT,
        env=dict(environment),
        hard_kill_deadline=hard_kill_deadline,
        state_mode=state_mode,
        phase="configure",
        require_verified_termination=require_verified_termination,
        deadline_clock=deadline_clock,
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
        hard_kill_deadline: float | None = None,
        state_mode: str | None = None,
        require_verified_termination: bool = False,
        deadline_clock=None,
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
        hard_kill_deadline=hard_kill_deadline,
        state_mode=state_mode,
        require_verified_termination=require_verified_termination,
        deadline_clock=deadline_clock,
    )
    if rc != 0:
        raise RuntimeError(f"{failure_label} exited with code {rc}")
    if not build_ninja.is_file():
        raise RuntimeError(
            f"preset {preset!r} did not produce {build_ninja}")
    return True

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
    "1. Read DOCS/CURRENT/GUIDELINES/0321_BIOSEQ_CODE_AND_WORKFLOW_GUIDELINES.md in full\n"
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

SUBAGENT_HARD_TIMEOUT_SEC = 90 * 60.0

@dataclass(frozen=True)
class InspectTargetDefinition:
    mode: str
    artifact_names: tuple[str, ...]
    shared_target_names: tuple[str, ...] = ()
    profiled_artifact_directory: str | None = None

def _native_shared_library_suffix(system_name: str) -> str:
    if system_name == "Darwin":
        return ".dylib"
    if system_name == "Windows":
        return ".dll"
    return ".so"

def _inspect_artifact_candidates(
    definition: InspectTargetDefinition,
    build_dir: Path,
    debug: bool,
    *,
    system_name: str | None = None,
) -> tuple[str, ...]:
    suffix = _native_shared_library_suffix(
        system_name or host_platform.system())
    native_names = tuple(
        name for name in definition.artifact_names
        if Path(name).suffix == suffix
    )
    shared_library_names = tuple(
        name for name in definition.artifact_names
        if Path(name).suffix in {".dylib", ".dll", ".so"}
    )
    if definition.profiled_artifact_directory is None:
        # A DSO target may list candidates for all supported hosts. Never let
        # an old foreign-platform file win merely because it appears first.
        if shared_library_names:
            if not native_names:
                raise RuntimeError(
                    "inspect target has no artifact for native suffix "
                    f"{suffix}")
            non_library_names = tuple(
                name for name in definition.artifact_names
                if name not in shared_library_names
            )
            return non_library_names + native_names
        return definition.artifact_names

    if not native_names:
        raise RuntimeError(
            f"inspect target has no artifact for native suffix {suffix}")
    configuration = (
        _cmake_cache_value(build_dir, "CMAKE_BUILD_TYPE")
        or ("Debug" if debug else "Release")
    )
    if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.+-]*", configuration) is None:
        raise RuntimeError(
            f"inspect build has invalid CMake configuration {configuration!r}")
    profiled_names = tuple(
        f"{definition.profiled_artifact_directory}/{configuration}/{name}"
        for name in native_names
    )
    return profiled_names + native_names

INSPECT_TARGETS: Dict[str, InspectTargetDefinition] = {
    "bioseq_benchmark": InspectTargetDefinition(
        mode="benchmark", artifact_names=("bioseq_benchmark",),
        shared_target_names=("bioseq_modules_nopg",)),
    "bioseq_io_benchmark": InspectTargetDefinition(
        mode="benchmark-io", artifact_names=("bioseq_io_benchmark",),
        shared_target_names=("bioseq_modules_nopg",)),
    "bioseq_unit_tests": InspectTargetDefinition(
        mode="unit-integration", artifact_names=("bioseq_unit_tests",),
        shared_target_names=("bioseq_modules_nopg",)),
    "bioseq_integration_tests": InspectTargetDefinition(
        mode="unit-integration",
        artifact_names=("bioseq_integration_tests",),
        shared_target_names=("bioseq_modules_nopg",)),
    "bioseq_benchmark_e2e_contract_tests": InspectTargetDefinition(
        mode="unit-integration",
        artifact_names=("bioseq_benchmark_e2e_contract_tests",),
        shared_target_names=("bioseq_modules_nopg",)),
    "bioseq_benchmark_framework_contract_tests": InspectTargetDefinition(
        mode="unit-integration",
        artifact_names=("bioseq_benchmark_framework_contract_tests",),
        shared_target_names=("bioseq_modules_nopg",)),
    "postgresql_bioseq": InspectTargetDefinition(
        mode="benchmark-e2e",
        artifact_names=("bioseq.dylib", "bioseq.so", "bioseq.dll"),
        shared_target_names=("bioseq_modules_nopg",)),
    "postgresql_bioseq_cache": InspectTargetDefinition(
        mode="cache-unit",
        artifact_names=("bioseq_cache.dylib", "bioseq_cache.so",
                        "bioseq_cache.dll"),
        shared_target_names=("bioseq_cache_core",),
        profiled_artifact_directory="artifacts/cache_production"),
    "bioseq_cache_unit_tests": InspectTargetDefinition(
        mode="cache-unit", artifact_names=("bioseq_cache_unit_tests",),
        shared_target_names=("bioseq_cache_core",)),
    "bioseq_cache_benchmark": InspectTargetDefinition(
        mode="cache-benchmark", artifact_names=("bioseq_cache_benchmark",),
        shared_target_names=("bioseq_cache_benchmark_support",)),
    "bioseq_cache_bioseq_benchmark": InspectTargetDefinition(
        mode="cache-bioseq-benchmark",
        artifact_names=("bioseq_cache_bioseq_benchmark",),
        shared_target_names=("bioseq_cache_benchmark_support",)),
}

def _build_target_groups_for(mode: str) -> tuple[tuple[str, ...], ...]:
    """Return the CMake invocations that own a mode's ordinary targets."""
    targets = tuple(MODES[mode].targets)
    if not targets:
        return ()
    if MODES[mode].build_targets_together:
        return (targets,)
    return tuple((target,) for target in targets)

def _cmake_build_argv(
        build_dir: Path, target_group: tuple[str, ...]) -> list[str]:
    """Build one non-empty target group under the global memory-safe job cap."""
    if not target_group:
        raise ValueError("CMake build target group must not be empty")
    return [
        "cmake", "--build", str(build_dir), "--target", *target_group,
        "--parallel", str(CMAKE_PARALLEL_JOBS),
    ]

_BUILD_SOURCE_FINGERPRINT_PATHS = tuple(
    path
    for path in package_linux.SOURCE_SNAPSHOT_PATHS
    if path != "DOCS" and not path.startswith("research/")
)

_BUILD_SOURCE_OUTPUT_COMPONENTS = frozenset({
    "__pycache__",
    "build",
    "reporting",
})

_PAPERS_EXCLUDED_PATHSPECS = (
    ":(top,exclude,literal)main/papers",
    ":(top,exclude,glob)main/papers/**",
)

def _source_graph_fingerprint(repository_root: Path) -> str:
    """Hash the checked-out build inputs without hashing generated outputs.

    The package snapshot's explicit lifted-source allowlist is the single
    repository closure authority. This build-receipt view deliberately removes
    documentation and research so their edits cannot invalidate a successful
    production binary receipt. Tracked deletions are represented explicitly,
    while ignored generated output never enters the digest.
    """
    repository_root = repository_root.resolve(strict=True)
    completed = subprocess.run(
        [
            "git", "-C", str(repository_root), "ls-files", "-z",
            "--cached", "--others", "--exclude-standard",
            "--", *_BUILD_SOURCE_FINGERPRINT_PATHS,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=60,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "could not enumerate source graph: "
            + completed.stderr.decode("utf-8", errors="replace").strip())
    digest = hashlib.sha256()
    for raw_path in sorted(completed.stdout.split(b"\0")):
        if not raw_path:
            continue
        relative = raw_path.decode("utf-8", errors="surrogateescape")
        relative_parts = relative.split("/")
        if (
            any(part in _BUILD_SOURCE_OUTPUT_COMPONENTS
                for part in relative_parts)
            or relative.endswith(".pyc")
        ):
            continue
        path = repository_root / relative
        digest.update(relative.encode("utf-8", errors="surrogateescape"))
        digest.update(b"\0")
        try:
            metadata = path.lstat()
        except OSError:
            digest.update(b"<missing>\0")
            continue
        digest.update(f"{stat.S_IMODE(metadata.st_mode):o}".encode("ascii"))
        digest.update(b"\0")
        if stat.S_ISLNK(metadata.st_mode):
            digest.update(b"<symlink>\0")
            digest.update(os.readlink(path).encode(
                "utf-8", errors="surrogateescape"))
            digest.update(b"\0")
            continue
        if not stat.S_ISREG(metadata.st_mode):
            digest.update(b"<nonregular>\0")
            continue
        file_digest = hashlib.sha256()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                file_digest.update(chunk)
        digest.update(file_digest.digest())
        digest.update(b"\0")
    return digest.hexdigest()

def _git_commit(repository_root: Path, ref: str = "HEAD") -> str:
    completed = subprocess.run(
        ["git", "-C", str(repository_root), "rev-parse", "--verify",
         f"{ref}^{{commit}}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        text=True,
        timeout=30,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"cannot resolve git ref {ref!r}: {completed.stderr.strip()}")
    commit = completed.stdout.strip()
    if not re.fullmatch(r"[0-9a-f]{40,64}", commit):
        raise RuntimeError(f"git returned malformed commit id {commit!r}")
    return commit

def _build_configuration_evidence(build_dir: Path) -> dict[str, object]:
    """Return the stable compiler/configuration identity of a built tree."""
    cache_path = build_dir / "CMakeCache.txt"
    ninja_path = build_dir / "build.ninja"
    stamp_path = build_dir / _CMAKE_CONFIGURE_STAMP_NAME
    if not cache_path.is_file() or not ninja_path.is_file():
        raise RuntimeError(
            f"build tree is incomplete and cannot be receipted: {build_dir}")
    cache_keys = (
        "CMAKE_CXX_COMPILER",
        "CMAKE_CXX_COMPILER_ID",
        "CMAKE_CXX_COMPILER_VERSION",
        "CMAKE_CXX_FLAGS",
        "CMAKE_BUILD_TYPE",
        "BIOSEQ_TEST_INDEXES",
        "BIOSEQ_TEMPLATE_FLATTEN_PROFILE",
    )
    return {
        "cmake_cache_sha256": sha256_file(cache_path),
        "build_ninja_sha256": sha256_file(ninja_path),
        "compile_commands_sha256": (
            sha256_file(build_dir / "compile_commands.json")
            if (build_dir / "compile_commands.json").is_file()
            else None
        ),
        "configure_stamp_sha256": (
            sha256_file(stamp_path) if stamp_path.is_file() else None
        ),
        "compiler_contract": {
            key: _cmake_cache_value(build_dir, key)
            for key in cache_keys
        },
    }

def _stable_artifact_digest(path: Path) -> tuple[os.stat_result, str]:
    """Hash one non-symlink artifact and reject replacement while reading."""
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as exc:
        raise RuntimeError(f"cannot open artifact safely: {path}: {exc}") from exc
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode):
            raise RuntimeError(f"artifact is not a regular file: {path}")
        digest = hashlib.sha256()
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
        after = os.fstat(descriptor)
    finally:
        os.close(descriptor)
    try:
        final_path = path.lstat()
    except OSError as exc:
        raise RuntimeError(
            f"artifact disappeared while it was hashed: {path}: {exc}") from exc
    stable_fields_before = (
        before.st_dev, before.st_ino, before.st_mode, before.st_size,
        before.st_mtime_ns,
    )
    stable_fields_after = (
        after.st_dev, after.st_ino, after.st_mode, after.st_size,
        after.st_mtime_ns,
    )
    stable_fields_path = (
        final_path.st_dev, final_path.st_ino, final_path.st_mode,
        final_path.st_size, final_path.st_mtime_ns,
    )
    if (
        stable_fields_before != stable_fields_after
        or stable_fields_after != stable_fields_path
        or stat.S_ISLNK(final_path.st_mode)
    ):
        raise RuntimeError(f"artifact changed while it was hashed: {path}")
    return after, digest.hexdigest()

def _runtime_artifact_evidence(
        mode: str, build_dir: Path) -> list[dict[str, object]]:
    """Seal every directly executed mode command for no-rebuild reuse."""
    bin_dir = build_dir / MODES[mode].binary_subdir
    evidence: list[dict[str, object]] = []
    for command in MODES[mode].commands:
        if not command:
            continue
        path = bin_dir / command[0]
        try:
            metadata, digest = _stable_artifact_digest(path)
        except RuntimeError as exc:
            raise RuntimeError(
                f"required runtime artifact is unavailable: {path}: "
                f"{exc}") from exc
        if not (metadata.st_mode & 0o111):
            raise RuntimeError(
                f"required runtime artifact is not executable: {path}")
        resolved_path = path.resolve(strict=True)
        try:
            resolved_path.relative_to(build_dir.resolve(strict=True))
        except ValueError as exc:
            raise RuntimeError(
                f"runtime artifact escapes the receipted tree: {path}") from exc
        evidence.append({
            "path": str(resolved_path),
            "size": metadata.st_size,
            "sha256": digest,
            "executable": True,
        })
    return evidence

_RECEIPT_SHARED_LIBRARY_SUFFIXES = {".dylib", ".dll", ".so"}

_RECEIPT_EXTENSION_TARGET_NAMES = {
    "postgresql_bioseq": ("bioseq.dylib", "bioseq.dll", "bioseq.so"),
    "postgresql_bioseq_cache": (
        "bioseq_cache.dylib", "bioseq_cache.dll", "bioseq_cache.so"),
}

def _receipt_extension_target_candidates(
        target: str,
        name: str,
        build_dir: Path,
) -> tuple[Path, ...]:
    """Return only CMake-owned output locations for an extension target."""
    if target == "postgresql_bioseq":
        return (build_dir / "bioseq" / name,)
    if target != "postgresql_bioseq_cache":
        return ()

    output_root = (
        build_dir
        / "bioseq_caching"
        / "artifacts"
        / "cache_production"
    )
    current = build_dir
    for part in output_root.relative_to(build_dir).parts:
        current /= part
        try:
            metadata = current.lstat()
        except FileNotFoundError:
            return (output_root / name,)
        except OSError as exc:
            raise RuntimeError(
                f"cannot inspect extension output directory {current}: "
                f"{exc}") from exc
        if not stat.S_ISDIR(metadata.st_mode):
            raise RuntimeError(
                "extension output parent must be a real non-symlink "
                f"directory: {current}")

    configuration = _cmake_cache_value(build_dir, "CMAKE_BUILD_TYPE")
    if configuration is None:
        raise RuntimeError(
            "cannot resolve CMAKE_BUILD_TYPE for extension output receipt")
    output_parent = output_root
    if configuration:
        configuration_path = Path(configuration)
        if (
            configuration_path.is_absolute()
            or configuration_path.parent != Path(".")
            or configuration_path.name != configuration
            or configuration_path.name in {"", ".", ".."}
        ):
            raise RuntimeError(
                f"CMAKE_BUILD_TYPE is not a safe configuration basename: "
                f"{configuration!r}")
        output_parent /= configuration
        try:
            metadata = output_parent.lstat()
        except FileNotFoundError:
            return (output_parent / name,)
        except OSError as exc:
            raise RuntimeError(
                f"cannot inspect extension output directory {output_parent}: "
                f"{exc}") from exc
        if not stat.S_ISDIR(metadata.st_mode):
            raise RuntimeError(
                "extension output configuration must be a real non-symlink "
                f"directory: {output_parent}")
    return (output_parent / name,)

def _is_receipt_shared_library(path: Path) -> bool:
    name = path.name
    return (
        path.suffix.lower() in _RECEIPT_SHARED_LIBRARY_SUFFIXES
        or ".so." in name
    )

def _receipt_lstat_identity(
        metadata: os.stat_result) -> tuple[int, int, int, int, int, int]:
    """Return fields that expose replacement during one receipt capture."""
    return (
        metadata.st_dev,
        metadata.st_ino,
        metadata.st_mode,
        metadata.st_size,
        metadata.st_mtime_ns,
        metadata.st_ctime_ns,
    )

@dataclass(frozen=True)
class _ReceiptSymlinkHop:
    path: Path
    identity: tuple[int, int, int, int, int, int]
    link_target: str

@dataclass(frozen=True)
class _ReceiptVcpkgLibraryAliasSnapshot:
    alias_path: Path
    parent_identities: tuple[
        tuple[Path, tuple[int, int, int, int, int, int]], ...
    ]
    chain: tuple[_ReceiptSymlinkHop, ...]
    terminal_path: Path

def _stable_receipt_symlink(path: Path) -> _ReceiptSymlinkHop:
    """Read one symlink without accepting replacement around ``readlink``."""
    try:
        before = path.lstat()
        if not stat.S_ISLNK(before.st_mode):
            raise RuntimeError(
                f"receipted library alias is not a symlink: {path}")
        link_target = os.readlink(path)
        after = path.lstat()
    except OSError as exc:
        raise RuntimeError(
            f"cannot inspect receipted library alias {path}: {exc}") from exc
    before_identity = _receipt_lstat_identity(before)
    after_identity = _receipt_lstat_identity(after)
    if (
        before_identity != after_identity
        or not stat.S_ISLNK(after.st_mode)
    ):
        raise RuntimeError(
            f"receipted library alias changed while inspected: {path}")
    return _ReceiptSymlinkHop(path, after_identity, link_target)

def _receipt_vcpkg_parent_identities(
        alias_path: Path,
        build_dir: Path,
) -> tuple[tuple[Path, tuple[int, int, int, int, int, int]], ...]:
    """Prove that every lexical parent of an admitted alias is a directory."""
    try:
        relative_parent = alias_path.parent.relative_to(build_dir)
    except ValueError as exc:
        raise RuntimeError(
            f"receipted vcpkg library alias escapes its build tree: "
            f"{alias_path}") from exc
    identities: list[
        tuple[Path, tuple[int, int, int, int, int, int]]
    ] = []
    current = build_dir
    for part in (Path("."), *relative_parent.parts):
        if part != Path("."):
            current /= part
        try:
            metadata = current.lstat()
        except OSError as exc:
            raise RuntimeError(
                f"cannot inspect vcpkg alias parent {current}: {exc}") from exc
        if not stat.S_ISDIR(metadata.st_mode):
            raise RuntimeError(
                "receipted vcpkg library alias has a non-directory or "
                f"symlink parent: {current}")
        identities.append((current, _receipt_lstat_identity(metadata)))
    return tuple(identities)

def _is_receipt_vcpkg_library_parent_path(
        path: Path, build_dir: Path) -> bool:
    """Whether ``path`` is a lexical parent of an admitted alias directory."""
    try:
        parts = path.relative_to(build_dir).parts
    except ValueError:
        return False
    return (
        parts == ("vcpkg_installed",)
        or (
            len(parts) == 2
            and parts[0] == "vcpkg_installed"
            and bool(parts[1])
        )
        or (
            len(parts) == 3
            and parts[0] == "vcpkg_installed"
            and bool(parts[1])
            and parts[2] in {"lib", "debug"}
        )
        or (
            len(parts) == 4
            and parts[0] == "vcpkg_installed"
            and bool(parts[1])
            and parts[2:] == ("debug", "lib")
        )
    )

def _receipt_vcpkg_library_alias_snapshot(
        path: Path,
        build_dir: Path,
) -> Optional[_ReceiptVcpkgLibraryAliasSnapshot]:
    """Capture one narrowly admitted vcpkg shared-library alias chain.

    vcpkg installs versioned shared libraries as relative aliases.  Only its
    exact release/debug library directories are admitted here; commands,
    extension DSOs, install inputs, and aliases elsewhere remain regular-file
    only.  The volatile identities are used to detect races but are not
    serialized into the portable receipt evidence.
    """
    build_dir = build_dir.resolve(strict=True)
    alias_path = Path(os.path.abspath(path))
    try:
        relative = alias_path.relative_to(build_dir)
    except ValueError:
        return None
    parts = relative.parts
    is_release_library = (
        len(parts) == 4
        and parts[0] == "vcpkg_installed"
        and bool(parts[1])
        and parts[2] == "lib"
    )
    is_debug_library = (
        len(parts) == 5
        and parts[0] == "vcpkg_installed"
        and bool(parts[1])
        and parts[2] == "debug"
        and parts[3] == "lib"
    )
    if not (is_release_library or is_debug_library):
        return None

    parent_identities = _receipt_vcpkg_parent_identities(
        alias_path, build_dir)
    current = alias_path
    seen: set[Path] = set()
    chain: list[_ReceiptSymlinkHop] = []
    while True:
        if current in seen:
            raise RuntimeError(
                f"receipted vcpkg library alias cycle at {current}")
        if len(seen) >= 64:
            raise RuntimeError(
                f"receipted vcpkg library alias chain is too long: "
                f"{alias_path}")
        seen.add(current)
        try:
            metadata = current.lstat()
        except OSError as exc:
            raise RuntimeError(
                f"receipted vcpkg library alias is broken at {current}: "
                f"{exc}") from exc
        if not stat.S_ISLNK(metadata.st_mode):
            if current == alias_path:
                raise RuntimeError(
                    f"receipted vcpkg library alias is no longer a symlink: "
                    f"{alias_path}")
            if (
                not stat.S_ISREG(metadata.st_mode)
                or not _is_receipt_shared_library(current)
            ):
                raise RuntimeError(
                    "receipted vcpkg library alias does not terminate at a "
                    f"regular shared library: {current}")
            terminal_path = current
            break

        if not _is_receipt_shared_library(current):
            raise RuntimeError(
                "receipted vcpkg library alias has a non-library hop: "
                f"{current}")
        hop = _stable_receipt_symlink(current)
        target = Path(hop.link_target)
        if (
            not hop.link_target
            or target.is_absolute()
            or target.parent != Path(".")
            or target.name in {"", ".", ".."}
            or target.name != hop.link_target
            or not _is_receipt_shared_library(target)
        ):
            raise RuntimeError(
                "receipted vcpkg library alias target must be a relative "
                f"same-directory shared-library basename: {current} -> "
                f"{hop.link_target!r}")
        chain.append(hop)
        current = current.parent / hop.link_target

    final_parent_identities = _receipt_vcpkg_parent_identities(
        alias_path, build_dir)
    if final_parent_identities != parent_identities:
        raise RuntimeError(
            "receipted vcpkg library alias parent changed while inspected: "
            f"{alias_path}")
    return _ReceiptVcpkgLibraryAliasSnapshot(
        alias_path=alias_path,
        parent_identities=parent_identities,
        chain=tuple(chain),
        terminal_path=terminal_path,
    )

def _receipt_authorized_artifact_evidence(
        mode: str,
        build_dir: Path,
        *,
        debug: bool) -> list[dict[str, object]]:
    """Seal every build-tree artifact a receipt can execute/install/inspect.

    Command binaries alone are insufficient authority: receipt-pinned runs
    may install extension DSOs through CMake, while receipt-pinned inspection
    may consume linked DSOs and pre-LTO ``.s``/``.bc`` listings.  Keep the
    selection intentionally broader than the immediate command list so a
    changed install plan or locally linked dependency also invalidates reuse.
    """
    build_dir = build_dir.resolve(strict=True)
    bin_dir = (build_dir / MODES[mode].binary_subdir).resolve(strict=False)
    selected: set[Path] = set()
    library_aliases: dict[
        Path, _ReceiptVcpkgLibraryAliasSnapshot
    ] = {}

    def select_regular(path: Path, *, required: bool = False) -> bool:
        try:
            metadata = path.lstat()
        except OSError:
            if required:
                raise RuntimeError(
                    f"required receipted artifact is missing: {path}")
            return False
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
            if required:
                raise RuntimeError(
                    "required receipted artifact is not a regular "
                    f"non-symlink file: {path}")
            return False
        resolved = path.resolve(strict=True)
        try:
            resolved.relative_to(build_dir)
        except ValueError as exc:
            raise RuntimeError(
                f"receipted artifact escapes its build tree: {path}") from exc
        selected.add(resolved)
        return True

    def select_vcpkg_library_alias(
            snapshot: _ReceiptVcpkgLibraryAliasSnapshot) -> None:
        """Register every alias hop and its regular terminal exactly once."""
        for index, hop in enumerate(snapshot.chain):
            hop_snapshot = _ReceiptVcpkgLibraryAliasSnapshot(
                alias_path=hop.path,
                parent_identities=snapshot.parent_identities,
                chain=snapshot.chain[index:],
                terminal_path=snapshot.terminal_path,
            )
            previous = library_aliases.get(hop.path)
            if previous is not None and previous != hop_snapshot:
                raise RuntimeError(
                    "receipted vcpkg library alias changed while selected: "
                    f"{hop.path}")
            library_aliases[hop.path] = hop_snapshot
        select_regular(snapshot.terminal_path, required=True)

    # Every directly executed command is mandatory.
    for command in MODES[mode].commands:
        if command:
            select_regular(bin_dir / command[0], required=True)

    # Private cache runners resolve their stage/DSO inputs through one
    # generated manifest.  Receipt-pinned execution must authenticate that
    # indirection and every referenced build-tree file, not merely the runner
    # executable that consumes it.
    cache_artifact_keys = sorted({
        argument[1:-1]
        for command in MODES[mode].commands
        for argument in command[1:]
        if argument.startswith("{")
        and argument.endswith("}")
        and argument != "{report}"
    })
    if cache_artifact_keys:
        cache_artifact_manifest = bin_dir / "cache_native_artifacts.json"
        select_regular(cache_artifact_manifest, required=True)
        cache_artifacts = _cache_native_artifacts(bin_dir)
        for key in cache_artifact_keys:
            value = cache_artifacts[key]
            if not value:
                raise RuntimeError(
                    "cache native artifact manifest has no path for selected "
                    f"mode placeholder: {key}")
            referenced = Path(value)
            directory_key = key.endswith(
                ("_postgres_bin_dir", "_postgres_stage_root"))
            if directory_key:
                try:
                    relative_directory = referenced.relative_to(build_dir)
                except ValueError as exc:
                    raise RuntimeError(
                        "cache native artifact manifest references an invalid "
                        f"build-tree directory: {referenced}") from exc
                current_directory = build_dir
                for component in relative_directory.parts:
                    current_directory /= component
                    try:
                        metadata = current_directory.lstat()
                    except OSError as exc:
                        raise RuntimeError(
                            "cache native artifact manifest directory is not "
                            f"usable: {referenced}") from exc
                    if (
                        stat.S_ISLNK(metadata.st_mode)
                        or not stat.S_ISDIR(metadata.st_mode)
                    ):
                        raise RuntimeError(
                            "cache native artifact manifest directory must "
                            "have only real build-tree directory components: "
                            f"{referenced}")
                continue
            try:
                referenced.resolve(strict=True).relative_to(build_dir)
            except (OSError, ValueError) as exc:
                raise RuntimeError(
                    "cache native artifact manifest references a file "
                    f"outside the receipted build tree: {referenced}") from exc
            select_regular(referenced, required=True)

    # CMake target names commonly coincide with linked output basenames.  A
    # target may also be a phony/custom target, so only the two extension
    # targets have a mandatory native linked output contract.
    for target in MODES[mode].targets:
        select_regular(bin_dir / target)
        extension_names = _RECEIPT_EXTENSION_TARGET_NAMES.get(target)
        if extension_names is None:
            continue
        native_suffix = _native_shared_library_suffix(
            host_platform.system())
        native_names = tuple(
            name for name in extension_names
            if Path(name).suffix == native_suffix)
        found_native = False
        for name in native_names:
            for candidate in _receipt_extension_target_candidates(
                    target, name, build_dir):
                found_native = select_regular(candidate) or found_native
        if not found_native:
            raise RuntimeError(
                f"built target {target!r} has no native {native_suffix} "
                f"artifact below {build_dir}")

    # Seal every install plan/payload and linked library.  This covers the
    # component-recursive cmake_install.cmake graph, generated extension SQL,
    # the cache/provider DSOs, and platform-local helper executables.
    if build_dir.is_dir():
        for path in build_dir.rglob("*"):
            try:
                metadata = path.lstat()
            except OSError as exc:
                raise RuntimeError(
                    f"cannot inspect receipted artifact {path}: {exc}") from exc
            suffix = path.suffix.lower()
            strict_authority_name = (
                path.name == "cmake_install.cmake"
                or suffix in {".control", ".sql"}
                or (debug and suffix in {".bc", ".s"})
            )
            shared_library_name = _is_receipt_shared_library(path)
            authority_name = shared_library_name or strict_authority_name
            if (
                _is_receipt_vcpkg_library_parent_path(path, build_dir)
                and not stat.S_ISDIR(metadata.st_mode)
            ):
                raise RuntimeError(
                    "receipted vcpkg library parent must be a real "
                    f"non-symlink directory: {path}")
            if stat.S_ISLNK(metadata.st_mode):
                if authority_name:
                    snapshot = (
                        _receipt_vcpkg_library_alias_snapshot(path, build_dir)
                        if shared_library_name and not strict_authority_name
                        else None
                    )
                    if snapshot is None:
                        raise RuntimeError(
                            "receipted install/inspect artifact must not be a "
                            f"symlink: {path}")
                    select_vcpkg_library_alias(snapshot)
                continue
            if not stat.S_ISREG(metadata.st_mode):
                continue
            relative_to_bin = False
            try:
                path.relative_to(bin_dir)
                relative_to_bin = True
            except ValueError:
                pass
            is_linked_or_install_payload = (
                shared_library_name
                or path.name == "cmake_install.cmake"
                or suffix in {".control", ".sql"}
                or (
                    relative_to_bin
                    and bool(metadata.st_mode & 0o111)
                )
            )
            is_debug_inspect_input = (
                debug and suffix in {".bc", ".s"}
            )
            if is_linked_or_install_payload or is_debug_inspect_input:
                select_regular(path)

    if not selected:
        raise RuntimeError(
            f"build receipt for mode {mode!r} has no authorized artifacts")
    evidence: list[dict[str, object]] = []
    regular_evidence: dict[Path, dict[str, object]] = {}
    for path in sorted(selected, key=lambda item: str(item)):
        metadata, digest = _stable_artifact_digest(path)
        item: dict[str, object] = {
            "relative_path": str(path.relative_to(build_dir)),
            "size": metadata.st_size,
            "mode": stat.S_IMODE(metadata.st_mode),
            "sha256": digest,
        }
        evidence.append(item)
        regular_evidence[path] = item

    for alias_path in sorted(library_aliases, key=str):
        expected = library_aliases[alias_path]
        before = _receipt_vcpkg_library_alias_snapshot(
            alias_path, build_dir)
        if before is None or before != expected:
            raise RuntimeError(
                "receipted vcpkg library alias changed while sealed: "
                f"{alias_path}")
        target_metadata, target_digest = _stable_artifact_digest(
            before.terminal_path)
        after = _receipt_vcpkg_library_alias_snapshot(alias_path, build_dir)
        if after is None or after != before or after != expected:
            raise RuntimeError(
                "receipted vcpkg library alias changed while sealed: "
                f"{alias_path}")
        target_evidence = regular_evidence.get(before.terminal_path)
        if target_evidence != {
            "relative_path": str(before.terminal_path.relative_to(build_dir)),
            "size": target_metadata.st_size,
            "mode": stat.S_IMODE(target_metadata.st_mode),
            "sha256": target_digest,
        }:
            raise RuntimeError(
                "receipted vcpkg library alias target changed while sealed: "
                f"{before.terminal_path}")
        evidence.append({
            "artifact_kind": "vcpkg-shared-library-alias",
            "relative_path": str(alias_path.relative_to(build_dir)),
            "mode": stat.S_IMODE(before.chain[0].identity[2]),
            "size": before.chain[0].identity[3],
            "link_target": before.chain[0].link_target,
            "symlink_chain": [
                {
                    "relative_path": str(
                        hop.path.relative_to(build_dir)),
                    "mode": stat.S_IMODE(hop.identity[2]),
                    "size": hop.identity[3],
                    "link_target": hop.link_target,
                }
                for hop in before.chain
            ],
            "target_relative_path": str(
                before.terminal_path.relative_to(build_dir)),
            "target_size": target_metadata.st_size,
            "target_mode": stat.S_IMODE(target_metadata.st_mode),
            "target_sha256": target_digest,
        })
    return sorted(evidence, key=lambda item: str(item["relative_path"]))

def _build_artifact_counts(build_dir: Path, mode: str) -> dict[str, int]:
    compile_commands_path = build_dir / "compile_commands.json"
    compile_commands_count = 0
    if compile_commands_path.is_file():
        try:
            commands = json.loads(
                compile_commands_path.read_text(encoding="utf-8"))
            if isinstance(commands, list):
                compile_commands_count = len(commands)
        except (OSError, json.JSONDecodeError):
            compile_commands_count = 0
    generated_root = build_dir / "bioseq" / "generated" / "highway"
    generated_sources = 0
    if generated_root.is_dir():
        generated_sources = sum(
            1
            for path in generated_root.rglob("*")
            if path.is_file() and path.suffix in {".cppm", ".cpp"}
        )
    compiled_objects = 0
    linked_artifacts = 0
    install_scripts = 0
    inspect_listings = 0
    runtime_artifacts = 0
    bin_dir = (build_dir / MODES[mode].binary_subdir).resolve(strict=False)
    if build_dir.is_dir():
        for path in build_dir.rglob("*"):
            try:
                metadata = path.lstat()
            except OSError:
                continue
            if path.is_symlink() or not stat.S_ISREG(metadata.st_mode):
                continue
            suffix = path.suffix.lower()
            if suffix in {".o", ".obj"}:
                compiled_objects += 1
            if _is_receipt_shared_library(path):
                linked_artifacts += 1
            else:
                try:
                    inside_bin = path.resolve(strict=False).is_relative_to(
                        bin_dir)
                except OSError:
                    inside_bin = False
                if inside_bin and bool(metadata.st_mode & 0o111):
                    linked_artifacts += 1
            if path.name == "cmake_install.cmake":
                install_scripts += 1
            if suffix in {".bc", ".s"}:
                inspect_listings += 1
    for command in MODES[mode].commands:
        if not command:
            continue
        path = bin_dir / command[0]
        try:
            metadata = path.lstat()
        except OSError:
            continue
        if not path.is_symlink() and stat.S_ISREG(metadata.st_mode):
            runtime_artifacts += 1
    return {
        "compile_commands": compile_commands_count,
        "generated_highway_sources": generated_sources,
        "compiled_objects": compiled_objects,
        "linked_artifacts": linked_artifacts,
        "install_scripts": install_scripts,
        "inspect_listings": inspect_listings,
        "runtime_artifacts": runtime_artifacts,
        "targets": len(MODES[mode].targets),
    }

def _validate_build_receipt(
        receipt_id: str,
        *,
        expected_mode: str | None = None,
        expected_debug: bool | None = None,
        verify_source: bool = True,
) -> tuple[dict[str, object] | None, str | None]:
    receipt = _get_build_receipt(receipt_id)
    if receipt is None:
        return None, f"unknown build receipt {receipt_id!r}"
    required_keys = {
        "kind", "schema_revision", "build_receipt_id", "status",
        "exit_code", "created_at", "mode", "debug", "fresh", "preset",
        "preset_family", "binary_subdir", "targets", "build_dir",
        "source_repository_root", "source_commit",
        "source_graph_fingerprint",
        "build_timeout_sec", "build_phase_started_at",
        "configure_completed_at", "configure_duration_sec",
        "build_phase_completed_at", "build_phase_duration_sec",
        "target_timings", "termination_evidence", "host",
        "configuration", "runtime_artifacts", "authorized_artifacts",
        "artifact_counts", "content_sha256",
    }
    if set(receipt) != required_keys:
        return None, (
            f"build receipt {receipt_id!r} has an unexpected schema: "
            f"missing={sorted(required_keys - set(receipt))} "
            f"extra={sorted(set(receipt) - required_keys)}")
    if (
        receipt.get("kind") != "bioseq-build-receipt"
        or type(receipt.get("schema_revision")) is not int
        or receipt.get("schema_revision") != 2
        or receipt.get("status") != "completed"
        or receipt.get("exit_code") != 0
    ):
        return None, f"build receipt {receipt_id!r} is not successful authority"
    sealed_receipt = dict(receipt)
    claimed_content_sha256 = sealed_receipt.pop("content_sha256", None)
    if (
        not isinstance(claimed_content_sha256, str)
        or claimed_content_sha256
        != canonical_json_sha256(sealed_receipt)
    ):
        return None, f"build receipt {receipt_id!r} content seal is invalid"
    if expected_mode is not None and receipt.get("mode") != expected_mode:
        return None, (
            f"build receipt mode mismatch: expected {expected_mode!r}, "
            f"got {receipt.get('mode')!r}")
    if not isinstance(receipt.get("debug"), bool):
        return None, "build receipt debug field is not boolean"
    if not isinstance(receipt.get("fresh"), bool):
        return None, "build receipt fresh field is not boolean"
    if expected_debug is not None and receipt["debug"] != expected_debug:
        return None, (
            "build receipt debug-tree mismatch: expected "
            f"{expected_debug}, got {receipt['debug']}")
    if not isinstance(receipt.get("targets"), list) or not all(
            isinstance(target, str) and target
            for target in receipt["targets"]):
        return None, "build receipt targets are malformed"
    if not isinstance(receipt.get("source_repository_root"), str) or not (
            receipt["source_repository_root"]):
        return None, "build receipt source_repository_root is malformed"
    if not isinstance(receipt.get("build_dir"), str) or not receipt[
            "build_dir"]:
        return None, "build receipt build_dir is malformed"
    source_repository_root = Path(receipt["source_repository_root"])
    build_dir = Path(receipt["build_dir"])
    try:
        source_repository_root = source_repository_root.resolve(strict=True)
        build_dir = build_dir.resolve(strict=True)
        build_dir.relative_to(source_repository_root / "build")
    except (OSError, ValueError):
        return None, "build receipt paths are missing or outside the owned tree"
    if verify_source:
        try:
            observed_source = _source_graph_fingerprint(
                source_repository_root)
        except RuntimeError as exc:
            return None, f"cannot validate build receipt source graph: {exc}"
        if observed_source != receipt.get("source_graph_fingerprint"):
            return None, "build receipt source graph no longer matches"
    try:
        observed_commit = _git_commit(source_repository_root)
    except RuntimeError as exc:
        return None, f"cannot validate build receipt commit: {exc}"
    if observed_commit != receipt.get("source_commit"):
        return None, "build receipt source commit no longer matches"
    mode = receipt.get("mode")
    if not isinstance(mode, str) or mode not in MODES:
        return None, "build receipt names an unknown mode"
    if receipt["targets"] != list(MODES[mode].targets):
        return None, "build receipt target set no longer matches the mode"
    try:
        observed_configuration = _build_configuration_evidence(build_dir)
    except RuntimeError as exc:
        return None, str(exc)
    if observed_configuration != receipt.get("configuration"):
        return None, "build receipt compiler/configuration evidence no longer matches"
    try:
        observed_artifacts = _runtime_artifact_evidence(mode, build_dir)
    except RuntimeError as exc:
        return None, str(exc)
    if observed_artifacts != receipt.get("runtime_artifacts"):
        return None, "build receipt runtime artifacts no longer match"
    try:
        observed_authorized_artifacts = (
            _receipt_authorized_artifact_evidence(
                mode,
                build_dir,
                debug=receipt["debug"],
            )
        )
    except RuntimeError as exc:
        return None, str(exc)
    if observed_authorized_artifacts != receipt.get(
            "authorized_artifacts"):
        return None, (
            "build receipt authorized install/inspect artifacts no longer "
            "match")
    if _build_artifact_counts(build_dir, mode) != receipt.get(
            "artifact_counts"):
        return None, "build receipt artifact counts no longer match"
    observed_host = {
        "node": host_platform.node(),
        "system": host_platform.system(),
        "machine": host_platform.machine(),
        "release": host_platform.release(),
    }
    if receipt.get("host") != observed_host:
        return None, "build receipt host identity no longer matches"
    receipt["source_repository_root"] = str(source_repository_root)
    receipt["build_dir"] = str(build_dir)
    return receipt, None

def _seal_successful_build_receipt(
        *,
        mode: str,
        entry: Mapping[str, object],
        build_dir: Path,
        preset: str,
        debug: bool,
) -> dict[str, object]:
    receipt_id = entry.get("pending_build_receipt_id")
    if not isinstance(receipt_id, str) or not receipt_id:
        raise RuntimeError("build-only run has no reserved receipt id")
    source_fingerprint = _source_graph_fingerprint(REPOSITORY_ROOT)
    if source_fingerprint != entry.get("source_graph_fingerprint"):
        raise RuntimeError(
            "source graph changed during build; refusing to seal receipt")
    source_commit = _git_commit(REPOSITORY_ROOT)
    if source_commit != entry.get("source_commit"):
        raise RuntimeError(
            "source commit changed during build; refusing to seal receipt")
    receipt: dict[str, object] = {
        "kind": "bioseq-build-receipt",
        "schema_revision": 2,
        "build_receipt_id": receipt_id,
        "status": "completed",
        "exit_code": 0,
        "created_at": time.time(),
        "mode": mode,
        "debug": debug,
        "fresh": bool(entry.get("fresh")),
        "preset": preset,
        "preset_family": MODES[mode].preset_family,
        "binary_subdir": MODES[mode].binary_subdir,
        "targets": list(MODES[mode].targets),
        "build_dir": str(build_dir.resolve(strict=True)),
        "source_repository_root": str(REPOSITORY_ROOT.resolve(strict=True)),
        "source_commit": source_commit,
        "source_graph_fingerprint": source_fingerprint,
        "build_timeout_sec": entry.get("build_timeout_sec"),
        "build_phase_started_at": entry.get("build_phase_started_at"),
        "configure_completed_at": entry.get("configure_completed_at"),
        "configure_duration_sec": entry.get("configure_duration_sec"),
        "build_phase_completed_at": entry.get("build_phase_completed_at"),
        "build_phase_duration_sec": entry.get("build_phase_duration_sec"),
        "target_timings": entry.get("target_timings") or [],
        "termination_evidence": entry.get("termination_evidence"),
        "host": {
            "node": host_platform.node(),
            "system": host_platform.system(),
            "machine": host_platform.machine(),
            "release": host_platform.release(),
        },
        "configuration": _build_configuration_evidence(build_dir),
        "runtime_artifacts": _runtime_artifact_evidence(mode, build_dir),
        "authorized_artifacts": _receipt_authorized_artifact_evidence(
            mode, build_dir, debug=debug),
        "artifact_counts": _build_artifact_counts(build_dir, mode),
    }
    receipt["content_sha256"] = canonical_json_sha256(receipt)
    _store_build_receipt(receipt)
    return receipt

def _seal_run_receipt(
        *,
        mode: str,
        build_receipt: Mapping[str, object],
        report_files: list[Path],
        bench_flags: list[str],
        test_flags: list[str],
        bench_e2e_config: str | None,
) -> dict[str, object]:
    report_evidence: list[dict[str, object]] = []
    for path in report_files:
        resolved = path.resolve(strict=True)
        metadata = resolved.stat()
        report_evidence.append({
            "path": str(resolved),
            "size": metadata.st_size,
            "sha256": sha256_file(resolved),
        })
    config_evidence = None
    if bench_e2e_config is not None:
        config_path = Path(bench_e2e_config).resolve(strict=True)
        config_evidence = {
            "path": str(config_path),
            "sha256": sha256_file(config_path),
        }
    receipt: dict[str, object] = {
        "kind": "bioseq-run-receipt",
        "schema_revision": 1,
        "build_receipt_id": f"rr_{secrets.token_hex(16)}",
        "status": "completed",
        "exit_code": 0,
        "created_at": time.time(),
        "mode": mode,
        "reused_build_receipt": build_receipt["build_receipt_id"],
        "reused_build_receipt_content_sha256": build_receipt[
            "content_sha256"],
        "source_commit": build_receipt["source_commit"],
        "source_graph_fingerprint": build_receipt[
            "source_graph_fingerprint"],
        "host": build_receipt["host"],
        "runtime_artifacts": build_receipt["runtime_artifacts"],
        "authorized_artifacts_sha256": canonical_json_sha256(
            build_receipt["authorized_artifacts"]),
        "bench_flags": list(bench_flags),
        "test_flags": list(test_flags),
        "bench_e2e_config": config_evidence,
        "reports": report_evidence,
    }
    receipt["content_sha256"] = canonical_json_sha256(receipt)
    _store_build_receipt(receipt)
    return receipt


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
    return max(new, key=lambda path: _report_index(path.name))

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
        # build's bin directory. Args after the first slot are passed
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
        command_deadline = _cache_command_deadline(
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
                    command_deadline,
                    cwd=active_build_dir,
                    env=run_env,
                    verified_executable=verified_executable,
                )
        else:
            try:
                rc = _stream_subprocess(
                    full_command,
                    log_handle,
                    command_deadline,
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
            if mode in _CACHE_NATIVE_REPORT_MODES:
                report_contract = _cache_native_report_contract(
                    command_report_path,
                    test_flags=test_flags or (),
                )
                reported_arguments = report_contract.get("arguments")
                if (
                    reported_arguments
                    and reported_arguments != full_command
                ):
                    raise RuntimeError(
                        "native cache report argv differs from the exact "
                        "builder invocation")
                _require_cache_cleanup_roots_absent(
                    [Path(path) for path in report_contract["cleanup_roots"]],
                    "native cache report cleanup root",
                )
            command_report_files.append(command_report_path)
            command_report_invocations.append(list(full_command))
            ordered_receipts = [
                str(path) for path in command_report_files
            ]
            _update_entry(
                mode,
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
                and mode not in _CACHE_NATIVE_REPORT_MODES
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
    elif mode in CATCH_FILTER_MODES and test_flags:
        # Every qualification executable receives the same Catch2 test-spec
        # and allow-no-tests policy. A focused tag may live in only one binary.
        full_command += list(test_flags)
    return full_command

def _run_package_linux_worker(entry: Mapping[str, object]) -> int:
    """Run the fixed local package qualifier outside the CMake mode path."""
    mode = PACKAGE_LINUX_MODE
    started_at = float(entry["started_at"])
    deadline = started_at + min(
        HARD_TIMEOUT_SEC,
        float(MODES[mode].hard_timeout_min or HARD_TIMEOUT_MIN) * 60.0,
    )
    log_path = Path(str(entry["log_file"]))
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_handle = log_path.open("w", buffering=1)
    log_handle.write(
        f"# bioseq package worker — mode={mode} pid={os.getpid()} "
        f"started={time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(started_at))}\n"
        f"# repository_root={REPOSITORY_ROOT}\n"
        f"# package_state_root={REPOSITORY_ROOT / 'build' / mode}\n"
        "# profile=debian:trixie postgresql=17 architecture=arm64\n"
    )
    log_handle.flush()
    bootstrap_log_value = entry.get("bootstrap_log_file")
    if isinstance(bootstrap_log_value, str) and bootstrap_log_value:
        try:
            Path(bootstrap_log_value).unlink(missing_ok=True)
        except OSError:
            pass
        _update_entry(mode, bootstrap_log_file=None)

    def progress(step: str) -> None:
        _update_entry(mode, step=step)
        log_handle.write(f"# step={step}\n")
        log_handle.flush()

    try:
        result = package_linux.run_package_linux(
            repository_root=REPOSITORY_ROOT,
            log_handle=log_handle,
            deadline=deadline,
            progress=progress,
        )
        completed_at = time.time()
        receipt = str(result["receipt_path"])
        _update_entry(
            mode,
            status="completed",
            step="completed",
            completed_at=completed_at,
            exit_code=0,
            report_files=[receipt],
            package_linux_qualification=dict(result),
            build_only=False,
            debug=False,
        )
        log_handle.write(
            "# package qualification completed: "
            + json.dumps(result, sort_keys=True) + "\n")
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
            build_only=False,
            debug=False,
        )
        log_handle.write(f"# package qualification timed out: {exc}\n")
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
            build_only=False,
            debug=False,
        )
        log_handle.write(f"# package qualification failed: {exc}\n")
        return 1
    finally:
        log_handle.close()

_RUN_ONLY_RECEIPT_MODES = {
    *CATCH_FILTER_MODES,
    "benchmark",
    "benchmark-fast",
    "benchmark-io",
    "benchmark-e2e",
}

def _build_receipt_required(mode: str, build_only: bool) -> bool:
    # The specialized two-tree PostgreSQL error-safety worker owns separate
    # qualification receipts and bypasses the ordinary single-tree sealer.
    return build_only and mode != POSTGRES_ERROR_SAFETY_MODE

def _run_only_from_build_receipt(
        *,
        mode: str,
        entry: Mapping[str, object],
        receipt: Mapping[str, object],
        active_build_dir: Path,
        active_bin_dir: Path,
        active_preset: str,
        bench_flags: list[str],
        test_flags: list[str],
        bench_e2e_config: str | None,
        log_handle,
        deadline: float,
) -> int:
    """Run install/tests/benchmarks from a validated tree without building."""
    mode_details = MODES[mode]
    if mode not in _RUN_ONLY_RECEIPT_MODES:
        raise RuntimeError(
            f"mode {mode!r} does not support receipt-pinned run_only")
    if mode_details.post_build_targets:
        raise RuntimeError(
            "run_only cannot invoke a CMake post-build target because that "
            "could compile implicitly")
    env = _build_worker_descendant_environment()
    env.pop("BIOSEQ_E2E_CONFIG_OVERRIDE", None)
    _ensure_local_toolchain_environment(env, active_build_dir)
    if _is_aws_clang_preset(active_preset):
        env["CMAKE_NAMED_ENV_TYPE"] = "AWS_MACHINE_X64"
    elif _is_aws_gcc_preset(active_preset):
        env["CMAKE_NAMED_ENV_TYPE"] = "AWS_MACHINE_X64_GCC16"

    pre_reports: set[Path] = set()
    if (
        mode_details.reporting_subdir_name
        and mode_details.reporting_dir.exists()
    ):
        pre_reports = _report_paths(mode_details.reporting_dir)

    if mode_details.install_extension:
        install_receipt, install_receipt_error = _validate_build_receipt(
            str(receipt["build_receipt_id"]),
            expected_mode=mode,
            expected_debug=bool(receipt["debug"]),
        )
        if install_receipt_error is not None or install_receipt is None:
            raise RuntimeError(
                "build receipt changed before extension install: "
                f"{install_receipt_error}")
        if not install_receipt.get("authorized_artifacts"):
            raise RuntimeError(
                "build receipt has no sealed extension install artifacts")
        _update_entry(mode, step="installing receipted extension")
        rc = _stream_subprocess(
            _cmake_install_cmd(active_build_dir, active_preset),
            log_handle,
            deadline,
            cwd=REPOSITORY_ROOT,
            env=env,
            state_mode=mode,
            phase="run_only_install",
        )
        if rc != 0:
            raise RuntimeError(
                "receipt-pinned extension install exited with code "
                f"{rc}")
        _restart_postgresql(
            active_build_dir, active_preset, log_handle, deadline)
        _prepare_postgresql_test_database(
            active_build_dir, active_preset, log_handle, deadline, env=env)

    _update_entry(mode, step="running from build receipt")
    run_env = env
    if mode == "benchmark-e2e":
        env.setdefault(
            "BIOSEQ_E2E_PG_RESTART_CMD",
            _postgresql_e2e_restart_shell_command(
                active_build_dir, active_preset),
        )
        if bench_e2e_config is not None:
            run_env = env.copy()
            run_env["BIOSEQ_E2E_CONFIG_OVERRIDE"] = bench_e2e_config
    report_path, report_files, _, _ = _run_mode_commands(
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
    revalidated_receipt, receipt_error = _validate_build_receipt(
        str(receipt["build_receipt_id"]),
        expected_mode=mode,
        expected_debug=bool(receipt["debug"]),
    )
    if receipt_error is not None or revalidated_receipt is None:
        raise RuntimeError(
            "build receipt changed during run_only execution: "
            f"{receipt_error}")
    run_receipt = _seal_run_receipt(
        mode=mode,
        build_receipt=revalidated_receipt,
        report_files=report_files,
        bench_flags=bench_flags,
        test_flags=test_flags,
        bench_e2e_config=bench_e2e_config,
    )
    completed_at = time.time()
    _update_entry(
        mode,
        status="completed",
        step="completed",
        completed_at=completed_at,
        exit_code=0,
        report_files=[str(path) for path in report_files],
        build_only=False,
        run_only=True,
        reused_build_receipt=receipt["build_receipt_id"],
        run_receipt_id=run_receipt["build_receipt_id"],
    )
    log_handle.write(
        "\n# receipt-pinned run_only completed without configure/generate/"
        "compile/link at "
        f"{time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(completed_at))}\n"
    )
    return 0

def _postgres_safety_services() -> dict[str, object]:
    return {
        "clang_rt_cmake_cache_repairs": _clang_rt_cmake_cache_repairs,
        "cmake_configure_stamp_document": _cmake_configure_stamp_document,
        "cmake_executable_identity": _cmake_executable_identity,
        "configured_local_build_rt_priority": _configured_local_build_rt_priority,
        "ensure_cmake_preset_tree_fresh": _ensure_cmake_preset_tree_fresh,
        "ensure_local_toolchain_environment": _ensure_local_toolchain_environment,
        "finalize_cmake_configure_stamp": _finalize_cmake_configure_stamp,
        "initial_clang_rt_cmake_args": _initial_clang_rt_cmake_args,
        "local_clang_rt_is_enabled": _local_clang_rt_is_enabled,
        "run_cmake_preset_configure": _run_cmake_preset_configure,
        "cmake_parallel_jobs": CMAKE_PARALLEL_JOBS,
        "hard_timeout_min": HARD_TIMEOUT_MIN,
        "hard_timeout_sec": HARD_TIMEOUT_SEC,
    }


def _run_build_worker(mode: str, build_only: bool = False,
                      debug: bool = False, bench_flags=None,
                      test_flags=None, bench_e2e_config=None,
                      run_only: bool = False,
                      reuse_build_receipt: str | None = None,
                      build_timeout_sec: int | None = None,
                      fresh: bool = False) -> int:
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
    a Catch correctness mode for a focused test run.

    `bench_e2e_config` is an absolute, server-validated path inside this
    repository and is exposed to the benchmark-e2e executable.
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
    if build_timeout_sec is not None and (
        not isinstance(build_timeout_sec, int)
        or isinstance(build_timeout_sec, bool)
        or not 60 <= build_timeout_sec <= MAX_BUILD_PHASE_TIMEOUT_SEC
    ):
        _log("worker received invalid build_timeout_sec")
        return 1
    try:
        entry = (
            _await_guarded_worker_arm(mode)
            if build_timeout_sec is not None
            else _load_state().get(mode)
        )
    except RuntimeError as exc:
        _log(f"guarded worker arm failed: {exc}")
        try:
            _update_entry(
                mode,
                status="failed",
                step="worker_identity_failed",
                completed_at=time.time(),
                notes=str(exc),
            )
        except RuntimeError:
            pass
        return 1
    if not entry:
        _log(f"worker for {mode}: no state entry, exiting")
        return 1
    reserved_arguments = {
        "build_only": build_only,
        "debug": debug,
        "fresh": fresh,
        "run_only": run_only,
        "build_timeout_sec": build_timeout_sec,
        "reuse_build_receipt": reuse_build_receipt,
    }
    for field_name, observed_value in reserved_arguments.items():
        if field_name in entry and entry.get(field_name) != observed_value:
            _log(
                "detached worker arguments differ from the reserved request: "
                f"{field_name} scheduled={entry.get(field_name)!r} "
                f"worker={observed_value!r}")
            return 1
    mode_details = MODES[mode]
    if mode_details.requires_build_only and (
        not build_only
        or run_only
        or debug
        or bench_flags
        or test_flags
        or bench_e2e_config
    ):
        _log(
            f"{mode} worker requires build_only=true and rejects every "
            "runtime-selection input")
        return 1
    if mode == PACKAGE_LINUX_MODE:
        if (
            build_only or run_only or fresh or build_timeout_sec is not None
            or reuse_build_receipt is not None
            or debug or bench_flags or test_flags
            or bench_e2e_config
        ):
            _log("package-linux worker received forbidden variable inputs")
            return 1
        return _run_package_linux_worker(entry)
    if mode == POSTGRES_ERROR_SAFETY_MODE:
        if run_only or fresh or build_timeout_sec is not None:
            _log(
                "postgres-error-safety does not accept receipt/fresh inputs")
            return 1
        return _run_postgres_error_safety_worker(
            entry,
            build_only,
            debug,
            bench_flags,
            test_flags,
            bench_e2e_config,
            _postgres_safety_services(),
        )

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
    if build_timeout_sec is not None:
        build_soft_deadline, build_hard_deadline = (
            _build_phase_deadlines(time.monotonic(), build_timeout_sec)
        )
        build_deadline_clock = time.monotonic
    else:
        build_hard_deadline = deadline
        build_soft_deadline = deadline
        build_deadline_clock = time.time

    active_preset = str(entry.get("preset") or preset_for(mode, debug))
    active_build_dir = Path(
        str(entry.get("build_dir") or build_dir_for(mode, debug))
    ).resolve(strict=False)
    active_bin_dir = active_build_dir / mode_details.binary_subdir
    recorded_repository_root = Path(
        str(entry.get("source_repository_root") or REPOSITORY_ROOT)
    ).resolve(strict=False)
    if recorded_repository_root != REPOSITORY_ROOT.resolve(strict=False):
        _log(
            "worker repository-root override does not match the reserved "
            f"source tree: worker={REPOSITORY_ROOT} "
            f"reserved={recorded_repository_root}")
        return 1

    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_handle = log_path.open("w", buffering=1)
    log_handle.write(
        f"# bioseq build worker — mode={mode} pid={os.getpid()} "
        f"started={time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(started_at))}\n"
        f"# repository_root={REPOSITORY_ROOT}\n"
        f"# build_dir={active_build_dir}\n"
        f"# preset={active_preset}\n"
        f"# debug={debug}\n"
        f"# fresh={fresh}\n"
        f"# run_only={run_only}\n"
        f"# reuse_build_receipt={reuse_build_receipt or ''}\n"
        f"# build_timeout_sec={build_timeout_sec or ''}\n"
        f"# build_soft_timeout_at={build_soft_deadline}\n"
        f"# build_hard_timeout_at={build_hard_deadline}\n"
        f"# bench_flags={json.dumps(bench_flags, ensure_ascii=True)}\n"
        f"# focused_test_args={json.dumps(test_flags, ensure_ascii=True)}\n"
        f"# bench_e2e_config={bench_e2e_config or ''}\n"
        f"# clang_rt_priority={entry.get('rt_priority')}\n"
        f"# hard_timeout_min={mode_timeout_sec / 60.0}\n"
    )
    log_handle.flush()
    bootstrap_log_value = entry.get("bootstrap_log_file")
    if isinstance(bootstrap_log_value, str) and bootstrap_log_value:
        try:
            Path(bootstrap_log_value).unlink(missing_ok=True)
        except OSError:
            pass
        _update_entry(mode, bootstrap_log_file=None)

    if mode_details.uses_private_cluster and mode_details.install_extension:
        raise AssertionError(
            "private-cluster mode reached shared extension install routing")

    pre_reports: set[Path] = set()
    if (mode_details.reporting_subdir_name
            and mode_details.reporting_dir.exists()):
        pre_reports = _report_paths(mode_details.reporting_dir)

    try:
        if run_only:
            if not reuse_build_receipt:
                raise RuntimeError(
                    "run_only requires reuse_build_receipt")
            receipt, receipt_error = _validate_build_receipt(
                reuse_build_receipt,
                expected_mode=mode,
                expected_debug=debug,
            )
            if receipt_error is not None or receipt is None:
                raise RuntimeError(
                    f"receipt validation failed: {receipt_error}")
            if Path(str(receipt["build_dir"])) != active_build_dir:
                raise RuntimeError(
                    "reserved run_only tree differs from the receipt tree")
            return _run_only_from_build_receipt(
                mode=mode,
                entry=entry,
                receipt=receipt,
                active_build_dir=active_build_dir,
                active_bin_dir=active_bin_dir,
                active_preset=active_preset,
                bench_flags=bench_flags,
                test_flags=test_flags,
                bench_e2e_config=bench_e2e_config,
                log_handle=log_handle,
                deadline=deadline,
            )

        if reuse_build_receipt is not None:
            raise RuntimeError(
                "reuse_build_receipt is valid only with run_only=true")
        expected_source_fingerprint = entry.get("source_graph_fingerprint")
        if expected_source_fingerprint is not None:
            observed_source_fingerprint = _source_graph_fingerprint(
                REPOSITORY_ROOT)
            if observed_source_fingerprint != expected_source_fingerprint:
                raise RuntimeError(
                    "source graph changed between scheduling and worker start")
        env = _build_worker_descendant_environment()
        # Never let ambient MCP-server state select an E2E configuration. The
        # explicit start_build argument below is the sole supported override.
        env.pop("BIOSEQ_E2E_CONFIG_OVERRIDE", None)
        env["CMAKE_EXPORT_COMPILE_COMMANDS"] = "1"
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
        isolated_tree_args = (
            ["-S", str(REPOSITORY_ROOT), "-B", str(active_build_dir)]
            if fresh else []
        )
        guarded_build = build_timeout_sec is not None
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
        build_phase_started_at = time.time()
        build_phase_started_monotonic = time.monotonic()
        if build_timeout_sec is not None:
            build_soft_deadline, build_hard_deadline = (
                _build_phase_deadlines(
                    build_phase_started_monotonic, build_timeout_sec)
            )
            build_soft_deadline_wall, build_hard_deadline_wall = (
                _build_phase_deadlines(
                    build_phase_started_at, build_timeout_sec)
            )
            _update_entry(
                mode,
                build_phase_started_at=build_phase_started_at,
                build_phase_started_monotonic=build_phase_started_monotonic,
                build_soft_deadline=build_soft_deadline_wall,
                build_hard_deadline=build_hard_deadline_wall,
                build_soft_deadline_monotonic=build_soft_deadline,
                build_hard_deadline_monotonic=build_hard_deadline,
                timeout_guard_armed=True,
            )
            log_handle.write(
                "# guarded build phase armed: "
                f"TERM-at={build_soft_deadline_wall} "
                f"KILL-by={build_hard_deadline_wall} "
                f"TERM-monotonic={build_soft_deadline} "
                f"KILL-monotonic={build_hard_deadline}\n")
            log_handle.flush()
        else:
            _update_entry(
                mode,
                build_phase_started_at=build_phase_started_at,
                timeout_guard_armed=False,
            )
        configured_fresh = _ensure_cmake_preset_tree_fresh(
            build_dir=active_build_dir,
            preset=active_preset,
            configure_stamp=configure_stamp,
            configure_arguments=[
                "cmake",
                "--preset",
                active_preset,
                *isolated_tree_args,
                *required_cache_args,
                *initial_clang_rt_args,
            ],
            log_handle=log_handle,
            deadline=build_soft_deadline,
            environment=env,
            hard_kill_deadline=build_hard_deadline,
            state_mode=mode,
            require_verified_termination=guarded_build,
            deadline_clock=build_deadline_clock,
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
                 *isolated_tree_args,
                 *required_cache_args, *initial_clang_rt_args],
                build_dir=active_build_dir,
                log_handle=log_handle,
                deadline=build_soft_deadline,
                environment=env,
                hard_kill_deadline=build_hard_deadline,
                state_mode=mode,
                require_verified_termination=guarded_build,
                deadline_clock=build_deadline_clock,
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
                 *isolated_tree_args,
                 *postgresql_reset_args,
                 *(f"-D{name}={value}" for name, value in cache_repairs.items())],
                build_dir=active_build_dir,
                log_handle=log_handle,
                deadline=build_soft_deadline,
                environment=env,
                hard_kill_deadline=build_hard_deadline,
                state_mode=mode,
                require_verified_termination=guarded_build,
                deadline_clock=build_deadline_clock,
            )
            if rc != 0:
                raise RuntimeError(
                    f"mode cache repair exited with code {rc}")

        if _is_aws_preset(active_preset):
            if (
                _is_aws_clang_preset(active_preset)
                and _cmake_cache_contains(
                    active_build_dir,
                    f"/vcpkg_installed/{AWS_DEFAULT_VCPKG_TRIPLET}/",
                )
            ):
                _clear_cmake_configure_cache(
                    active_build_dir,
                    log_handle,
                    f"{AWS_DEFAULT_VCPKG_TRIPLET} package paths remain "
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
                    f"/vcpkg_installed/{AWS_DEFAULT_VCPKG_TRIPLET}/" in current_openssl_details
                    and f"/vcpkg_installed/{AWS_LIBCXX_VCPKG_TRIPLET}/"
                    not in current_openssl_details
                ):
                    repair_reasons.append(
                        "clearing stale OpenSSL/PostgreSQL find-package cache "
                        f"from {AWS_DEFAULT_VCPKG_TRIPLET}")
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
                     *isolated_tree_args,
                     *dict.fromkeys(repair_unset_args), *repair_args],
                    build_dir=active_build_dir,
                    log_handle=log_handle,
                    deadline=build_soft_deadline,
                    environment=env,
                    hard_kill_deadline=build_hard_deadline,
                    state_mode=mode,
                    require_verified_termination=guarded_build,
                    deadline_clock=build_deadline_clock,
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
                ["cmake", "--preset", active_preset,
                 *isolated_tree_args, *clang_rt_repairs],
                build_dir=active_build_dir,
                log_handle=log_handle,
                deadline=build_soft_deadline,
                environment=env,
                hard_kill_deadline=build_hard_deadline,
                state_mode=mode,
                require_verified_termination=guarded_build,
                deadline_clock=build_deadline_clock,
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
        configure_completed_at = time.time()
        _update_entry(
            mode,
            configure_completed_at=configure_completed_at,
            configure_duration_sec=(
                configure_completed_at - build_phase_started_at),
        )

        _update_entry(mode, step="building", build_only=build_only,
                      debug=debug,
                      bioseq_test_indexes=verified_test_indexes)
        target_timings: list[dict[str, object]] = []
        for target_group in _build_target_groups_for(mode):
            target_label = (
                target_group[0]
                if len(target_group) == 1
                else " + ".join(target_group)
            )
            target_started_at = time.time()
            rc = _stream_subprocess(
                _cmake_build_argv(active_build_dir, target_group),
                log_handle, build_soft_deadline,
                cwd=REPOSITORY_ROOT,
                env=env,
                hard_kill_deadline=build_hard_deadline,
                state_mode=mode,
                phase=f"build:{target_label}",
                require_verified_termination=guarded_build,
                deadline_clock=build_deadline_clock,
            )
            if rc != 0:
                if len(target_group) == 1:
                    raise RuntimeError(
                        f"build of target '{target_label}' exited with code {rc}")
                raise RuntimeError(
                    "build of concurrent target group "
                    f"'{target_label}' exited with code {rc}")
            target_completed_at = time.time()
            target_timings.append({
                "target": target_label,
                "targets": list(target_group),
                "co_scheduled": len(target_group) > 1,
                "started_at": target_started_at,
                "completed_at": target_completed_at,
                "duration_sec": target_completed_at - target_started_at,
            })
            _update_entry(mode, target_timings=list(target_timings))

        build_phase_completed_at = time.time()
        _update_entry(
            mode,
            build_phase_completed_at=build_phase_completed_at,
            build_phase_duration_sec=(
                build_phase_completed_at - build_phase_started_at),
            target_timings=target_timings,
        )

        if mode in _CACHE_BIOSEQ_DSO_QUALIFICATION_MODES:
            _update_entry(
                mode,
                step="validating production DSO qualification receipt",
            )
            qualification_evidence = (
                _cache_bioseq_dso_qualification_evidence(
                    active_bin_dir,
                    expected_build_dir=active_build_dir,
                    expected_preset=active_preset,
                )
            )
            _update_entry(
                mode,
                production_dso_qualification=qualification_evidence,
            )

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
            receipt_entry = dict(_load_state().get(mode) or entry)
            receipt = (
                _seal_successful_build_receipt(
                    mode=mode,
                    entry=receipt_entry,
                    build_dir=active_build_dir,
                    preset=active_preset,
                    debug=debug,
                )
                if entry.get("receipt_required") else None
            )
            completed_at = time.time()
            _update_entry(
                mode,
                status="completed",
                step="completed",
                completed_at=completed_at,
                exit_code=0,
                report_files=[],
                build_only=True,
                debug=debug,
                build_receipt_id=(
                    receipt["build_receipt_id"] if receipt else None),
                configuration=(receipt["configuration"] if receipt else None),
                artifact_counts=(
                    receipt["artifact_counts"] if receipt else
                    _build_artifact_counts(active_build_dir, mode)),
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
                    log_handle, deadline, cwd=REPOSITORY_ROOT, env=env,
                )
                if rc != 0:
                    raise RuntimeError(
                        "extension install (cmake --install --component bioseq) "
                        f"exited with code {rc}")
                _restart_postgresql(active_build_dir, active_preset,
                                    log_handle, deadline)
                _prepare_postgresql_test_database(
                    active_build_dir, active_preset, log_handle, deadline,
                    env=env)

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
                    log_handle, deadline, cwd=REPOSITORY_ROOT, env=env,
                )
                if rc != 0:
                    raise RuntimeError(
                        f"post-build target '{post_target}' exited with code {rc}")

            _update_entry(
                mode,
                step="running",
                uses_private_cluster=mode_details.uses_private_cluster,
            )
            if mode == "benchmark-e2e":
                env.setdefault(
                    "BIOSEQ_E2E_PG_RESTART_CMD",
                    _postgresql_e2e_restart_shell_command(active_build_dir, active_preset),
                )
                log_handle.write(
                    "# BIOSEQ_E2E_PG_RESTART_CMD="
                    f"{env['BIOSEQ_E2E_PG_RESTART_CMD']}\n")
                log_handle.flush()
            run_env = env
            if mode == "highway-gcc-canary":
                run_env = HIGHWAY_GCC_CANARY_RUNTIME_ENVIRONMENT.copy()
                log_handle.write(
                    "# highway-gcc-canary runtime environment="
                    "PATH,LC_ALL,LANG\n")
                log_handle.flush()
            elif mode == "benchmark-e2e" and bench_e2e_config is not None:
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
            completed_at = time.time()
            _update_entry(
                mode,
                status="completed",
                step="completed",
                completed_at=completed_at,
                exit_code=0,
                report_files=[str(path) for path in report_files],
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
        timeout_seconds = (
            build_timeout_sec
            if build_timeout_sec is not None
            else mode_timeout_sec
        )
        try:
            partial_artifact_counts = _build_artifact_counts(
                active_build_dir, mode)
            artifact_count_error = None
        except (OSError, RuntimeError) as artifact_error:
            partial_artifact_counts = None
            artifact_count_error = str(artifact_error)
        _update_entry(
            mode,
            status="killed_timeout",
            step="killed_timeout",
            completed_at=completed_at,
            exit_code=None,
            notes=str(e),
            build_only=build_only,
            debug=debug,
            artifact_counts=partial_artifact_counts,
            artifact_count_error=artifact_count_error,
        )
        log_handle.write(
            f"\n# killed by {timeout_seconds}-second "
            f"{'build-phase' if build_timeout_sec is not None else 'mode'} "
            f"hard timeout: {e}\n")
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

def _unverified_guard_cleanup(
        state: Mapping[str, object]) -> str | None:
    """Re-sweep unique build roots before clearing an unverified cleanup."""
    for entry in state.values():
        if not isinstance(entry, dict) or not entry.get("cleanup_unverified"):
            continue
        build_dir_value = entry.get("build_dir")
        if not isinstance(build_dir_value, str) or not build_dir_value:
            return "a previous guarded cleanup has no ownership root"
        ownership_root = Path(build_dir_value).resolve(strict=False)
        try:
            snapshot, owned_pids = _guarded_process_snapshot(
                "cleanup-revalidation-sweep", ownership_root)
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            return f"cannot revalidate previous guarded cleanup: {exc}"
        live = set(owned_pids)
        evidence = entry.get("termination_evidence")
        identities = (
            evidence.get("survivor_identities")
            if isinstance(evidence, dict) else None
        )
        if identities is not None:
            if not isinstance(identities, list):
                return (
                    "a previous guarded cleanup has malformed identity "
                    "evidence")
            for value in identities:
                if not isinstance(value, dict):
                    return (
                        "a previous guarded cleanup has malformed identity "
                        "evidence")
                try:
                    identity = _ProcessIdentity(**value)
                except (TypeError, ValueError):
                    return (
                        "a previous guarded cleanup has malformed identity "
                        "evidence")
                if _same_process(snapshot.get(identity.pid), identity):
                    live.add(identity.pid)
        if live:
            return (
                "previous guarded build descendants remain live: "
                + ", ".join(str(pid) for pid in sorted(live))
            )
    return None

def _running_in_same_group(
        state: dict, mode: str, build_only: bool,
        requested_build_dir: Path) -> tuple[str | None, dict | None]:
    """Find any currently-running run that conflicts with `mode`.

    Returns (running_mode, entry) or (None, None). Conflicts are decided by
    lock groups, every resolved CMake build directory, and the shared
    PostgreSQL install/restart resource. Build-directory comparison uses all
    paths recorded when a worker was reserved; entries without those paths
    fall back to their recorded debug selection. An unreadable or malformed path
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


_ORDINARY_BENCH_E2E_BENCHMARKS = (
    "bench_e2e",
    "bench_e2e_signoff",
    "bench_e2e_gpu_batch_validation",
    "bench_e2e_routing_poc",
    "bench_sort_scan",
)




def _ordinary_bench_e2e_default_filter(
        mode: str,
        bench_filter,
        *,
        build_only: bool,
        debug_report: bool,
        filter_supplied: bool):
    """Select the ordinary E2E benchmark set when no filter was supplied."""
    if (
        mode == "benchmark-e2e"
        and not build_only
        and not debug_report
        and not filter_supplied
        and bench_filter is None
    ):
        return {
            "benchmarks": list(_ORDINARY_BENCH_E2E_BENCHMARKS),
        }
    return bench_filter



def _validate_bench_e2e_config_override(mode: str, requested_path):
    """Resolve one explicit local benchmark-e2e config without path escape.

    The public surface accepts either an absolute path or a repository-root
    relative path. ``resolve(strict=True)`` follows symlinks before the
    containment check, so a repository symlink cannot smuggle an outside file
    into the worker. Returning the canonical absolute path also avoids making
    interpretation depend on the detached worker's current directory.
    """
    if requested_path is None:
        return None, None
    if mode != "benchmark-e2e":
        return None, "bench_e2e_config is accepted only for mode='benchmark-e2e'"
    if not isinstance(requested_path, str) or not requested_path.strip():
        return None, "bench_e2e_config must be a non-empty path string"
    if requested_path != requested_path.strip():
        return None, "bench_e2e_config may not have surrounding whitespace"
    if any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in requested_path):
        return None, "bench_e2e_config may not contain control characters"

    candidate = Path(requested_path).expanduser()
    if not candidate.is_absolute():
        candidate = REPOSITORY_ROOT / candidate
    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(REPOSITORY_ROOT.resolve(strict=True))
    except (OSError, RuntimeError, ValueError) as exc:
        return None, (
            "bench_e2e_config must resolve to an existing file inside the "
            f"repository {REPOSITORY_ROOT}: {exc}"
        )
    if not resolved.is_file():
        return None, f"bench_e2e_config is not a regular file: {resolved}"
    if resolved.suffix.lower() != ".json":
        return None, f"bench_e2e_config must name a .json file: {resolved}"
    return str(resolved), None

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
                     fresh: bool = False,
                     build_timeout_sec: int | None = None,
                     run_only: bool = False,
                     reuse_build_receipt: str | None = None) -> str:
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

    if build_timeout_sec is not None and (
        not isinstance(build_timeout_sec, int)
        or isinstance(build_timeout_sec, bool)
        or not 60 <= build_timeout_sec <= MAX_BUILD_PHASE_TIMEOUT_SEC
    ):
        return (
            "build_timeout_sec must be an integer between 60 and "
            f"{MAX_BUILD_PHASE_TIMEOUT_SEC} seconds")
    if run_only:
        if build_only:
            return "run_only and build_only are mutually exclusive"
        if fresh or build_timeout_sec is not None:
            return (
                "run_only rejects fresh and build_timeout_sec; "
                "the receipt pins those values")
        if not isinstance(reuse_build_receipt, str) or not reuse_build_receipt:
            return "run_only requires reuse_build_receipt"
        if mode not in _RUN_ONLY_RECEIPT_MODES:
            return f"mode {mode!r} does not support receipt-pinned run_only"
    else:
        if reuse_build_receipt is not None:
            return "reuse_build_receipt is valid only with run_only=true"
        if fresh and not build_only:
            return "fresh=true requires build_only=true"
        if build_timeout_sec is not None and not build_only:
            return "build_timeout_sec is accepted only for build_only runs"
        if build_timeout_sec is not None and not fresh:
            return (
                "build_timeout_sec requires fresh=true so detached-process "
                "ownership is isolated to one MCP build directory")
    mode_details = MODES[mode]
    if mode_details.requires_build_only and (
        not build_only
        or run_only
        or reuse_build_receipt is not None
        or debug_supplied or debug
        or filter_supplied or bench_filter is not None
        or debug_report_supplied or debug_report
        or bench_e2e_config is not None
    ):
        return (
            f"{mode} is a fixed local compile gate: it requires "
            "build_only=true and accepts no debug, filter, debug_report, "
            "bench_e2e_config inputs.")
    if mode == PACKAGE_LINUX_MODE and (
        build_only
        or run_only or fresh or build_timeout_sec is not None
        or reuse_build_receipt is not None
        or debug_supplied or debug
        or filter_supplied or bench_filter is not None
        or debug_report_supplied or debug_report
        or bench_e2e_config is not None
    ):
        return (
            "package-linux owns one fixed local Debian 13/PostgreSQL 17/ARM64 "
            "qualification and accepts no build_only, debug, filter, "
            "debug_report or bench_e2e_config inputs.")
    resolved_e2e_config, config_error = _validate_bench_e2e_config_override(
        mode, bench_e2e_config)
    if config_error is not None:
        return f"Invalid bench_e2e_config: {config_error}"
    bench_filter = _ordinary_bench_e2e_default_filter(
        mode,
        bench_filter,
        build_only=build_only,
        debug_report=debug_report,
        filter_supplied=filter_supplied,
    )
    if mode == POSTGRES_ERROR_SAFETY_MODE:
        if (
            run_only or fresh or build_timeout_sec is not None
            or reuse_build_receipt is not None
        ):
            return (
                "postgres-error-safety does not accept receipt/fresh inputs")
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
    run_token = secrets.token_hex(16)
    selected_receipt: dict[str, object] | None = None
    try:
        if run_only:
            selected_receipt, receipt_error = _validate_build_receipt(
                str(reuse_build_receipt),
                expected_mode=mode,
                expected_debug=(debug if debug_supplied else None),
            )
            if receipt_error is not None or selected_receipt is None:
                return f"Build receipt validation error: {receipt_error}"
            debug = bool(selected_receipt["debug"])
            selected_preset = str(selected_receipt["preset"])
            source_repository_root = Path(
                str(selected_receipt["source_repository_root"])
            ).resolve(strict=True)
            selected_build_dir = Path(
                str(selected_receipt["build_dir"])
            ).resolve(strict=True)
            source_commit = str(selected_receipt["source_commit"])
            source_graph_fingerprint = str(
                selected_receipt["source_graph_fingerprint"])
        else:
            selected_preset = preset_for(mode, debug)
            source_repository_root = REPOSITORY_ROOT
            source_repository_root = source_repository_root.resolve(
                strict=True)
            source_commit = _git_commit(source_repository_root)
            source_graph_fingerprint = _source_graph_fingerprint(
                source_repository_root)
            selected_build_dir = (
                source_repository_root / "build" / "mcp" / run_token
                / selected_preset
                if fresh
                else source_repository_root / "build" / selected_preset
            ).resolve(strict=False)
    except (OSError, RuntimeError) as exc:
        return f"Build context isolation error: {exc}"

    # Validate + translate runtime selection before spawning the long-running
    # worker. CLI benchmark modes retain their filter CLI. Catch correctness
    # modes accept only filter.tags, interpreted as Catch2 test-spec
    # expressions and forwarded identically to every executable in the mode.
    bench_flags: list[str] = []
    test_flags: list[str] = []
    if mode in CATCH_FILTER_MODES:
        if debug_report_supplied or debug_report:
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
            return ("`debug_report` requires a non-empty `filter` — debug is for targeted "
                    "runs, not for instrumenting an entire benchmark sweep.")
        bench_flags = list(filter_args)
        if debug_report:
            bench_flags.append("--debug")

    with _global_lock():
        state = _load_state()
        cleanup_blocker = _unverified_guard_cleanup(state)
        if cleanup_blocker is not None:
            return (
                "Build guard cleanup error: " + cleanup_blocker
                + "; refusing to start another build")
        running_mode, running_entry = _running_in_same_group(
            state, mode, build_only,
            selected_build_dir)
        # Refreshing running entries can be the operation that discovers a
        # dead guarded worker and records fail-closed cleanup evidence.  Do
        # not let this same scheduling request pass the cleanup gate using
        # the pre-refresh state.
        cleanup_blocker = _unverified_guard_cleanup(state)
        if cleanup_blocker is not None:
            return (
                "Build guard cleanup error: " + cleanup_blocker
                + "; refusing to start another build")
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
        worker_env["BIOSEQ_BUILD_REPOSITORY_ROOT"] = str(
            source_repository_root)

        # Validate the machine-owned compiler policy only after request
        # validation and conflict detection.  The preflight creates one
        # short-lived unprivileged RR/1 probe; it never changes this persistent
        # MCP server's scheduling policy.
        if mode == PACKAGE_LINUX_MODE:
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
        bootstrap_log_path = log_path.with_name(
            f"{log_path.name}.bootstrap")

        worker_e2e_config = resolved_e2e_config
        receipt_required = _build_receipt_required(mode, build_only)
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
            "bootstrap_log_file": str(bootstrap_log_path),
            "exit_code": None,
            "report_files": [],
            "notes": None,
            "debug": debug,
            "build_only": build_only,
            "run_only": run_only,
            "fresh": fresh,
            "build_timeout_sec": build_timeout_sec,
            "timeout_guard_armed": False,
            "reuse_build_receipt": reuse_build_receipt,
            "receipt_required": receipt_required,
            "pending_build_receipt_id": (
                f"br_{run_token}" if receipt_required else None
            ),
            "preset": selected_preset,
            "build_dir": str(selected_build_dir),
            "build_dirs": [str(selected_build_dir)],
            "source_repository_root": str(source_repository_root),
            "source_commit": source_commit,
            "source_graph_fingerprint": source_graph_fingerprint,
            "preset_family": mode_details.preset_family,
            "binary_subdir": mode_details.binary_subdir,
            "uses_private_cluster": mode_details.uses_private_cluster,
            "bench_flags": bench_flags,
            "focused_test_args": test_flags,
            "bench_e2e_config": worker_e2e_config,
            "rt_priority": configured_rt_priority,
            "effective_run_args":
                test_flags if mode in CATCH_FILTER_MODES else bench_flags,
        }
        _save_state(state)

        server_path = Path(__file__).with_name("server.py").resolve()
        try:
            bootstrap_log_path.parent.mkdir(parents=True, exist_ok=True)
            with bootstrap_log_path.open("w", buffering=1) as bootstrap_log:
                bootstrap_log.write(
                    "# detached build worker bootstrap; the worker removes "
                    "this file after opening its primary log\n")
                bootstrap_log.flush()
                proc = subprocess.Popen(
                    [sys.executable, str(server_path),
                     "--run-build", mode,
                     "--build-only", ("true" if build_only else "false"),
                     "--debug", ("true" if debug else "false"),
                     "--fresh", ("true" if fresh else "false"),
                     "--run-only", ("true" if run_only else "false"),
                     "--build-timeout-sec", (
                         str(build_timeout_sec)
                         if build_timeout_sec is not None else ""),
                     "--reuse-build-receipt", reuse_build_receipt or "",
                     "--bench-flags", json.dumps(bench_flags),
                     "--test-flags", json.dumps(test_flags),
                     "--benchmark-e2e-config", worker_e2e_config or ""],
                    stdin=subprocess.DEVNULL,
                    stdout=bootstrap_log,
                    stderr=subprocess.STDOUT,
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
        worker_identity = _capture_process_identity(proc.pid)
        guarded_identity_missing = (
            build_timeout_sec is not None
            and not _valid_detached_worker_identity(
                worker_identity,
                child_pid=proc.pid,
                parent_pid=os.getpid(),
            )
        )
        if guarded_identity_missing:
            state[mode].update({
                "status": "failed",
                "step": "worker_identity_failed",
                "completed_at": time.time(),
                "worker_process_identity": None,
                "notes": (
                    "guarded worker exact pid/start/session capture failed "
                    "before arming"
                ),
            })
            _save_state(state)
            unarmed_termination = _terminate_unarmed_popen_group(proc)
            state = _load_state()
            if mode in state:
                cleanup_unverified = not bool(
                    unarmed_termination.get("cleanup_verified"))
                state[mode]["termination_evidence"] = unarmed_termination
                state[mode]["cleanup_unverified"] = cleanup_unverified
                _save_state(state)
            return (
                "Build worker identity error: guarded worker could not be "
                "armed before configure/build")
        state[mode]["worker_process_identity"] = (
            worker_identity.__dict__ if worker_identity is not None else None
        )
        if build_timeout_sec is not None:
            state[mode]["step"] = "armed"
        _save_state(state)
        _log(f"started worker pid={proc.pid} mode={mode} debug={debug} "
             f"rt_priority={configured_rt_priority} "
             f"bench_flags={bench_flags} focused_test_args={test_flags} "
             f"bench_e2e_config={resolved_e2e_config} "
             f"log={log_path}")

    return (f"{mode_details.label} {mode_details.verb_is} scheduled "
            f"(preset={selected_preset}, build_dir={selected_build_dir})"
            + (
                f" from build receipt {reuse_build_receipt} with no build"
                if run_only else ""
            )
            + (
                f"; pending build receipt br_{run_token} becomes valid only "
                "after successful build-only completion"
                if receipt_required else ""
            )
            + (
                f"; guarded build timeout={build_timeout_sec}s"
                if build_timeout_sec is not None else ""
            )
            + (f" with bench flags {' '.join(bench_flags)}" if bench_flags else "")
            + (f" with focused Catch2 args {shlex.join(test_flags)}"
               if test_flags else "")
            + (f" with E2E config {resolved_e2e_config}"
               if resolved_e2e_config else "")
            + f". Predicted finish in {_format_minutes((mode_details.build_only_expected_duration_min if build_only else  mode_details.expected_duration_min), plural_int=True)}.")

def _run_build_inline(mode: str, build_only: bool = False,
                      debug: bool = False, bench_flags=None,
                      test_flags=None, bench_e2e_config=None) -> int:
    """Reserve a local slot and run the worker synchronously."""
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
        _log(f"--run-build-sync: invalid mode {mode!r}")
        return 2
    bench_flags = list(bench_flags or [])
    test_flags = list(test_flags or [])
    mode_details = MODES[mode]
    if mode_details.requires_build_only and (
        not build_only or debug or bench_flags or test_flags
        or bench_e2e_config
    ):
        _log(
            f"--run-build-sync: {mode} requires build_only=true and "
            "rejects every runtime-selection input")
        return 2
    if mode == PACKAGE_LINUX_MODE and (
        build_only or debug or bench_flags or test_flags or bench_e2e_config
    ):
        _log("--run-build-sync: package-linux rejects variable inputs")
        return 2
    resolved_e2e_config, config_error = _validate_bench_e2e_config_override(
        mode, bench_e2e_config)
    if config_error is not None:
        _log(f"--run-build-sync: invalid bench_e2e_config: {config_error}")
        return 2
    if mode == POSTGRES_ERROR_SAFETY_MODE:
        try:
            _checked_postgres_error_safety_case_template()
        except RuntimeError as exc:
            _log(f"--run-build-sync: safety configuration error: {exc}")
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
            REPOSITORY_ROOT / "build" / selected_preset)
        if running_entry:
            _log(
                f"--run-build-sync: a conflicting build ({running_mode}) is "
                f"already running on this host (pid={running_entry.get('pid')}); "
                f"refusing to start {mode}")
            return 3

        if mode == PACKAGE_LINUX_MODE:
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
            "report_files": [],
            "notes": None,
            "debug": debug,
            "build_only": build_only,
            "run_only": False,
            "fresh": False,
            "build_timeout_sec": None,
            "reuse_build_receipt": None,
            "preset": selected_preset,
            "build_dir": str(REPOSITORY_ROOT / "build" / selected_preset),
            "build_dirs": [str(path) for path in build_dirs_for(mode, debug)],
            "source_repository_root": str(REPOSITORY_ROOT),
            "source_commit": _git_commit(REPOSITORY_ROOT),
            "source_graph_fingerprint": _source_graph_fingerprint(
                REPOSITORY_ROOT),
            "preset_family": mode_details.preset_family,
            "binary_subdir": mode_details.binary_subdir,
            "uses_private_cluster": mode_details.uses_private_cluster,
            "bench_flags": bench_flags,
            "focused_test_args": test_flags,
            "bench_e2e_config": resolved_e2e_config,
            "rt_priority": configured_rt_priority,
            "effective_run_args": (
                test_flags if mode in CATCH_FILTER_MODES else bench_flags
            ),
        }
        _save_state(state)
        _log(
            f"--run-build-sync: running {mode} inline (pid={os.getpid()}, "
            f"preset={selected_preset}, build_only={build_only}, "
            f"clang_rt_priority={configured_rt_priority}, "
            f"bench_flags={bench_flags}, focused_test_args={test_flags}, "
            f"log={log_path})")

    return _run_build_worker(
        mode,
        build_only=build_only,
        debug=debug,
        bench_flags=bench_flags,
        test_flags=test_flags,
        bench_e2e_config=resolved_e2e_config,
    )


def tool_inspect(arguments: dict) -> str:
    """Implement the `inspect` tool. Returns a single string (a JSON
    document) — the MCP tools/call envelope wraps it in a content item.

    All errors come back as JSON `{"error": "...", ...}` rather than
    raising, so the AI agent always gets a structured result.

    Inspection requires DWARF and per-TU temporary files. Benchmark targets
    resolve against the production preset family; unit and
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
    target = arguments.get("target", "bioseq_benchmark")
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

    # run_inspect validates/defaults/clamps these two result-size controls and
    # surfaces any correction in warnings. Preserve the raw value here so a
    # malformed direct/CLI caller does not silently become indistinguishable
    # from an explicit default.
    max_matches = arguments.get("max_matches", 4)
    snippet_lines = arguments.get("snippet_lines", 200)
    max_llc_bc_files = _int_arg("max_llc_bc_files", 4)
    source_basename = arguments.get("source_basename")
    if source_basename is not None and not isinstance(source_basename, str):
        source_basename = None
    try:
        llc_on_bc = _parse_worker_bool("llc_on_bc", arguments.get("llc_on_bc", True))
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
    receipt_id = arguments.get("reuse_build_receipt")
    try:
        selected_receipt: dict[str, object] | None = None
        if receipt_id is not None:
            if not isinstance(receipt_id, str) or not receipt_id:
                raise RuntimeError(
                    "reuse_build_receipt must be a non-empty string")
            explicit_debug = (
                _parse_worker_bool("debug", debug_raw)
                if debug_raw is not None else None
            )
            selected_receipt, receipt_error = _validate_build_receipt(
                receipt_id,
                expected_mode=inspect_mode,
                expected_debug=explicit_debug,
            )
            if receipt_error is not None or selected_receipt is None:
                raise RuntimeError(
                    f"inspect receipt validation failed: {receipt_error}")
            debug_chosen = bool(selected_receipt["debug"])
            if not debug_chosen:
                raise RuntimeError(
                    "receipt-selected inspect requires a debug build receipt")
            inspect_build_dir = Path(
                str(selected_receipt["build_dir"])
            )
            inspect_preset = str(selected_receipt["preset"])
        elif debug_raw is None:
            debug_chosen = (
                build_dir_for(inspect_mode, True) / "build.ninja"
            ).exists()
            inspect_build_dir = build_dir_for(inspect_mode, debug_chosen)
            inspect_preset = preset_for(inspect_mode, debug_chosen)
        else:
            debug_chosen = _parse_worker_bool("debug", debug_raw)
            inspect_build_dir = build_dir_for(inspect_mode, debug_chosen)
            inspect_preset = preset_for(inspect_mode, debug_chosen)
    except RuntimeError as e:
        return json.dumps({"error": str(e)}, indent=2)

    try:
        artifact_names = _inspect_artifact_candidates(
            target_definition,
            inspect_build_dir,
            debug_chosen,
        )
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
        artifact_names=artifact_names,
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
    if selected_receipt is not None:
        out["build_metadata"]["build_receipt_id"] = receipt_id
        out["build_metadata"]["source_graph_fingerprint"] = (
            selected_receipt["source_graph_fingerprint"])
    return json.dumps(out, indent=2)

def _unknown_document_activity() -> dict:
    return {
        "document_update": {
            "queued": 0,
            "running": 0,
            "state": "unknown",
        },
        "document_embed": {
            "queued": 0,
            "running": 0,
            "state": "unknown",
        },
        "qmd_server": {"persistent": False, "state": "unknown"},
    }

def _document_activity() -> dict:
    """Read the existing QMD/document manager without starting an operation."""
    try:
        from docs.qmd_manager import get_manager

        snapshot = get_manager().activity_snapshot()
    except Exception:
        return _unknown_document_activity()
    required = {"document_update", "document_embed", "qmd_server"}
    if not isinstance(snapshot, dict) or set(snapshot) != required:
        return _unknown_document_activity()
    for name in ("document_update", "document_embed"):
        value = snapshot.get(name)
        if (
            not isinstance(value, dict)
            or set(value) != {"queued", "running", "state"}
            or any(
                not isinstance(value[field], int)
                or isinstance(value[field], bool)
                or value[field] < 0
                for field in ("queued", "running")
            )
            or value.get("state")
            not in {"absent", "idle", "queued", "running", "unknown"}
        ):
            return _unknown_document_activity()
    qmd = snapshot.get("qmd_server")
    if (
        not isinstance(qmd, dict)
        or set(qmd) != {"persistent", "state"}
        or not isinstance(qmd.get("persistent"), bool)
        or qmd.get("state") not in {"absent", "idle", "busy", "unknown"}
    ):
        return _unknown_document_activity()
    return snapshot

def _local_activity() -> dict:
    """Return the closed, path-free aggregate used at tree-freeze boundaries."""
    with _global_lock():
        build_state, build_registry_present = _read_activity_registry(STATE_FILE)
    if build_state is None:
        build_activity = _transient_activity([None])
        papers_activity = _transient_activity([None])
        other_activity = _transient_activity([None])
    else:
        build_activity = _transient_activity(
            [build_state[key] for key in MODES if key in build_state],
            registry_present=build_registry_present,
        )
        papers_entries = (
            [build_state[PAPERS_BUILD_STATE_KEY]]
            if PAPERS_BUILD_STATE_KEY in build_state else []
        )
        papers_activity = _transient_activity(papers_entries)
        known_keys = set(MODES) | {PAPERS_BUILD_STATE_KEY}
        other_activity = _transient_activity(
            [value for key, value in build_state.items()
             if key not in known_keys]
        )

    subagent_state, subagent_present = _read_activity_registry(
        SUBAGENT_STATE_FILE)
    subagent_activity = _transient_activity(
        [subagent_state] if subagent_state else (
            [None] if subagent_state is None else []),
        registry_present=subagent_present,
    )
    remote_state, remote_present = _read_activity_registry(REMOTE_STATE_FILE)
    remote_activity = _transient_activity(
        [remote_state] if remote_state else (
            [None] if remote_state is None else []),
        registry_present=remote_present,
    )
    document_activity = _document_activity()
    workers = {
        "build": build_activity,
        "document_update": document_activity["document_update"],
        "document_embed": document_activity["document_embed"],
        "mcp_subagent": subagent_activity,
        "papers": papers_activity,
        "remote_operation": remote_activity,
        "other_detached": other_activity,
        "qmd_server": document_activity["qmd_server"],
    }
    transient_names = (
        "build",
        "document_update",
        "document_embed",
        "mcp_subagent",
        "papers",
        "remote_operation",
        "other_detached",
    )
    state_root = REPOSITORY_ROOT / "build"
    disk_probe = state_root
    while not disk_probe.exists() and disk_probe.parent != disk_probe:
        disk_probe = disk_probe.parent
    try:
        free_disk_bytes = int(shutil.disk_usage(disk_probe.resolve()).free)
    except OSError:
        free_disk_bytes = 0
        workers["other_detached"] = {
            "queued": workers["other_detached"]["queued"],
            "running": workers["other_detached"]["running"],
            "state": "unknown",
        }
    quiescent = all(
        workers[name]["queued"] == 0
        and workers[name]["running"] == 0
        and workers[name]["state"] in {"absent", "idle"}
        for name in transient_names
    ) and workers["qmd_server"]["state"] in {"absent", "idle"}
    return {
        "quiescent": quiescent,
        "free_disk_bytes": free_disk_bytes,
        "workers": workers,
    }

def tool_status(mode: str | None = None) -> str:
    if mode is None:
        return json.dumps({"local_activity": _local_activity()}, indent=2)
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
    bootstrap_log_path = entry.get("bootstrap_log_file")
    bootstrap_suffix = (
        f". Bootstrap log: {bootstrap_log_path}"
        if isinstance(bootstrap_log_path, str) and bootstrap_log_path
        else ""
    )
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
    else:
        report_suffix = ""
    qualification = entry.get("production_dso_qualification")
    qualification_suffix = (
        ". Production DSO qualification: "
        + json.dumps(qualification, sort_keys=True)
        if isinstance(qualification, dict) and qualification
        else ""
    )
    receipt_suffix = (
        f". Build receipt: {entry['build_receipt_id']}"
        if entry.get("build_receipt_id") else ""
    )
    if entry.get("run_receipt_id"):
        receipt_suffix += f". Run receipt: {entry['run_receipt_id']}"
    artifact_counts = entry.get("artifact_counts")
    artifact_suffix = (
        ". Artifact counts: " + json.dumps(
            artifact_counts, sort_keys=True)
        if isinstance(artifact_counts, dict)
        else ""
    )
    guard_suffix = ""
    if entry.get("build_timeout_sec") is not None:
        guard_suffix = (
            ". Build guard: "
            f"timeout={entry['build_timeout_sec']}s "
            f"armed={bool(entry.get('timeout_guard_armed'))} "
            f"active_phase={entry.get('active_phase')} "
            f"active_command={entry.get('active_command')} "
            f"active_edge={entry.get('active_edge')} "
            f"TERM-at={entry.get('build_soft_deadline')} "
            f"KILL-by={entry.get('build_hard_deadline')} "
            "worker_identity="
            f"{json.dumps(entry.get('worker_process_identity'), sort_keys=True)}"
        )
    termination_suffix = (
        ". Termination evidence: "
        + json.dumps(entry.get("termination_evidence"), sort_keys=True)
        + f" cleanup_unverified={bool(entry.get('cleanup_unverified'))}"
        if entry.get("termination_evidence") is not None
        else ""
    )

    if status == "running":
        elapsed_min = (time.time() - entry["started_at"]) / 60.0
        eta_min = max(0.0, (mode_details.build_only_expected_duration_min if build_only else  mode_details.expected_duration_min) - elapsed_min)
        step = entry.get("step", "running")
        return (f"{mode_details.label} {mode_details.verb_is} already scheduled"
                f"{preset_suffix}. "
                f"Current step: {step}. "
                f"Predicted finish in {_format_minutes(eta_min)}. "
                f"{focus_suffix} "
                f"Log: {log_path}{report_suffix}{qualification_suffix}"
                f"{bootstrap_suffix}{guard_suffix}{artifact_suffix}")

    completed_at = entry.get("completed_at") or time.time()
    duration_min = (completed_at - entry["started_at"]) / 60.0

    if status == "completed":
        msg = (f"{mode_details.label} {mode_details.verb_was} already completed"
               f"{preset_suffix} "
               f"(total time: {_format_minutes(duration_min, plural_int=True)}). "
               f"{focus_suffix} "
               f"Log: {log_path}{report_suffix}")
        if entry.get("build_only", False):
            msg += (". This was a build-only job, so compile targets were built "
                    "but commands and post-build/private-cluster targets were not run.")
        if debug_used:
            msg += (f". This was a debug build (preset={selected_preset}); "
                    "DWARF + per-TU .s/.bc artefacts are available under "
                    f"{entry.get('build_dir', str(build_dir_for(mode, True)))} for the "
                    "`inspect` tool.")
        msg += qualification_suffix
        msg += bootstrap_suffix
        msg += receipt_suffix
        msg += guard_suffix
        msg += artifact_suffix
        return msg

    if status == "killed_timeout":
        return ("Last run was forcefully killed due to timeout. "
                f"{focus_suffix} "
                f"Logs for inspection are available here: "
                f"{log_path}{report_suffix}{qualification_suffix}"
                f"{bootstrap_suffix}{guard_suffix}{artifact_suffix}. "
                "Termination evidence: "
                f"{json.dumps(entry.get('termination_evidence'), sort_keys=True)}")

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
                f"Log: {log_path}{report_suffix}{qualification_suffix}."
                f"{private_note}{bootstrap_suffix}{guard_suffix}"
                f"{artifact_suffix}"
                f"{termination_suffix}")

    if status == "aborted":
        return (f"{mode_details.label} run was aborted (worker process died "
                f"unexpectedly after {_format_minutes(duration_min)}). "
                f"{focus_suffix} "
                f"Log: {log_path}{report_suffix}{qualification_suffix}"
                f"{bootstrap_suffix}{guard_suffix}{artifact_suffix}"
                f"{termination_suffix}")

    return (f"{mode_details.label} status: {status}."
            f"{focus_suffix} Log: {log_path}{report_suffix}"
            f"{qualification_suffix}{bootstrap_suffix}")

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
            "sha256": sha256_file(PAPERS_PDF_PATH),
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
            pdf_sha256=sha256_file(PAPERS_PDF_PATH),
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

        server_path = Path(__file__).with_name("server.py").resolve()
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
        include_images = _parse_worker_bool(
            "include_images", arguments.get("include_images", True))
    except RuntimeError as exc:
        return _papers_inspect_error(str(exc))

    pdf_sha256 = sha256_file(PAPERS_PDF_PATH)
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
        f"# repository_root={REPOSITORY_ROOT}\n"
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
            "-C", str(REPOSITORY_ROOT),
            full_prompt,
        ]
        rc = _stream_subprocess(cmd, log_handle, deadline,
                                cwd=REPOSITORY_ROOT, env=env)

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

        server_path = Path(__file__).with_name("server.py").resolve()
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
