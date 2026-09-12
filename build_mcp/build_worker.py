"""Build execution and worker-facing tool operations."""

from __future__ import annotations

from typing import List, Dict, Set, Optional, Any, Mapping

import argparse
import base64
import contextlib
import copy
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
    ALL_ALPHABETS_UNIT_INTEGRATION_MODE,
    BENCH_CLI_MODES,
    CATCH_FILTER_MODES,
    CMAKE_PRESETS_FILE,
    DEFAULT_BUILD_DIR,
    LINT_FOCUSED_MODE,
    LINT_FULL_MODE,
    LINT_MODES,
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
    lint_filter_to_paths,
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
from doxygen_toolchain import ensure_exact_doxygen
from integrity import (
    canonical_json_sha256,
    sha256_file,
)
from postgres_runtime import (
    _cmake_install_cmd,
    _cleanup_owned_postgresql_backends,
    _postgresql_discovery_cache_reset_args,
    _postgresql_e2e_restart_shell_command,
    _postgresql_guard_service_boundary_provider,
    _legacy_q1024_global_postgresql_zero_evidence,
    _postgresql_recovery_service_boundary_provider,
    _prepare_postgresql_test_database,
    _restart_postgresql,
)
from postgres_safety import (
    _checked_postgres_error_safety_case_template,
    _run_postgres_error_safety_worker,
)
from process_state import (
    BUILD_TIMEOUT_TERM_GRACE_SEC,
    LOG_DIR,
    REMOTE_STATE_FILE,
    STATE_FILE,
    _ProcessIdentity,
    _await_guarded_worker_arm,
    _await_q1024_watchdog_arm,
    _build_worker_descendant_environment,
    _capture_process_identity,
    _claim_q1024_postgresql_cleanup,
    _claim_q1024_postgresql_revalidation,
    _finish_q1024_postgresql_cleanup,
    _finish_q1024_postgresql_revalidation,
    _get_build_receipt,
    _global_lock,
    _guarded_process_snapshot,
    _load_state,
    _log,
    _read_activity_registry,
    _refresh,
    _require_trusted_root_owned_path,
    _same_process,
    _save_state,
    _store_build_receipt,
    _stream_subprocess,
    _tail_path,
    _terminate_unarmed_popen_group,
    _transient_activity,
    _trusted_root_owned_executable,
    _update_entry,
    _valid_detached_worker_identity,
)
from subagent import subagent_activity_snapshot
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
        managed_toolchain_identity: Mapping[str, object] | None = None,
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
    if managed_toolchain_identity:
        inputs["managed_toolchain_identity"] = dict(
            managed_toolchain_identity)
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


def _prepare_mode_toolchain(
        mode: str,
        *,
        environment: dict[str, str],
        log_handle) -> tuple[dict[str, str], dict[str, object]]:
    """Provision and expose exact tools owned by one managed build mode.

    Website provisioning runs before any CMake configure command. Other modes
    remain independent of Doxygen and therefore return empty contracts.
    """
    if mode != "website":
        return {}, {}

    provisioned = ensure_exact_doxygen()
    executable = str(provisioned.executable)
    executable_directory = str(provisioned.executable.parent)
    existing_path = environment.get("PATH", "")
    path_entries = [
        entry
        for entry in existing_path.split(os.pathsep)
        if entry and entry != executable_directory
    ]
    environment["PATH"] = os.pathsep.join(
        [executable_directory, *path_entries])
    identity = provisioned.configure_identity()
    log_handle.write(
        "# exact website Doxygen provisioned: "
        f"path={executable} version={provisioned.version_output!r} "
        f"platform={provisioned.platform_name} "
        f"architecture={provisioned.architecture} "
        f"archive_sha256={provisioned.archive_sha256} "
        f"executable_sha256={provisioned.executable_sha256} "
        f"cache_hit={provisioned.cache_hit}.\n"
    )
    log_handle.flush()
    return (
        {"DOXYGEN_EXECUTABLE": executable},
        {"doxygen": identity},
    )


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
        shared_target_names=("bioseq_modules_nopg",
                             "bioseq_highway_fixed_target",
                             "bioseq_runtime_core")),
    "bioseq_io_benchmark": InspectTargetDefinition(
        mode="benchmark-io", artifact_names=("bioseq_io_benchmark",),
        shared_target_names=("bioseq_modules_nopg", "bioseq_runtime_core")),
    "bioseq_unit_tests": InspectTargetDefinition(
        mode="unit-integration", artifact_names=("bioseq_unit_tests",),
        shared_target_names=("bioseq_modules_nopg",
                             "bioseq_highway_fixed_target",
                             "bioseq_runtime_core")),
    "bioseq_integration_tests": InspectTargetDefinition(
        mode="unit-integration",
        artifact_names=("bioseq_integration_tests",),
        shared_target_names=("bioseq_modules_nopg",
                             "bioseq_highway_fixed_target",
                             "bioseq_runtime_core")),
    "bioseq_benchmark_e2e_contract_tests": InspectTargetDefinition(
        mode="unit-integration",
        artifact_names=("bioseq_benchmark_e2e_contract_tests",),
        shared_target_names=("bioseq_modules_nopg", "bioseq_runtime_core")),
    "bioseq_benchmark_framework_contract_tests": InspectTargetDefinition(
        mode="unit-integration",
        artifact_names=("bioseq_benchmark_framework_contract_tests",),
        shared_target_names=("bioseq_modules_nopg", "bioseq_runtime_core")),
    "postgresql_bioseq": InspectTargetDefinition(
        mode="benchmark-e2e",
        artifact_names=("bioseq.dylib", "bioseq.so", "bioseq.dll"),
        shared_target_names=("bioseq_modules_nopg",
                             "bioseq_highway_fixed_target",
                             "bioseq_runtime_core",
                             "bioseq_pipeline_dispatch_owners")),
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


def _clang_tidy_scope(mode: str) -> str:
    """Return the script scope owned by one closed lint build mode."""

    if mode == LINT_FOCUSED_MODE:
        return "focused"
    if mode == LINT_FULL_MODE:
        return "full"
    raise ValueError(f"mode {mode!r} does not own clang-tidy analysis")


def _clang_tidy_report_path(build_dir: Path, mode: str) -> Path:
    """Return the build-tree report path for a lint mode."""

    return build_dir / "bioseq-clang-tidy" / f"{_clang_tidy_scope(mode)}.json"


def _clang_tidy_command(
        build_dir: Path, mode: str, lint_paths: list[str]) -> list[str]:
    """Construct the non-mutating clang-tidy runner command."""

    command = [
        sys.executable,
        str(REPOSITORY_ROOT / "build_mcp" / "clang_tidy.py"),
        "--source-root", str(REPOSITORY_ROOT),
        "--build-dir", str(build_dir),
        "--scope", _clang_tidy_scope(mode),
        "--report", str(_clang_tidy_report_path(build_dir, mode)),
    ]
    for lint_path in lint_paths:
        command.extend(("--path", lint_path))
    return command


def _clang_tidy_report_summary(
    report_path: Path,
    *,
    expected_scope: str | None = None,
) -> dict[str, object]:
    """Validate a lint report and return its bounded status summary."""

    try:
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise RuntimeError(
            f"clang-tidy did not create its report: {report_path}"
        ) from error
    except json.JSONDecodeError as error:
        raise RuntimeError(f"clang-tidy report is invalid JSON: {error}") from error
    if not isinstance(report, dict) or report.get("schema_version") != 2:
        raise RuntimeError("clang-tidy report has an unsupported schema")
    required_integer_fields = (
        "available_count",
        "available_path_count",
        "available_authored_count",
        "available_authored_path_count",
        "available_generated_count",
        "available_generated_path_count",
        "selected_count",
        "selected_path_count",
        "authored_count",
        "authored_path_count",
        "generated_count",
        "generated_path_count",
        "optimization_verified_count",
        "source_suppression_count",
        "diagnostic_count",
        "failure_count",
    )
    for field_name in required_integer_fields:
        value = report.get(field_name)
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise RuntimeError(
                f"clang-tidy report field {field_name!r} must be a nonnegative integer")
    compile_databases = report.get("compile_databases")
    if not isinstance(compile_databases, list) or not compile_databases:
        raise RuntimeError(
            "clang-tidy report must identify at least one compile database")
    compile_database_paths: list[str] = []
    for evidence in compile_databases:
        if not isinstance(evidence, dict):
            raise RuntimeError("clang-tidy compile database evidence is invalid")
        path = evidence.get("path")
        digest = evidence.get("sha256")
        if (
            not isinstance(path, str)
            or not path
            or path.startswith("/")
            or ".." in Path(path).parts
            or not isinstance(digest, str)
            or re.fullmatch(r"[0-9a-f]{64}", digest) is None
        ):
            raise RuntimeError("clang-tidy compile database evidence is invalid")
        compile_database_paths.append(path)
    if len(set(compile_database_paths)) != len(compile_database_paths):
        raise RuntimeError("clang-tidy compile database evidence has duplicate paths")
    contributions = report.get("compile_database_contributions")
    if (
        not isinstance(contributions, dict)
        or set(contributions) != set(compile_database_paths)
    ):
        raise RuntimeError("clang-tidy compile database contributions are invalid")
    for path, count in contributions.items():
        if not isinstance(path, str) or type(count) is not int or count < 0:
            raise RuntimeError("clang-tidy compile database contributions are invalid")
    if sum(contributions.values()) != report["available_count"]:
        raise RuntimeError(
            "clang-tidy compile database contributions do not cover availability")
    report_scope = report.get("scope")
    if report_scope not in {"focused", "full"}:
        raise RuntimeError("clang-tidy report has an invalid scope")
    if expected_scope is not None and report_scope != expected_scope:
        raise RuntimeError(
            "clang-tidy report scope does not match the scheduled mode: "
            f"expected={expected_scope} observed={report_scope}"
        )
    if (
        report["available_authored_count"] + report["available_generated_count"]
        != report["available_count"]
    ):
        raise RuntimeError("clang-tidy report available unit counts are inconsistent")
    if (
        report["available_authored_path_count"]
        + report["available_generated_path_count"]
        != report["available_path_count"]
    ):
        raise RuntimeError("clang-tidy report available path counts are inconsistent")
    if (
        report["available_count"] == 0
        or report["available_path_count"] == 0
        or report["available_path_count"] > report["available_count"]
        or report["available_authored_path_count"]
        > report["available_authored_count"]
        or report["available_generated_path_count"]
        > report["available_generated_count"]
    ):
        raise RuntimeError("clang-tidy report available coverage is empty or invalid")
    if (
        report["authored_count"] + report["generated_count"]
        != report["selected_count"]
    ):
        raise RuntimeError("clang-tidy report unit counts are inconsistent")
    if (
        report["authored_path_count"] + report["generated_path_count"]
        != report["selected_path_count"]
    ):
        raise RuntimeError("clang-tidy report selected path counts are inconsistent")
    if (
        report["selected_count"] == 0
        or report["selected_path_count"] == 0
        or report["selected_path_count"] > report["selected_count"]
        or report["authored_path_count"] > report["authored_count"]
        or report["generated_path_count"] > report["generated_count"]
    ):
        raise RuntimeError("clang-tidy report selected coverage is empty or invalid")
    if (
        report["selected_count"] > report["available_count"]
        or report["selected_path_count"] > report["available_path_count"]
        or report["authored_count"] > report["available_authored_count"]
        or report["generated_count"] > report["available_generated_count"]
        or report["authored_path_count"] > report["available_authored_path_count"]
        or report["generated_path_count"] > report["available_generated_path_count"]
    ):
        raise RuntimeError("clang-tidy report selected coverage exceeds availability")
    if report["optimization_verified_count"] != report["available_count"]:
        raise RuntimeError(
            "clang-tidy report does not verify every available compile profile")
    optimization_policy = report.get("optimization_policy")
    if optimization_policy != {
        "optimization_flags": ["-O3"],
        "ndebug_required": True,
    }:
        raise RuntimeError("clang-tidy report has an invalid optimization policy")
    if report["failure_count"] > report["selected_count"]:
        raise RuntimeError("clang-tidy report failure count exceeds selected units")
    results = report.get("results")
    if not isinstance(results, list) or len(results) != report["selected_count"]:
        raise RuntimeError("clang-tidy report result count is inconsistent")
    result_identities: set[tuple[str, int]] = set()
    result_paths: dict[bool, set[str]] = {False: set(), True: set()}
    result_counts: dict[bool, int] = {False: 0, True: 0}
    result_failures = 0
    result_diagnostics = 0
    result_suppressions: set[tuple[str, str]] = set()
    result_database_counts = {path: 0 for path in compile_database_paths}
    for result in results:
        if not isinstance(result, dict):
            raise RuntimeError("clang-tidy report result record is invalid")
        path = result.get("path")
        variant = result.get("variant")
        generated = result.get("generated")
        exit_code = result.get("exit_code")
        diagnostic_count = result.get("diagnostic_count")
        diagnostics = result.get("diagnostics")
        source_suppressions = result.get("source_suppressions")
        compile_database = result.get("compile_database")
        response_files = result.get("response_files")
        suppression_profiles = result.get("suppression_profiles")
        disabled_checks = result.get("disabled_checks")
        ninja_owners = result.get("ninja_owner_candidates")
        heuristic_owners = result.get("heuristic_owner_candidates")
        owner_query_status = result.get("owner_query_status")
        working_directory = result.get("working_directory")
        compiler = result.get("compiler")
        duration_seconds = result.get("duration_seconds")
        generator_hint = result.get("generator_hint")
        invocation = result.get("invocation")
        output = result.get("output")
        if (
            not isinstance(path, str)
            or not path
            or path.startswith("/")
            or "\\" in path
            or ".." in Path(path).parts
            or type(variant) is not int
            or variant < 0
            or type(generated) is not bool
            or type(exit_code) is not int
            or type(diagnostic_count) is not int
            or diagnostic_count < 0
            or not isinstance(diagnostics, list)
            or len(diagnostics) != diagnostic_count
            or any(not isinstance(diagnostic, dict) for diagnostic in diagnostics)
            or not isinstance(source_suppressions, list)
            or not isinstance(compile_database, str)
            or compile_database not in contributions
            or not isinstance(response_files, list)
            or not isinstance(suppression_profiles, list)
            or any(
                not isinstance(profile, str)
                or profile not in {
                    "named_module",
                    "postgres_abi",
                    "generated_highway",
                    "zero_cost_hot_path",
                }
                for profile in suppression_profiles
            )
            or len(set(suppression_profiles)) != len(suppression_profiles)
            or not isinstance(disabled_checks, list)
            or any(
                not isinstance(check, str) or not check or "*" in check
                for check in disabled_checks
            )
            or len(set(disabled_checks)) != len(disabled_checks)
            or not isinstance(ninja_owners, list)
            or any(not isinstance(owner, str) or not owner for owner in ninja_owners)
            or len(set(ninja_owners)) != len(ninja_owners)
            or not isinstance(heuristic_owners, list)
            or any(
                not isinstance(owner, str) or not owner or owner == "CMakeLists.txt"
                for owner in heuristic_owners
            )
            or len(set(heuristic_owners)) != len(heuristic_owners)
            or (
                owner_query_status is not None
                and (
                    not isinstance(owner_query_status, str)
                    or re.fullmatch(
                        r"ninja-(?:authored-inputs|no-authored-inputs|timeout|"
                        r"exit--?\d+|error-[A-Za-z_][A-Za-z0-9_]*)",
                        owner_query_status,
                    ) is None
                )
            )
            or not isinstance(working_directory, str)
            or not Path(working_directory).is_absolute()
            or not isinstance(compiler, str)
            or not Path(compiler).is_absolute()
            or (
                not isinstance(duration_seconds, (int, float))
                or isinstance(duration_seconds, bool)
                or not math.isfinite(duration_seconds)
                or duration_seconds < 0
            )
            or (generator_hint is not None and not isinstance(generator_hint, str))
            or not isinstance(invocation, list)
            or not invocation
            or any(not isinstance(argument, str) for argument in invocation)
            or not isinstance(output, str)
        ):
            raise RuntimeError("clang-tidy report result record is invalid")
        if (
            (not generated and (ninja_owners or heuristic_owners or owner_query_status))
            or (
                generated
                and (exit_code != 0 or diagnostic_count != 0)
                and not owner_query_status
            )
        ):
            raise RuntimeError("clang-tidy generated-owner result record is invalid")
        for response_file in response_files:
            response_path = (
                response_file.get("path")
                if isinstance(response_file, dict)
                else None
            )
            if (
                not isinstance(response_file, dict)
                or not isinstance(response_path, str)
                or not response_path
                or response_path.startswith("/")
                or "\\" in response_path
                or ".." in Path(response_path).parts
                or not isinstance(response_file.get("sha256"), str)
                or re.fullmatch(r"[0-9a-f]{64}", response_file["sha256"]) is None
            ):
                raise RuntimeError("clang-tidy response-file evidence is invalid")
        if len({item["path"] for item in response_files}) != len(response_files):
            raise RuntimeError("clang-tidy response-file evidence has duplicate paths")
        for diagnostic in diagnostics:
            if (
                not isinstance(diagnostic.get("path"), str)
                or not diagnostic["path"]
                or type(diagnostic.get("line")) is not int
                or diagnostic["line"] <= 0
                or type(diagnostic.get("column")) is not int
                or diagnostic["column"] <= 0
                or diagnostic.get("severity") not in {"warning", "error"}
                or not isinstance(diagnostic.get("message"), str)
                or not diagnostic["message"]
                or (
                    diagnostic.get("check") is not None
                    and (
                        not isinstance(diagnostic.get("check"), str)
                        or not diagnostic["check"]
                    )
                )
            ):
                raise RuntimeError("clang-tidy diagnostic record is invalid")
        identity = (path, variant)
        if identity in result_identities:
            raise RuntimeError("clang-tidy report has duplicate result identities")
        result_identities.add(identity)
        result_counts[generated] += 1
        result_paths[generated].add(path)
        result_failures += exit_code != 0
        result_diagnostics += diagnostic_count
        result_database_counts[compile_database] += 1
        suppression_identities: set[str] = set()
        for suppression in source_suppressions:
            if not isinstance(suppression, dict):
                raise RuntimeError("clang-tidy source suppression record is invalid")
            line = suppression.get("line")
            kind = suppression.get("kind")
            checks = suppression.get("checks")
            reason = suppression.get("reason")
            if (
                type(line) is not int
                or line <= 0
                or kind not in {
                    "NOLINT",
                    "NOLINTNEXTLINE",
                    "NOLINTBEGIN",
                    "NOLINTEND",
                }
                or not isinstance(checks, list)
                or not checks
                or any(
                    not isinstance(check, str)
                    or re.fullmatch(r"[a-z0-9_.-]+", check) is None
                    for check in checks
                )
                or len(set(checks)) != len(checks)
                or not isinstance(reason, str)
                or not reason.strip()
            ):
                raise RuntimeError("clang-tidy source suppression record is invalid")
            suppression_identity = json.dumps(
                suppression,
                sort_keys=True,
                separators=(",", ":"),
            )
            if suppression_identity in suppression_identities:
                raise RuntimeError(
                    "clang-tidy source suppression record is duplicated"
                )
            suppression_identities.add(suppression_identity)
            result_suppressions.add((path, suppression_identity))
    if any(
        result_database_counts[path] > contributions[path]
        for path in compile_database_paths
    ):
        raise RuntimeError(
            "clang-tidy result database provenance exceeds availability"
        )
    if (
        bool(result_paths[False] & result_paths[True])
        or result_counts[False] != report["authored_count"]
        or result_counts[True] != report["generated_count"]
        or len(result_paths[False]) != report["authored_path_count"]
        or len(result_paths[True]) != report["generated_path_count"]
        or result_failures != report["failure_count"]
        or result_diagnostics != report["diagnostic_count"]
        or len(result_suppressions) != report["source_suppression_count"]
    ):
        raise RuntimeError("clang-tidy report result aggregates are inconsistent")
    if report_scope == "full":
        if (
            report["selected_count"] != report["available_count"]
            or report["selected_path_count"] != report["available_path_count"]
        ):
            raise RuntimeError(
                "full clang-tidy report does not cover every available unit")
        if (
            report["available_authored_count"] == 0
            or report["available_generated_count"] == 0
        ):
            raise RuntimeError(
                "full clang-tidy report requires authored and generated coverage")
        if len(compile_databases) < 2:
            raise RuntimeError(
                "full clang-tidy report requires primary and generator databases")
        generator_database_paths = {
            path
            for path in compile_database_paths
            if re.fullmatch(
                r"codegen/generator-build(?:-[a-z0-9][a-z0-9-]*)?/"
                r"compile_commands\.json",
                path,
            ) is not None
        }
        if result_database_counts != contributions:
            raise RuntimeError(
                "full clang-tidy result database provenance is inconsistent"
            )
        if (
            contributions.get("compile_commands.json", 0) == 0
            or not any(contributions[path] > 0 for path in generator_database_paths)
        ):
            raise RuntimeError(
                "full clang-tidy report lacks primary or generator database "
                "contribution"
            )
    return {
        "scope": report_scope,
        **{field_name: report[field_name] for field_name in required_integer_fields},
        "compile_database_count": len(compile_databases),
        "compile_database_contributions": contributions,
        "optimization_policy": optimization_policy,
        "policy_sha256": report.get("policy_sha256"),
        "toolchain": report.get("toolchain"),
    }

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


def _require_control_deadline(
        deadline: float | None, operation: str) -> None:
    """Stop cooperative receipt/control work at its absolute wall deadline."""
    if deadline is not None and time.time() >= deadline:
        raise TimeoutError(f"deadline reached during {operation}")


def _remaining_control_timeout(
        deadline: float | None, maximum_seconds: float,
        operation: str) -> float:
    _require_control_deadline(deadline, operation)
    if deadline is None:
        return maximum_seconds
    return min(maximum_seconds, max(0.001, deadline - time.time()))


def _deadline_sha256_file(path: Path, deadline: float | None) -> str:
    """Hash one file while cooperatively enforcing a receipt deadline."""
    if deadline is None:
        return sha256_file(path)
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            _require_control_deadline(deadline, f"hashing {path}")
            chunk = handle.read(1024 * 1024)
            if not chunk:
                return digest.hexdigest()
            digest.update(chunk)


def _source_graph_fingerprint(
        repository_root: Path, *, deadline: float | None = None) -> str:
    """Hash the checked-out build inputs without hashing generated outputs.

    The package snapshot's explicit lifted-source allowlist is the single
    repository closure authority. This build-receipt view deliberately removes
    documentation and research so their edits cannot invalidate a successful
    production binary receipt. Tracked deletions are represented explicitly,
    while ignored generated output never enters the digest.
    """
    _require_control_deadline(deadline, "source-graph fingerprint setup")
    repository_root = repository_root.resolve(strict=True)
    try:
        completed = subprocess.run(
            [
                "git", "-C", str(repository_root), "ls-files", "-z",
                "--cached", "--others", "--exclude-standard",
                "--", *_BUILD_SOURCE_FINGERPRINT_PATHS,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=_remaining_control_timeout(
                deadline, 60.0, "source-graph enumeration"),
        )
    except subprocess.TimeoutExpired as exc:
        if deadline is not None and time.time() >= deadline:
            raise TimeoutError(
                "deadline reached during source-graph enumeration") from exc
        raise RuntimeError("source-graph enumeration timed out") from exc
    if completed.returncode != 0:
        raise RuntimeError(
            "could not enumerate source graph: "
            + completed.stderr.decode("utf-8", errors="replace").strip())
    digest = hashlib.sha256()
    for raw_path in sorted(completed.stdout.split(b"\0")):
        _require_control_deadline(deadline, "source-graph fingerprint")
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
                _require_control_deadline(deadline, "source-graph fingerprint")
                file_digest.update(chunk)
        digest.update(file_digest.digest())
        digest.update(b"\0")
    return digest.hexdigest()

def _git_commit(
        repository_root: Path, ref: str = "HEAD",
        *, deadline: float | None = None) -> str:
    try:
        completed = subprocess.run(
            ["git", "-C", str(repository_root), "rev-parse", "--verify",
             f"{ref}^{{commit}}"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            text=True,
            timeout=_remaining_control_timeout(
                deadline, 30.0, "source commit validation"),
        )
    except subprocess.TimeoutExpired as exc:
        if deadline is not None and time.time() >= deadline:
            raise TimeoutError(
                "deadline reached during source commit validation") from exc
        raise RuntimeError("source commit validation timed out") from exc
    if completed.returncode != 0:
        raise RuntimeError(
            f"cannot resolve git ref {ref!r}: {completed.stderr.strip()}")
    commit = completed.stdout.strip()
    if not re.fullmatch(r"[0-9a-f]{40,64}", commit):
        raise RuntimeError(f"git returned malformed commit id {commit!r}")
    return commit

def _build_configuration_evidence(
        build_dir: Path, *, deadline: float | None = None) -> dict[str, object]:
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
        "cmake_cache_sha256": _deadline_sha256_file(cache_path, deadline),
        "build_ninja_sha256": _deadline_sha256_file(ninja_path, deadline),
        "compile_commands_sha256": (
            _deadline_sha256_file(
                build_dir / "compile_commands.json", deadline)
            if (build_dir / "compile_commands.json").is_file()
            else None
        ),
        "configure_stamp_sha256": (
            _deadline_sha256_file(stamp_path, deadline)
            if stamp_path.is_file() else None
        ),
        "compiler_contract": {
            key: _cmake_cache_value(build_dir, key)
            for key in cache_keys
        },
    }

def _stable_artifact_digest(
        path: Path, *, deadline: float | None = None
) -> tuple[os.stat_result, str]:
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
            _require_control_deadline(deadline, f"hashing artifact {path}")
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
        mode: str, build_dir: Path, *, deadline: float | None = None
) -> list[dict[str, object]]:
    """Seal every directly executed mode command for no-rebuild reuse."""
    bin_dir = build_dir / MODES[mode].binary_subdir
    evidence: list[dict[str, object]] = []
    for command in MODES[mode].commands:
        _require_control_deadline(deadline, "runtime-artifact validation")
        if not command:
            continue
        path = bin_dir / command[0]
        try:
            metadata, digest = _stable_artifact_digest(
                path, deadline=deadline)
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
        debug: bool,
        deadline: float | None = None) -> list[dict[str, object]]:
    """Seal every build-tree artifact a receipt can execute/install/inspect.

    Command binaries alone are insufficient authority: receipt-pinned runs
    may install extension DSOs through CMake, while receipt-pinned inspection
    may consume linked DSOs and pre-LTO ``.s``/``.bc`` listings.  Keep the
    selection intentionally broader than the immediate command list so a
    changed install plan or locally linked dependency also invalidates reuse.
    """
    _require_control_deadline(deadline, "authorized-artifact validation")
    build_dir = build_dir.resolve(strict=True)
    bin_dir = (build_dir / MODES[mode].binary_subdir).resolve(strict=False)
    selected: set[Path] = set()
    library_aliases: dict[
        Path, _ReceiptVcpkgLibraryAliasSnapshot
    ] = {}

    def select_regular(path: Path, *, required: bool = False) -> bool:
        _require_control_deadline(deadline, "authorized-artifact selection")
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
        _require_control_deadline(deadline, "authorized-artifact selection")
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
            _require_control_deadline(deadline, "authorized-artifact selection")
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
        _require_control_deadline(deadline, "authorized-artifact selection")
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
            _require_control_deadline(deadline, "authorized-artifact walk")
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
        metadata, digest = _stable_artifact_digest(
            path, deadline=deadline)
        item: dict[str, object] = {
            "relative_path": str(path.relative_to(build_dir)),
            "size": metadata.st_size,
            "mode": stat.S_IMODE(metadata.st_mode),
            "sha256": digest,
        }
        evidence.append(item)
        regular_evidence[path] = item

    for alias_path in sorted(library_aliases, key=str):
        _require_control_deadline(deadline, "authorized-artifact alias validation")
        expected = library_aliases[alias_path]
        before = _receipt_vcpkg_library_alias_snapshot(
            alias_path, build_dir)
        if before is None or before != expected:
            raise RuntimeError(
                "receipted vcpkg library alias changed while sealed: "
                f"{alias_path}")
        target_metadata, target_digest = _stable_artifact_digest(
            before.terminal_path, deadline=deadline)
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

def _build_artifact_counts(
        build_dir: Path, mode: str, *, deadline: float | None = None
) -> dict[str, int]:
    _require_control_deadline(deadline, "artifact-count validation")
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
        for path in generated_root.rglob("*"):
            _require_control_deadline(deadline, "generated-source count")
            if path.is_file() and path.suffix in {".cppm", ".cpp"}:
                generated_sources += 1
    compiled_objects = 0
    linked_artifacts = 0
    install_scripts = 0
    inspect_listings = 0
    runtime_artifacts = 0
    bin_dir = (build_dir / MODES[mode].binary_subdir).resolve(strict=False)
    if build_dir.is_dir():
        for path in build_dir.rglob("*"):
            _require_control_deadline(deadline, "artifact-count walk")
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
        _require_control_deadline(deadline, "runtime-artifact count")
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
        deadline: float | None = None,
) -> tuple[dict[str, object] | None, str | None]:
    _require_control_deadline(deadline, "build-receipt validation")
    receipt = _get_build_receipt(receipt_id)
    _require_control_deadline(deadline, "build-receipt load")
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
                source_repository_root, deadline=deadline)
        except RuntimeError as exc:
            return None, f"cannot validate build receipt source graph: {exc}"
        if observed_source != receipt.get("source_graph_fingerprint"):
            return None, "build receipt source graph no longer matches"
    try:
        observed_commit = _git_commit(
            source_repository_root, deadline=deadline)
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
        observed_configuration = _build_configuration_evidence(
            build_dir, deadline=deadline)
    except RuntimeError as exc:
        return None, str(exc)
    if observed_configuration != receipt.get("configuration"):
        return None, "build receipt compiler/configuration evidence no longer matches"
    try:
        observed_artifacts = _runtime_artifact_evidence(
            mode, build_dir, deadline=deadline)
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
                deadline=deadline,
            )
        )
    except RuntimeError as exc:
        return None, str(exc)
    if observed_authorized_artifacts != receipt.get(
            "authorized_artifacts"):
        return None, (
            "build receipt authorized install/inspect artifacts no longer "
            "match")
    if _build_artifact_counts(
            build_dir, mode, deadline=deadline) != receipt.get(
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
    _require_control_deadline(deadline, "build-receipt validation")
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
        deadline: float | None = None,
) -> dict[str, object]:
    _require_control_deadline(deadline, "run-receipt sealing")
    campaign_selected = _is_algorithm_matrix_campaign_config(bench_e2e_config)
    report_evidence: list[dict[str, object]] = []
    for path in report_files:
        _require_control_deadline(deadline, "run-report sealing")
        resolved = (
            _campaign_internal_regular_file(path, "campaign run receipt report")
            if campaign_selected else path.resolve(strict=True))
        metadata = resolved.lstat() if campaign_selected else resolved.stat()
        report_evidence.append({
            "path": str(resolved),
            "size": metadata.st_size,
            "sha256": _deadline_sha256_file(resolved, deadline),
        })
    config_evidence = None
    if bench_e2e_config is not None:
        config_path = (
            _campaign_regular_repository_file(
                bench_e2e_config, "campaign run receipt config")
            if campaign_selected
            else Path(bench_e2e_config).resolve(strict=True))
        config_evidence = {
            "path": str(config_path),
            "sha256": _deadline_sha256_file(config_path, deadline),
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
    _require_control_deadline(deadline, "run-receipt publication")
    _store_build_receipt(receipt)
    _require_control_deadline(deadline, "run-receipt publication")
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


_ALGORITHM_MATRIX_CAMPAIGN_ID = "bioseq-e2e-algorithm-matrix-local-v1"
_ALGORITHM_MATRIX_CAMPAIGN_DIGEST = (
    "011fbb1ef33a214ab80d3568c2341e8d0f8311f82e21a5955bdcb93b3c63b979")
_ALGORITHM_MATRIX_Q8_CAMPAIGN_ID = (
    "bioseq-e2e-algorithm-matrix-local-q8-v1")
_ALGORITHM_MATRIX_Q8_CAMPAIGN_DIGEST = (
    "e1efe2033ea95f729e147725d2807f9c94eccb83d9cd8711879034c09e70168a")
_ALGORITHM_MATRIX_Q1_CALIBRATION_MANIFEST = (
    "bench/bench_e2e_algorithm_matrix_q1_calibration_manifest.json")
# Updated only when the deterministic q1 panel sealer changes the manifest.
_ALGORITHM_MATRIX_Q1_CALIBRATION_MANIFEST_SHA256 = (
    "4c49297a10c59c44d10bf98d01365215a27c3d44c230ed546a1cb337ef01bf63")
_ALGORITHM_MATRIX_Q1_CALIBRATION_CONFIGS = (
    "bench/e2e_algorithm_matrix_q1_calibration_shards/"
    "bench_e2e_algorithm_matrix_q1_calibration_m2_d4_k15_w10_"
    "isolated_minimap2_no_index.json",
    "bench/e2e_algorithm_matrix_q1_calibration_shards/"
    "bench_e2e_algorithm_matrix_q1_calibration_m2_d4_k15_w10_no_index_"
    "scalar_scalar_minimap2_fragment_ksw2.json",
    "bench/e2e_algorithm_matrix_q1_calibration_shards/"
    "bench_e2e_algorithm_matrix_q1_calibration_m2_d4_k15_w10_no_index_"
    "scalar_scalar_minimap2_fragment_block_aligner.json",
    "bench/e2e_algorithm_matrix_q1_calibration_shards/"
    "bench_e2e_algorithm_matrix_q1_calibration_m2_d4_k15_w10_no_index_"
    "scalar_scalar_minimap2_sequence_ksw2.json",
    "bench/e2e_algorithm_matrix_q1_calibration_shards/"
    "bench_e2e_algorithm_matrix_q1_calibration_m2_d4_k15_w10_no_index_"
    "scalar_scalar_minimap2_sequence_block_aligner.json",
)
_ALGORITHM_MATRIX_DEADLINE_MANIFEST = (
    "bench/bench_e2e_algorithm_matrix_deadline_manifest.json")
_ALGORITHM_MATRIX_DEADLINE_MANIFEST_SHA256 = (
    "73cd6928e7430c46bff2836f0bb0820565e9d5c9b0011939ff4aad9fbe771fa0")
_ALGORITHM_MATRIX_DEADLINE_CAMPAIGN_ID = (
    "bioseq-e2e-algorithm-matrix-deadline-v1")
_ALGORITHM_MATRIX_DEADLINE_CAMPAIGN_DIGEST = (
    "3d03b3aea29a5418721ba5899c58a698f568ca10dddf9f00eaab42adf450a3b0")
_ALGORITHM_MATRIX_DEADLINE_DIRECTORY = (
    "bench/e2e_algorithm_matrix_deadline_shards")
_ALGORITHM_MATRIX_DEADLINE_NORMAL_TIMEOUT_SECONDS = 14_400
_ALGORITHM_MATRIX_DEADLINE_EXTENDED_TIMEOUT_SECONDS = 172_800
_Q1024_WHOLE_INVOCATION_TIMEOUT_SECONDS = 43_200
_Q1024_CLEANUP_RESERVE_SECONDS = 300
_Q1024_POSTGRES_APPLICATION_NAME_PREFIX = "bioseq-q1024-"
_ALGORITHM_MATRIX_Q1024_SINGLETON0058_EXTENDED_TIMEOUT_MANIFEST = (
    "bench/bench_e2e_algorithm_matrix_q1024_singleton0058_"
    "extended_timeout_authority.json")
_ALGORITHM_MATRIX_Q1024_SINGLETON0058_EXTENDED_TIMEOUT_MANIFEST_SHA256 = (
    "69e8d149406c8d84592be3847b6651959a0fc11660c2ea5d2f6869acf85a9a2d")
_ALGORITHM_MATRIX_Q1024_SINGLETON0058_EXTENDED_TIMEOUT_AUTHORITY_DIGEST = (
    "64a9d461d3bcafd8715891fbc92c7dd8a0f0da552b092499e3fc10c947351bd2")
_ALGORITHM_MATRIX_Q1024_SINGLETON0058_CONFIG = (
    "bench/e2e_algorithm_matrix_shards/"
    "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
    "0058_15aceb88564a0756.json")
_ALGORITHM_MATRIX_Q1024_SINGLETON0058_CONFIG_SHA256 = (
    "92d84607060b0fe7225f4b7d0f146b47777668f70d96db1c3127c929481e6af0")
_ALGORITHM_MATRIX_Q1024_SINGLETON0058_STABLE_KEY = (
    "bioseq-e2e-cell-v2:[\"bioseq-e2e-algorithm-matrix-local-v1\","
    "\"matrix_2gbp_dna4_k15_w10\",\"dna4\",2,15,10,\"no_index\","
    "\"bioseq\",\"minimizers_per_fragment\",\"bounded_nthash_simd\","
    "\"simd_minimap2\",\"block_aligner\"]")
_ALGORITHM_MATRIX_Q1024_SINGLETON0058_STABLE_KEY_SHA256 = (
    "15aceb88564a07568553f1e73a271786d919737f9f3c93653657d1847ef5322a")
_ALGORITHM_MATRIX_Q1024_SINGLETON0068_EXTENDED_TIMEOUT_MANIFEST = (
    "bench/bench_e2e_algorithm_matrix_q1024_singleton0068_"
    "extended_timeout_authority.json")
_ALGORITHM_MATRIX_Q1024_SINGLETON0068_EXTENDED_TIMEOUT_MANIFEST_SHA256 = (
    "01f036513541df316dfccb374bc2e97907d82e51e4b08870fc47d4e432717af0")
_ALGORITHM_MATRIX_Q1024_SINGLETON0068_EXTENDED_TIMEOUT_AUTHORITY_DIGEST = (
    "7d810d0450813b5ceb3c6db764c5ad269ed21836c519b0b58aa4a21c3cfdb0b1")
_ALGORITHM_MATRIX_Q1024_SINGLETON0068_CONFIG = (
    "bench/e2e_algorithm_matrix_shards/"
    "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
    "0068_2496323c88e8add2.json")
_ALGORITHM_MATRIX_Q1024_SINGLETON0068_CONFIG_SHA256 = (
    "7cbedf6a5ccc42f304e37b6a1faffbc4c3e19d6fb592c237ecdc3a141ad14b44")
_ALGORITHM_MATRIX_Q1024_SINGLETON0068_STABLE_KEY = (
    "bioseq-e2e-cell-v2:[\"bioseq-e2e-algorithm-matrix-local-v1\","
    "\"matrix_2gbp_dna4_k15_w10\",\"dna4\",2,15,10,\"no_index\","
    "\"bioseq\",\"minimizers_per_fragment\",\"bounded_nthash_simd\","
    "\"range_minimum_query\",\"block_aligner\"]")
_ALGORITHM_MATRIX_Q1024_SINGLETON0068_STABLE_KEY_SHA256 = (
    "2496323c88e8add288fbd281ec0df05618b57a4b7425473db3092894d37cfa61")
_ALGORITHM_MATRIX_Q1024_SINGLETON0068_HARD_STOP_AT = (
    "2026-09-02T18:30:00+02:00")
_ALGORITHM_MATRIX_Q1024_SINGLETON0068_HARD_STOP_EPOCH = 1_788_366_600
_ALGORITHM_MATRIX_DEADLINE_V2_MANIFEST = (
    "bench/bench_e2e_algorithm_matrix_deadline_v2_manifest.json")
_ALGORITHM_MATRIX_DEADLINE_V2_MANIFEST_SHA256 = (
    "bf2abc14dbda84079d6384e330b1fdc752e1754360cc61d9fec42af1e2d1b31c")
_ALGORITHM_MATRIX_DEADLINE_V2_CHECKPOINT_SHA256 = (
    "401751c75a95e8814d0a1a62d289b093c9a097b43789dc55ec89b4f4b86e4053")
_ALGORITHM_MATRIX_DEADLINE_V2_CAMPAIGN_ID = (
    "bioseq-e2e-algorithm-matrix-deadline-v2")
_ALGORITHM_MATRIX_DEADLINE_V2_CAMPAIGN_DIGEST = (
    "44a85360a5e975ec8a6d70ac0b06486c51d748770c2ada46106d2ccc5db4728a")
_ALGORITHM_MATRIX_DEADLINE_V2_DIRECTORY = (
    "bench/e2e_algorithm_matrix_deadline_v2_shards")
_ALGORITHM_MATRIX_DEADLINE_V2_EXPECTED_SHARDS = 60
_ALGORITHM_MATRIX_DEADLINE_V2_EXPECTED_RAW_RECORDS = 300
_ALGORITHM_MATRIX_DEADLINE_V2_DEADLINE_SECONDS = 604_800
_ALGORITHM_MATRIX_DEADLINE_V2_ESTIMATED_WALL_SECONDS = 586_467
_ALGORITHM_MATRIX_DEADLINE_V2_LINEAGE = {
    "parent_campaign_id": _ALGORITHM_MATRIX_DEADLINE_CAMPAIGN_ID,
    "parent_campaign_digest": _ALGORITHM_MATRIX_DEADLINE_CAMPAIGN_DIGEST,
    "parent_manifest": _ALGORITHM_MATRIX_DEADLINE_MANIFEST,
    "parent_manifest_sha256": _ALGORITHM_MATRIX_DEADLINE_MANIFEST_SHA256,
    "parent_shard_directory": _ALGORITHM_MATRIX_DEADLINE_DIRECTORY,
    "parent_shard_tree_sha256": (
        "297634d7b22f4ca05bef640282acd09c92e17cd23b544ce624e1c59ba4838270"),
    "parent_expected_shards": 59,
    "parent_expected_raw_records": 295,
    "parent_artifact_policy": "immutable_exact_parent_authority",
}
_ALGORITHM_MATRIX_STABLE_KEY_FIELDS = [
    "campaign_id",
    "dataset",
    "alphabet",
    "target_size_gbp",
    "kmer_size",
    "window_size",
    "index_mode",
    "engine",
    "bioseq_index_name",
    "requested_minimizer_algorithm",
    "requested_chaining_algorithm",
    "requested_base_alignment_algorithm",
]
_ALGORITHM_MATRIX_EXACT_BENCH_FILTER = {"benchmarks": ["bench_e2e"]}
_ALGORITHM_MATRIX_EXACT_BENCH_FLAGS = ["--filter", "benchmark=bench_e2e"]
_ALGORITHM_MATRIX_RUNTIME_ENVIRONMENT = {
    "BIOSEQ_E2E_KEEP_ARTIFACTS": "0",
    "BIOSEQ_E2E_RETAIN_MATRIX_JUST_INDEX_TARGET": "0",
    "BIOSEQ_E2E_SKIP_INDEX_BASELINE_SETUP": "0",
    "BIOSEQ_E2E_SKIP_MINIMAP2_BASELINE_SETUP": "0",
    "BIOSEQ_E2E_JUST_INDEX_SETUP_ONLY": "",
    "BIOSEQ_E2E_JUST_INDEX_SETUP_PIPELINE_MODE": "task_parallel",
}
_ALGORITHM_MATRIX_CAMPAIGN_PROFILES = (
    {
        "name": "q1024",
        "campaign_id": _ALGORITHM_MATRIX_CAMPAIGN_ID,
        "campaign_digest": _ALGORITHM_MATRIX_CAMPAIGN_DIGEST,
        "monolith": "bench/bench_e2e_algorithm_matrix_campaign.json",
        "shard_directory": "bench/e2e_algorithm_matrix_shards",
        "shard_pattern": (
            r"bench/e2e_algorithm_matrix_shards/"
            r"bench_e2e_algorithm_matrix_campaign_shard_[a-z0-9_]+\.json"),
        "manifest": "bench/bench_e2e_algorithm_matrix_campaign_contract.json",
        "singleton_partition_manifest": (
            "bench/bench_e2e_algorithm_matrix_q1024_singleton_partition_manifest.json"),
        "singleton_partition_manifest_sha256": (
            "6bfe0681b46087860b7d873cd868da8e70be5ecab85b817e6979a29fce6f2c3a"),
        "packed_partition_manifest": (
            "bench/bench_e2e_algorithm_matrix_q1024_packed_partition_manifest.json"),
        "packed_partition_manifest_sha256": (
            "65d31b8b3aa49400598d80d50641b2a9a4b1f78c08c85c8a33afbef761fd891b"),
        "packed_pattern": (
            r"bench/e2e_algorithm_matrix_q1024_packed_shards/"
            r"bench_e2e_algorithm_matrix_q1024_packed_[a-z0-9_]+\.json"),
        "singleton_pattern": (
            r"bench/e2e_algorithm_matrix_shards/"
            r"bench_e2e_algorithm_matrix_campaign_shard_singleton_"
            r"[0-9]{4}_[0-9a-f]{16}\.json"),
        "required_shards": 88,
        "ledger": "ALL_RESULTS.yaml",
        "ledger_schema_version": 6,
        "legacy_empty_ledger_schema_version": 4,
        "ledger_contract_schema": "bioseq.e2e.algorithm-matrix-all-results",
        "ledger_contract_version": 4,
        "legacy_empty_ledger_contract_version": 2,
        "query_prefix_proof": None,
    },
    {
        "name": "q8",
        "campaign_id": _ALGORITHM_MATRIX_Q8_CAMPAIGN_ID,
        "campaign_digest": _ALGORITHM_MATRIX_Q8_CAMPAIGN_DIGEST,
        "monolith": "bench/bench_e2e_algorithm_matrix_q8_campaign.json",
        "shard_directory": "bench/e2e_algorithm_matrix_q8_shards",
        "shard_pattern": (
            r"bench/e2e_algorithm_matrix_q8_shards/"
            r"bench_e2e_algorithm_matrix_q8_campaign_shard_[a-z0-9_]+\.json"),
        "manifest": (
            "bench/bench_e2e_algorithm_matrix_q8_campaign_manifest.json"),
        "singleton_partition_manifest": None,
        "singleton_partition_manifest_sha256": None,
        "packed_partition_manifest": None,
        "packed_partition_manifest_sha256": None,
        "packed_pattern": None,
        "singleton_pattern": None,
        "required_shards": 256,
        "ledger": "Q8_RESULTS.yaml",
        "ledger_schema_version": 1,
        "legacy_empty_ledger_schema_version": None,
        "ledger_contract_schema": (
            "bioseq.e2e.algorithm-matrix-q8-working-results"),
        "ledger_contract_version": 1,
        "legacy_empty_ledger_contract_version": None,
        "query_prefix_proof": (
            "bench/bench_e2e_algorithm_matrix_q8_query_prefix_proof.json"),
    },
)

_ALGORITHM_MATRIX_OBSERVED_RUNG_PROFILES = tuple(
    {
        "name": "observed_rung",
        "query_count": query_count,
        "campaign_digest": campaign_digest,
        "manifest": (
            f"bench/bench_e2e_algorithm_matrix_q{query_count}_"
            "observed_campaign_manifest.json"),
        "monolith": monolith,
        "shard_directory": (
            f"bench/e2e_algorithm_matrix_q{query_count}_observed_shards"),
        "shard_pattern": (
            rf"bench/e2e_algorithm_matrix_q{query_count}_observed_shards/"
            rf"bench_e2e_algorithm_matrix_q{query_count}_observed_shard_"
            r"[a-z0-9_]+\.json"),
        "singleton_partition_manifest": (
            f"bench/bench_e2e_algorithm_matrix_q{query_count}_"
            "observed_singleton_partition_manifest.json"),
        "singleton_partition_manifest_sha256": singleton_manifest_sha256,
        "singleton_directory": (
            f"bench/e2e_algorithm_matrix_q{query_count}_observed_singletons"),
        "singleton_pattern": (
            rf"bench/e2e_algorithm_matrix_q{query_count}_observed_singletons/"
            rf"bench_e2e_algorithm_matrix_q{query_count}_observed_singleton_"
            r"[0-9]{4}_[0-9a-f]{16}\.json"),
    }
    for query_count, campaign_digest, monolith, singleton_manifest_sha256 in (
        (
            2,
            "e73930afc46ea87a4328e41e43a728f9ff7d827a5f6486bfe85a4f5e28ed3c7c",
            "bench/bench_e2e_algorithm_matrix_q2_campaign.json",
            "4ee3826dd40105a171ceeb1edf1fd8410d73ecda981d89daa7e424a9840ed14d",
        ),
        (
            8,
            "f4dde934669b83e775f93aa52f8f7321b64392e3e9889cc13be050e69b08a792",
            "bench/bench_e2e_algorithm_matrix_q8_observed_campaign.json",
            "0426c9baccdd5301ef3290d79521eb7e95f259fae62549fd66c94d155d6cd9e5",
        ),
        (
            16,
            "7d503a21a8dca3524de339485cf82934720a5e526c53b4c4cdc51732a9a28505",
            "bench/bench_e2e_algorithm_matrix_q16_campaign.json",
            "fa1a9d90aa0805d1fe3c4dced93faa0fc525409266b57fc5e186ce33fca5fd53",
        ),
        (
            32,
            "d5dd19e67e2eb2315947a1148ad7eac475db95124d1af84c433416dc4e9160ee",
            "bench/bench_e2e_algorithm_matrix_q32_campaign.json",
            "103c81eb16d43d2aacf50bbc84c7cb4ab8d6ded0003c2961d30f282914d0284d",
        ),
        (
            64,
            "f1f75a12fc29816152d4acc7493a4c9b8c09e994bc04be8dc7c0aec1490e365a",
            "bench/bench_e2e_algorithm_matrix_q64_campaign.json",
            "419f84df201df48c58ac8bf0481c924d4e81272de0da1f45b98013dd0d6e8e10",
        ),
        (
            128,
            "c7778fe98036dfd78dc26a94a0511d386216999adb5ebd7dfd107c873e233817",
            "bench/bench_e2e_algorithm_matrix_q128_campaign.json",
            "4b797bc3e083846aeb87833b6c08f674926880675261da54453948d775f3e348",
        ),
        (
            256,
            "60c24a00f29e46c3ecaa5c7c0edf3056cce8b33f338d83bb966b26f1790f9443",
            "bench/bench_e2e_algorithm_matrix_q256_campaign.json",
            "c751bd020190e09486810624fd9ac49caed8037b54529789fafe9283c897ca9a",
        ),
        (
            512,
            "399cf6416e0d687633a7dfc0687963b88cec11db7e05482414a29e715eff2c48",
            "bench/bench_e2e_algorithm_matrix_q512_campaign.json",
            "11b3aca82bc385e88286dd55dd9e9de4655b5655d5685271f56c2747a0abb907",
        ),
        (
            1024,
            "df2d45776d637271f453cca213027b03f03ede56c887a19fa3e8d736bd2d0857",
            "bench/bench_e2e_algorithm_matrix_q1024_observed_campaign.json",
            "4913ed26a424736cef6cc86e8749eabcc6266957ac901c9a7261fe1a78c19ca1",
        ),
    )
)


def _legacy_q1024_matrix_contract() -> dict[str, object]:
    return {
        "target_size_gbp": [2, 4],
        "alphabet": ["dna4", "dna5"],
        "window_kmer": [
            {"window_size": 10, "kmer_size": 15},
            {"window_size": 15, "kmer_size": 20},
        ],
        "query_count": 1024,
        "cigar_projection": True,
        "mapq_projection": True,
        "prefilter_enabled": False,
        "index_mode": ["no_index", "just_index"],
        "bioseq_index_name": [
            "minimizers_per_fragment", "minimizers_per_sequence"],
        "repetitions_per_cell": 5,
        "minimizer_algorithms": [
            "scalar", "simd_minimap2_reference", "two_stack_nthash",
            "vulkan_minimap_integer", "bounded_nthash_simd",
        ],
        "chaining_algorithms": [
            "scalar_minimap2", "simd_minimap2", "range_minimum_query",
            "vulkan_tiled_dynamic_programming",
        ],
        "base_alignment_algorithms": ["ksw2", "block_aligner"],
        "explicitly_excluded_base_alignment_algorithms": ["ksw2_batched"],
        "bioseq_result_cells": 1280,
        "minimap2_baseline_result_cells": 16,
        "expected_result_cells": 1296,
        "expected_raw_records": 6480,
    }


def _q1024_deadline_v2_checkpoint_is_exact(value: object) -> bool:
    """Worker-side recognition of the immutable nonpublication checkpoint."""
    if not isinstance(value, dict):
        return False
    try:
        raw = json.dumps(
            value, sort_keys=True, separators=(",", ":"),
            ensure_ascii=True, allow_nan=False).encode("utf-8")
    except (TypeError, ValueError):
        return False
    authority = value.get("authority")
    deadline = (
        authority.get("deadline_v2_manifest")
        if isinstance(authority, dict) else None)
    coverage = value.get("coverage")
    deadline_coverage = (
        coverage.get("deadline_v2")
        if isinstance(coverage, dict) else None)
    literal_coverage = (
        coverage.get("literal_q1024")
        if isinstance(coverage, dict) else None)
    anchors = value.get("q1024_anchors")
    return (
        hashlib.sha256(raw).hexdigest()
            == _ALGORITHM_MATRIX_DEADLINE_V2_CHECKPOINT_SHA256
        and value.get("schema")
            == "bioseq.e2e.deadline-v2-audited-checkpoint"
        and value.get("version") == 1
        and value.get("status") == "INCOMPLETE_LITERAL_Q1024_COVERAGE"
        and value.get("updated_through") == "s058"
        and value.get("active_excluded") == "s059"
        and isinstance(deadline, dict)
        and deadline.get("path") == _ALGORITHM_MATRIX_DEADLINE_V2_MANIFEST
        and deadline.get("sha256")
            == _ALGORITHM_MATRIX_DEADLINE_V2_MANIFEST_SHA256
        and isinstance(deadline_coverage, dict)
        and deadline_coverage.get("terminal_audited_shards") == 59
        and deadline_coverage.get("total_durable_terminal_records") == 295
        and isinstance(literal_coverage, dict)
        and literal_coverage.get("expected_cells") == 1296
        and literal_coverage.get("accepted_cells") == 3
        and literal_coverage.get("complete") is False
        and isinstance(anchors, list)
        and [anchor.get("shard") for anchor in anchors
             if isinstance(anchor, dict)] == ["s056", "s057", "s058"]
    )


def _legacy_q1024_ledger_is_exactly_empty(
        document: object,
        campaign_id: str,
        campaign_digest: str,
) -> bool:
    """Worker-side equivalent of the aggregator's legacy-empty predicate."""
    legacy_fields = {
        "schema_version", "contract", "campaign", "matrix_contract", "wiring",
        "aggregation_state", "results", "calibration", "execution_environment",
        "publication_methodology", "publication_state", "status",
    }
    if (not isinstance(document, dict)
            or set(document) not in {
                frozenset(legacy_fields),
                frozenset(legacy_fields | {"deadline_v2_checkpoint"}),
            }):
        return False
    if ("deadline_v2_checkpoint" in document
            and not _q1024_deadline_v2_checkpoint_is_exact(
                document["deadline_v2_checkpoint"])):
        return False
    contract = document.get("contract")
    if (document.get("schema_version") != 4
            or not isinstance(contract, dict)
            or set(contract) != {
                "schema", "version", "campaign_id", "campaign_digest",
                "campaign_contract", "campaign_contract_sha256", "encoding",
                "raw_framework_schema", "raw_payload_schema",
                "raw_payload_schema_version", "old_or_mixed_schema_policy",
                "minimum_accepted_repeats", "stable_key_schema",
                "stable_key_fields", "stable_key_encoding", "repeat_identity",
                "duplicate_policy", "idempotency_policy", "metric_a", "metric_b",
                "timing_acceptance", "clock_semantics", "match_parity",
                "result_row_timing_fields", "non_timing_result_fields",
            }
            or contract.get("schema")
                != "bioseq.e2e.algorithm-matrix-all-results"
            or contract.get("version") != 2
            or contract.get("campaign_id") != campaign_id
            or contract.get("campaign_digest") != campaign_digest
            or contract.get("raw_framework_schema") != 1
            or contract.get("raw_payload_schema") != "bioseq.e2e.raw-sample"
            or contract.get("raw_payload_schema_version") != 11
            or contract.get("minimum_accepted_repeats") != 5
            or contract.get("stable_key_fields")
                != _ALGORITHM_MATRIX_STABLE_KEY_FIELDS
            or contract.get("result_row_timing_fields") != [
                "run_count", "outer_e2e_ns_p50",
                "outer_e2e_minus_cigar_or_mapq_union_ns_p50",
            ]):
        return False
    if document.get("results") != [] or document.get("status") != "in_progress":
        return False
    if document.get("matrix_contract") != _legacy_q1024_matrix_contract():
        return False
    if (not isinstance(document.get("wiring"), dict)
            or set(document["wiring"]) != {
                "campaign_contract", "shard_generator", "serial_shard_directory",
                "serial_shard_count", "builder_config_override",
                "mcp_run_only_example", "automatic_consumer",
                "manual_consumer_invocation", "proof_policy", "shard_resume",
            }) \
            or not isinstance(document.get("execution_environment"), dict):
        return False

    state = document.get("aggregation_state")
    if not isinstance(state, dict) or set(state) != {
        "campaign_id", "campaign_digest", "canonical_result_cell_count",
        "canonical_membership_complete", "canonical_statuses_valid",
        "missing_canonical_cell_count", "processed_reports", "report_count",
        "raw_record_count", "result_cell_count", "accepted_cell_count",
        "rejected_cell_count", "invalid_cell_count", "pending_cell_count",
    }:
        return False
    if (state.get("campaign_id") != campaign_id
            or state.get("campaign_digest") != campaign_digest
            or state.get("canonical_membership_complete") is not False
            or state.get("canonical_statuses_valid") is not True
            or state.get("processed_reports") != []):
        return False
    exact_state_counts = {
        "canonical_result_cell_count": 1296,
        "missing_canonical_cell_count": 1296,
        "report_count": 0,
        "raw_record_count": 0,
        "result_cell_count": 0,
        "accepted_cell_count": 0,
        "rejected_cell_count": 0,
        "invalid_cell_count": 0,
        "pending_cell_count": 0,
    }
    if any(type(state.get(field)) is not int or state[field] != expected
           for field, expected in exact_state_counts.items()):
        return False

    calibration = document.get("calibration")
    if not isinstance(calibration, dict) or set(calibration) != {
        "schema_version", "publication_eligible", "separation_policy", "q1", "q8",
    } or calibration.get("schema_version") != 1 \
            or calibration.get("publication_eligible") is not False \
            or not isinstance(calibration.get("separation_policy"), str):
        return False
    q1 = calibration.get("q1")
    q8 = calibration.get("q8")
    if (not isinstance(q1, dict)
            or set(q1) != {
                "status", "query_count", "reason", "run_count",
                "outer_e2e_ns_p50",
                "outer_e2e_minus_cigar_or_mapq_union_ns_p50", "parity_comment",
            }
            or q1.get("status") != "excluded"
            or q1.get("query_count") != 1
            or q1.get("reason") != "query_count_not_significant"
            or type(q1.get("run_count")) is not int
            or q1.get("run_count") != 5
            or type(q1.get("outer_e2e_ns_p50")) is not int
            or type(q1.get(
                "outer_e2e_minus_cigar_or_mapq_union_ns_p50")) is not int
            or not isinstance(q1.get("parity_comment"), str)
            or not isinstance(q8, dict)
            or set(q8) != {
                "status", "query_count", "accepted", "running", "pending_next",
            }
            or q8.get("status") not in {"in_progress", "complete"}
            or q8.get("query_count") != 8):
        return False
    calibration_lists: list[list[object]] = []
    for field in ("accepted", "running", "pending_next"):
        value = q8.get(field)
        if not isinstance(value, list) or any(not isinstance(item, dict) for item in value):
            return False
        calibration_lists.append(value)
    calibration_ids = [
        item.get("id") for values in calibration_lists for item in values
    ]
    if (any(not isinstance(value, str) or not value for value in calibration_ids)
            or len(calibration_ids) != len(set(calibration_ids))):
        return False
    accepted_calibration, running_calibration, pending_calibration = (
        len(values) for values in calibration_lists)
    expected_q8_status = (
        "complete" if running_calibration == 0 and pending_calibration == 0
        else "in_progress")
    if q8.get("status") != expected_q8_status:
        return False

    campaign = document.get("campaign")
    if not isinstance(campaign, dict) or set(campaign) != {
        "name", "campaign_id", "campaign_digest", "status", "last_updated_at",
        "execution_scope", "remote_execution_invoked", "measurements_collected",
        "benchmark_runs_started", "accepted_calibration_records",
        "excluded_calibration_records", "active_calibration_runs",
        "pending_calibration_runs", "results_are_publishable", "no_results_reason",
    }:
        return False
    if (campaign.get("name") != "local_e2e_algorithm_matrix"
            or campaign.get("campaign_id") != campaign_id
            or campaign.get("campaign_digest") != campaign_digest
            or campaign.get("status") != "in_progress"
            or not isinstance(campaign.get("last_updated_at"), str)
            or not campaign["last_updated_at"]
            or campaign.get("execution_scope") != "local_campaign"
            or campaign.get("remote_execution_invoked") is not False
            or campaign.get("results_are_publishable") is not False
            or not isinstance(campaign.get("no_results_reason"), str)
            or not campaign["no_results_reason"]):
        return False
    exact_campaign_counts = {
        "measurements_collected": 0,
        "benchmark_runs_started": 1 + accepted_calibration + running_calibration,
        "accepted_calibration_records": accepted_calibration,
        "excluded_calibration_records": 1,
        "active_calibration_runs": running_calibration,
        "pending_calibration_runs": pending_calibration,
    }
    if any(type(campaign.get(field)) is not int or campaign[field] != expected
           for field, expected in exact_campaign_counts.items()):
        return False

    publication_state = document.get("publication_state")
    if publication_state != {
        "status": "pending",
        "query_count": 1024,
        "result_collection": "results",
        "result_count": 0,
        "calibration_results_included": False,
    } or type(publication_state.get("result_count")) is not int \
            or publication_state.get("calibration_results_included") is not False:
        return False
    methodology = document.get("publication_methodology")
    if not isinstance(methodology, dict) or set(methodology) != {
        "status", "query_count", "target_size_gbp", "alphabet", "window_kmer",
        "index_mode", "cigar_projection", "mapq_projection", "prefilter_enabled",
        "engines", "minimizer_algorithms", "chaining_algorithms",
        "base_alignment_algorithms", "excluded_base_alignment_algorithms",
        "minimum_run_count", "reported_timing_fields_only", "metric_a", "metric_b",
        "correctness_requirement", "algorithm_audit_requirement",
        "calibration_inference_policy",
    }:
        return False
    if (methodology.get("status") != "pending"
            or methodology.get("query_count") != 1024
            or methodology.get("cigar_projection") is not True
            or methodology.get("mapq_projection") is not True
            or methodology.get("prefilter_enabled") is not False
            or methodology.get("base_alignment_algorithms")
                != ["ksw2", "block_aligner"]
            or methodology.get("excluded_base_alignment_algorithms")
                != ["ksw2_batched"]
            or methodology.get("minimum_run_count") != 5
            or methodology.get("reported_timing_fields_only") != [
                "run_count", "outer_e2e_ns_p50",
                "outer_e2e_minus_cigar_or_mapq_union_ns_p50",
            ]):
        return False
    return True


_BENCHMARK_E2E_CONFIG_IDENTITY_ENV = "BIOSEQ_E2E_CONFIG_OVERRIDE"
_BENCHMARK_E2E_CONFIG_OPEN_ENV = "BIOSEQ_E2E_CONFIG_OPEN_PATH"
_BENCHMARK_E2E_CONFIG_ENVIRONMENT_KEYS = (
    _BENCHMARK_E2E_CONFIG_IDENTITY_ENV,
    _BENCHMARK_E2E_CONFIG_OPEN_ENV,
)


def _canonical_campaign_relative_path(value: str, label: str) -> str:
    """Reject lexical/canonical aliases before any path resolution."""
    if (not isinstance(value, str) or not value
            or value.startswith("/") or "\\" in value
            or any(ord(character) < 0x20 or ord(character) == 0x7F
                   for character in value)):
        raise ValueError(f"{label} must be a canonical repository-relative path")
    components = value.split("/")
    if any(component in {"", ".", ".."} for component in components):
        raise ValueError(f"{label} contains a lexical path alias")
    if Path(value).as_posix() != value:
        raise ValueError(f"{label} is not in canonical repository form")
    return value


def _campaign_regular_repository_file(value: str, label: str) -> Path:
    """Require real directory components and one regular non-symlink leaf."""
    relative = _canonical_campaign_relative_path(value, label)
    current = REPOSITORY_ROOT
    components = relative.split("/")
    for index, component in enumerate(components):
        current = current / component
        metadata = current.lstat()
        if stat.S_ISLNK(metadata.st_mode):
            raise ValueError(f"{label} may not traverse a symlink: {current}")
        if index + 1 == len(components):
            if not stat.S_ISREG(metadata.st_mode):
                raise ValueError(
                    f"{label} must be a regular non-symlink file: {current}")
        elif not stat.S_ISDIR(metadata.st_mode):
            raise ValueError(
                f"{label} parent component is not a directory: {current}")
    return current


def _campaign_internal_regular_file(path: Path, label: str) -> Path:
    """Verify an internally constructed repository path without resolving it."""
    try:
        relative = path.relative_to(REPOSITORY_ROOT).as_posix()
    except ValueError as exc:
        raise ValueError(f"{label} must stay inside the repository") from exc
    verified = _campaign_regular_repository_file(relative, label)
    if verified != path:
        raise ValueError(f"{label} is not lexically canonical")
    return verified


def _strict_json_object(path: Path, label: str) -> tuple[dict, bytes]:
    """Read one duplicate-free finite JSON object for campaign authority."""
    def object_from_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key {key!r}")
            result[key] = value
        return result

    def reject_constant(value):
        raise ValueError(f"non-finite JSON token {value!r}")

    _campaign_internal_regular_file(path, label)
    raw = path.read_bytes()
    value = json.loads(
        raw.decode("utf-8"),
        object_pairs_hook=object_from_pairs,
        parse_constant=reject_constant,
    )
    if not isinstance(value, dict):
        raise ValueError(f"{label} root must be an object")
    return value, raw


def _q1024_singleton_partition_authority(
        *,
        config_path: str,
        profile: Mapping[str, object],
        campaign_contract: Mapping[str, object],
        campaign_contract_raw: bytes,
) -> tuple[dict | None, str | None]:
    """Validate one exact member of the pinned q1024 execution partition."""
    pattern = profile.get("singleton_pattern")
    if not isinstance(pattern, str) or re.fullmatch(pattern, config_path) is None:
        return None, "q1024 singleton path does not match the reserved syntax"
    try:
        manifest_relative = str(profile["singleton_partition_manifest"])
        manifest_path = _campaign_regular_repository_file(
            manifest_relative, "q1024 singleton partition manifest")
        manifest, manifest_raw = _strict_json_object(
            manifest_path, "q1024 singleton partition manifest")
        expected_manifest_sha = profile.get(
            "singleton_partition_manifest_sha256")
        if (not isinstance(expected_manifest_sha, str)
                or hashlib.sha256(manifest_raw).hexdigest()
                    != expected_manifest_sha):
            return None, "q1024 singleton partition SHA-256 is not pinned current"
        if set(manifest) != {
                "schema", "version", "campaign_binding",
                "partition_contract", "parents", "children"}:
            return None, "q1024 singleton partition has the wrong exact shape"
        binding = manifest.get("campaign_binding")
        partition = manifest.get("partition_contract")
        parents = manifest.get("parents")
        children = manifest.get("children")
        ownership = campaign_contract.get("shard_ownership")
        entries = campaign_contract.get("configs")
        if (manifest.get("schema")
                != "bioseq.e2e.algorithm-matrix-q1024-singleton-partition"
                or manifest.get("version") != 1
                or not isinstance(binding, dict)
                or binding != {
                    "campaign_id": profile["campaign_id"],
                    "campaign_digest": profile["campaign_digest"],
                    "canonical_manifest": profile["manifest"],
                    "canonical_manifest_sha256": hashlib.sha256(
                        campaign_contract_raw).hexdigest(),
                    "canonical_parent_config_count": 88,
                    "canonical_result_cells": 1296,
                    "canonical_raw_records": 6480,
                    "canonical_stable_keys_sha256": (
                        ownership.get("stable_keys_sha256")
                        if isinstance(ownership, dict) else None),
                }
                or not isinstance(partition, dict)
                or not isinstance(parents, list) or len(parents) != 88
                or not isinstance(children, list) or len(children) != 1296
                or not isinstance(entries, list)):
            return None, "q1024 singleton campaign binding/cardinality is invalid"
        claimed_partition_digest = partition.get("partition_digest")
        digest_payload = copy.deepcopy(manifest)
        digest_payload["partition_contract"].pop("partition_digest", None)
        if (not isinstance(claimed_partition_digest, str)
                or not re.fullmatch(r"[0-9a-f]{64}", claimed_partition_digest)
                or canonical_json_sha256(digest_payload)
                    != claimed_partition_digest
                or partition.get("child_config_count") != 1296
                or partition.get("cells_per_child") != 1
                or partition.get("records_per_child") != 5
                or partition.get("repetitions_per_cell") != 5
                or partition.get("repeat_indices") != list(range(5))
                or partition.get("normalization_axes")
                    != ["test_case_index", "tuple_index"]
                or partition.get("canonical_ownership_preserved") is not True
                or partition.get("parent_unions_exact") is not True
                or partition.get("global_union_exact") is not True
                or partition.get("stable_keys_sha256")
                    != binding.get("canonical_stable_keys_sha256")
                or partition.get("metric_count_distribution")
                    != {"15": 512, "21": 16, "29": 768}
                or partition.get("cell_disposition_distribution") != {
                    "timed_bioseq": 768,
                    "minimap2_baseline": 16,
                    "rejected_setup": 480,
                    "rejected_sql": 32,
                }
                or partition.get("prefilter_enabled") is not False
                or partition.get("pipeline_mode") != "task_parallel"
                or partition.get("cold_no_index_persistence") != "off"
                or partition.get("cigar_projection") is not True
                or partition.get("mapq_projection") is not True
                or partition.get("base_alignment_algorithms")
                    != ["ksw2", "block_aligner"]
                or partition.get("excluded_base_alignment_algorithms")
                    != ["ksw2_batched"]
                or partition.get("q1_or_q8_membership_permitted") is not False
                or partition.get("reserved_filename_regex") != pattern
                or partition.get("deterministic_first_launch") != (
                    "bench/e2e_algorithm_matrix_shards/"
                    "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
                    "0081_423ec685980909f5.json")
                or partition.get("partition_digest_algorithm") != (
                    "sha256-canonical-json-without-partition_digest")):
            return None, "q1024 singleton partition contract is invalid"

        canonical_parents = {
            entry.get("path"): entry for entry in entries
            if isinstance(entry, dict) and entry.get("role") == "shard"
        }
        parent_ordinals: dict[str, set[int]] = {}
        if len(canonical_parents) != 88:
            return None, "q1024 canonical parent membership changed"
        for parent_ordinal, proof in enumerate(parents):
            if (not isinstance(proof, dict)
                    or set(proof) != {
                        "ordinal", "path", "config_sha256",
                        "expected_result_cells", "expected_raw_records",
                        "expected_stable_keys_sha256", "child_ordinals"}
                    or proof.get("ordinal") != parent_ordinal):
                return None, "q1024 singleton parent proof is malformed"
            parent = canonical_parents.get(proof.get("path"))
            child_ordinals = proof.get("child_ordinals")
            if (parent is None or not isinstance(child_ordinals, list)
                    or len(child_ordinals)
                        != parent.get("expected_result_cells")
                    or any(isinstance(value, bool) or not isinstance(value, int)
                           for value in child_ordinals)
                    or proof.get("config_sha256")
                        != parent.get("config_sha256")
                    or proof.get("expected_result_cells")
                        != parent.get("expected_result_cells")
                    or proof.get("expected_raw_records")
                        != parent.get("expected_raw_records")
                    or proof.get("expected_stable_keys_sha256")
                        != parent.get("expected_stable_keys_sha256")):
                return None, "q1024 singleton parent binding is invalid"
            parent_ordinals[str(proof["path"])] = set(child_ordinals)

        exact_child_keys = {
            "ordinal", "path", "config_sha256", "parent_path",
            "parent_config_sha256", "parent_expected_stable_keys_sha256",
            "parent_test_case_index", "parent_tuple_index", "stable_key",
            "stable_key_sha256", "expected_stable_keys_sha256",
            "expected_result_cells", "expected_raw_records",
            "repetitions_per_cell", "declared_metric_names",
            "expected_all_records_disposition", "canonical_cell",
            "execution_cell",
        }
        selected = None
        all_keys: list[str] = []
        parent_keys: dict[str, list[str]] = {
            path: [] for path in canonical_parents}
        metric_counts: dict[int, int] = {}
        dispositions = {
            "timed_bioseq": 0, "minimap2_baseline": 0,
            "rejected_setup": 0, "rejected_sql": 0,
        }
        for ordinal, child in enumerate(children):
            if (not isinstance(child, dict) or set(child) != exact_child_keys
                    or child.get("ordinal") != ordinal):
                return None, "q1024 singleton child proof is malformed"
            path = child.get("path")
            parent_path = child.get("parent_path")
            stable_key = child.get("stable_key")
            canonical_cell = child.get("canonical_cell")
            execution_cell = child.get("execution_cell")
            parent = canonical_parents.get(parent_path)
            if (not isinstance(path, str) or re.fullmatch(pattern, path) is None
                    or not isinstance(stable_key, str)
                    or not isinstance(canonical_cell, dict)
                    or not isinstance(execution_cell, dict)
                    or parent is None
                    or ordinal not in parent_ordinals[str(parent_path)]):
                return None, "q1024 singleton child identity is invalid"
            stable_sha = hashlib.sha256(stable_key.encode("utf-8")).hexdigest()
            expected_path = (
                "bench/e2e_algorithm_matrix_shards/"
                "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
                f"{ordinal:04d}_{stable_sha[:16]}.json")
            normalized = copy.deepcopy(canonical_cell)
            normalized["test_case_index"] = 0
            normalized["tuple_index"] = 0
            parent_cells = parent.get("cells")
            parent_matches = [
                cell for cell in parent_cells
                if isinstance(parent_cells, list) and isinstance(cell, dict)
                and cell.get("test_case_index")
                    == child.get("parent_test_case_index")
                and cell.get("tuple_index") == child.get("parent_tuple_index")
            ] if isinstance(parent_cells, list) else []
            identity = canonical_cell.get("identity")
            metrics = child.get("declared_metric_names")
            disposition = child.get("expected_all_records_disposition")
            if (path != expected_path
                    or child.get("stable_key_sha256") != stable_sha
                    or child.get("expected_stable_keys_sha256") != stable_sha
                    or child.get("expected_result_cells") != 1
                    or child.get("expected_raw_records") != 5
                    or child.get("repetitions_per_cell") != 5
                    or len(parent_matches) != 1
                    or canonical_cell != parent_matches[0]
                    or execution_cell != normalized
                    or canonical_cell.get("stable_key") != stable_key
                    or not isinstance(identity, dict)
                    or tuple(identity) != tuple(_ALGORITHM_MATRIX_STABLE_KEY_FIELDS)
                    or identity.get("campaign_id") != profile["campaign_id"]
                    or child.get("parent_config_sha256")
                        != parent.get("config_sha256")
                    or child.get("parent_expected_stable_keys_sha256")
                        != parent.get("expected_stable_keys_sha256")
                    or not isinstance(metrics, list)
                    or len(metrics) not in {15, 21, 29}
                    or disposition not in {None, "rejected_setup", "rejected_sql"}):
                return None, "q1024 singleton child ownership proof is invalid"
            expected_metric_count = (
                15 if disposition is not None else
                21 if identity.get("engine") == "minimap2" else 29)
            if len(metrics) != expected_metric_count:
                return None, "q1024 singleton metric/disposition proof is invalid"
            bucket = disposition or (
                "minimap2_baseline"
                if identity.get("engine") == "minimap2" else "timed_bioseq")
            dispositions[bucket] += 1
            metric_counts[len(metrics)] = metric_counts.get(len(metrics), 0) + 1
            all_keys.append(stable_key)
            parent_keys[str(parent_path)].append(stable_key)
            if path == config_path:
                if selected is not None:
                    return None, "q1024 singleton path is duplicated"
                selected = child
        if (len(set(all_keys)) != 1296
                or hashlib.sha256(
                    "\n".join(sorted(all_keys)).encode("utf-8")).hexdigest()
                    != partition.get("stable_keys_sha256")
                or metric_counts != {15: 512, 21: 16, 29: 768}
                or dispositions != {
                    "timed_bioseq": 768, "minimap2_baseline": 16,
                    "rejected_setup": 480, "rejected_sql": 32,
                }):
            return None, "q1024 singleton global union is invalid"
        for parent_path, keys in parent_keys.items():
            parent = canonical_parents[parent_path]
            if hashlib.sha256(
                    "\n".join(sorted(keys)).encode("utf-8")).hexdigest() \
                    != parent.get("expected_stable_keys_sha256"):
                return None, "q1024 singleton parent union is invalid"
        if selected is None:
            return None, "q1024 singleton config is not manifest-owned"
        selected_path = _campaign_regular_repository_file(
            config_path, "q1024 singleton config")
        if sha256_file(selected_path) != selected.get("config_sha256"):
            return None, "q1024 singleton config bytes differ from the manifest"
    except (OSError, UnicodeError, ValueError, KeyError, TypeError) as exc:
        return None, f"cannot validate q1024 singleton partition: {exc}"
    return {
        "execution_role": "q1024_singleton",
        "partition_manifest": manifest_relative,
        "partition_manifest_sha256": expected_manifest_sha,
        "singleton": selected,
    }, None


def _q1024_packed_partition_authority(
        *,
        config_path: str,
        profile: Mapping[str, object],
        campaign_contract: Mapping[str, object],
        campaign_contract_raw: bytes,
        singleton_stable_key: str | None = None,
) -> tuple[dict | None, str | None]:
    """Validate packed membership and reject superseded singleton alternatives."""
    try:
        manifest_relative = str(profile["packed_partition_manifest"])
        manifest_path = _campaign_regular_repository_file(
            manifest_relative, "q1024 packed partition manifest")
        manifest, manifest_raw = _strict_json_object(
            manifest_path, "q1024 packed partition manifest")
        expected_manifest_sha = profile.get("packed_partition_manifest_sha256")
        if (not isinstance(expected_manifest_sha, str)
                or hashlib.sha256(manifest_raw).hexdigest()
                    != expected_manifest_sha):
            return None, "q1024 packed partition SHA-256 is not pinned current"
        if set(manifest) != {
                "schema", "version", "campaign_binding", "execution_contract",
                "packed_members", "adoptions", "retained_singletons",
                "optional_singletons", "execution_plan"}:
            return None, "q1024 packed partition has the wrong exact shape"
        binding = manifest.get("campaign_binding")
        contract = manifest.get("execution_contract")
        members = manifest.get("packed_members")
        adoptions = manifest.get("adoptions")
        ownership = campaign_contract.get("shard_ownership")
        entries = campaign_contract.get("configs")
        singleton_manifest_relative = str(profile["singleton_partition_manifest"])
        if (manifest.get("schema")
                != "bioseq.e2e.algorithm-matrix-q1024-packed-partition"
                or manifest.get("version") != 1
                or not isinstance(binding, dict)
                or binding != {
                    "campaign_id": profile["campaign_id"],
                    "campaign_digest": profile["campaign_digest"],
                    "canonical_manifest": profile["manifest"],
                    "canonical_manifest_sha256": hashlib.sha256(
                        campaign_contract_raw).hexdigest(),
                    "singleton_manifest": singleton_manifest_relative,
                    "singleton_manifest_sha256": profile[
                        "singleton_partition_manifest_sha256"],
                    "deadline_v2_manifest": (
                        "bench/bench_e2e_algorithm_matrix_deadline_v2_manifest.json"),
                    "deadline_v2_manifest_sha256": (
                        "bf2abc14dbda84079d6384e330b1fdc752e1754360cc61d9fec42af1e2d1b31c"),
                    "deadline_v2_campaign_id": (
                        "bioseq-e2e-algorithm-matrix-deadline-v2"),
                    "deadline_v2_campaign_digest": (
                        "44a85360a5e975ec8a6d70ac0b06486c51d748770c2ada46106d2ccc5db4728a"),
                }
                or not isinstance(contract, dict)
                or not isinstance(members, list) or len(members) != 25
                or not isinstance(adoptions, list) or len(adoptions) != 4
                or not isinstance(ownership, dict)
                or not isinstance(entries, list)):
            return None, "q1024 packed campaign binding/cardinality is invalid"
        digest = contract.get("partition_digest")
        digest_payload = copy.deepcopy(manifest)
        digest_payload["execution_contract"].pop("partition_digest", None)
        if (not isinstance(digest, str)
                or re.fullmatch(r"[0-9a-f]{64}", digest) is None
                or canonical_json_sha256(digest_payload) != digest):
            return None, "q1024 packed partition digest is invalid"

        canonical_cells = {}
        for entry in entries:
            if not isinstance(entry, dict) or entry.get("role") != "monolithic":
                continue
            cells = entry.get("cells")
            if not isinstance(cells, list):
                return None, "q1024 canonical monolith cells are malformed"
            canonical_cells = {
                cell.get("stable_key"): cell for cell in cells
                if isinstance(cell, dict) and isinstance(cell.get("stable_key"), str)}
        if len(canonical_cells) != 1296:
            return None, "q1024 canonical stable-key union changed"

        exact_member_keys = {
            "ordinal", "id", "kind", "path", "config_sha256",
            "source_parent_path", "source_parent_config_sha256",
            "expected_result_cells", "expected_raw_records",
            "repetitions_per_cell", "stable_keys",
            "expected_stable_keys_sha256", "allowed_nonblocking_rule_names",
            "declared_metric_names", "expected_all_records_disposition",
            "planned_supported_cells", "planned_selector_rejection_cells",
            "canonical_cells", "execution_cells",
        }
        selected = None
        packed_keys: list[str] = []
        packed_paths: set[str] = set()
        packed_ids: set[str] = set()
        packed_cells = 0
        for ordinal, member in enumerate(members):
            if (not isinstance(member, dict) or set(member) != exact_member_keys
                    or member.get("ordinal") != ordinal
                    or not isinstance(member.get("id"), str)
                    or member["id"] in packed_ids
                    or member.get("kind") not in {
                        "canonical_parent", "generated_persisted_residual",
                        "generated_minimap2_pair"}
                    or not isinstance(member.get("path"), str)
                    or member["path"] in packed_paths
                    or not isinstance(member.get("config_sha256"), str)
                    or re.fullmatch(
                        r"[0-9a-f]{64}", member["config_sha256"]) is None
                    or not isinstance(member.get("stable_keys"), list)
                    or not isinstance(member.get("expected_result_cells"), int)
                    or member["expected_result_cells"] <= 0
                    or member.get("expected_raw_records")
                        != member["expected_result_cells"] * 5
                    or member.get("repetitions_per_cell") != 5
                    or len(member["stable_keys"])
                        != member["expected_result_cells"]
                    or len(set(member["stable_keys"])) != len(member["stable_keys"])
                    or any(key not in canonical_cells for key in member["stable_keys"])
                    or hashlib.sha256(
                        "\n".join(sorted(member["stable_keys"])).encode("utf-8")
                    ).hexdigest() != member.get("expected_stable_keys_sha256")):
                return None, "q1024 packed member proof is malformed"
            packed_ids.add(member["id"])
            packed_paths.add(member["path"])
            packed_keys.extend(member["stable_keys"])
            packed_cells += member["expected_result_cells"]
            if member["path"] == config_path:
                selected = member
        if (packed_cells != 150 or len(packed_keys) != 150
                or len(set(packed_keys)) != 150):
            return None, "q1024 packed member union changed"

        exact_adoption_keys = {
            "ordinal", "source_ordinal", "source_path",
            "source_config_sha256", "source_stable_key",
            "source_stable_key_sha256", "canonical_stable_key",
            "canonical_stable_key_sha256", "canonical_singleton_path",
            "canonical_singleton_config_sha256", "expected_result_cells",
            "expected_raw_records", "repetitions_per_cell",
            "declared_metric_names", "expected_all_records_disposition",
            "canonical_cell", "execution_cell", "run_receipt_config",
            "run_receipt_id", "sealed_artifacts",
            "ingestion_policy",
        }
        adopted_keys: list[str] = []
        for ordinal, adoption in enumerate(adoptions):
            if (not isinstance(adoption, dict)
                    or set(adoption) != exact_adoption_keys
                    or adoption.get("ordinal") != ordinal
                    or adoption.get("source_ordinal") != 56 + ordinal
                    or not isinstance(adoption.get("canonical_stable_key"), str)
                    or adoption["canonical_stable_key"] not in canonical_cells
                    or adoption.get("canonical_stable_key_sha256")
                        != hashlib.sha256(
                            adoption["canonical_stable_key"].encode("utf-8")
                        ).hexdigest()
                    or adoption.get("expected_result_cells") != 1
                    or adoption.get("expected_raw_records") != 5
                    or adoption.get("repetitions_per_cell") != 5):
                return None, "q1024 adoption proof is malformed"
            receipt_config = adoption.get("run_receipt_config")
            if (not isinstance(receipt_config, dict)
                    or set(receipt_config) != {"path", "sha256"}
                    or not isinstance(receipt_config.get("path"), str)
                    or receipt_config.get("sha256")
                        != adoption.get("source_config_sha256")):
                return None, "q1024 adoption receipt-config proof is malformed"
            receipt_id = adoption.get("run_receipt_id")
            if ((ordinal < 3 and (not isinstance(receipt_id, str)
                    or re.fullmatch(r"rr_[0-9a-f]{32}", receipt_id) is None))
                    or (ordinal == 3 and receipt_id is not None)):
                return None, "q1024 adoption run-receipt proof is malformed"
            adopted_keys.append(adoption["canonical_stable_key"])
        replacement_keys = set(packed_keys) | set(adopted_keys)
        fallback_keys = set(canonical_cells) - replacement_keys
        if (len(set(adopted_keys)) != 4
                or set(adopted_keys) & set(packed_keys)
                or len(replacement_keys) != 154
                or len(fallback_keys) != 1142
                or contract.get("authority_mode") != "hybrid_exact_disjoint"
                or contract.get("packed_config_count") != 25
                or contract.get("packed_result_cells") != 150
                or contract.get("adopted_config_count") != 4
                or contract.get("adopted_result_cells") != 4
                or contract.get("superseded_singleton_cells") != 154
                or contract.get("singleton_fallback_cells") != 1142
                or contract.get("hybrid_execution_config_count") != 1171
                or contract.get("canonical_result_cells") != 1296
                or contract.get("canonical_raw_records") != 6480
                or contract.get("repetitions_per_cell") != 5
                or contract.get("exact_nonoverlapping") is not True
                or contract.get("canonical_union_exact") is not True
                or contract.get("duplicate_stable_key_repeat_permitted") is not False
                or contract.get("canonical_stable_keys_sha256")
                    != ownership.get("stable_keys_sha256")
                or contract.get("packed_stable_keys_sha256")
                    != hashlib.sha256(
                        "\n".join(sorted(packed_keys)).encode("utf-8")
                    ).hexdigest()
                or contract.get("adopted_stable_keys_sha256")
                    != hashlib.sha256(
                        "\n".join(sorted(adopted_keys)).encode("utf-8")
                    ).hexdigest()
                or contract.get("superseded_stable_keys_sha256")
                    != hashlib.sha256(
                        "\n".join(sorted(replacement_keys)).encode("utf-8")
                    ).hexdigest()
                or contract.get("singleton_fallback_stable_keys_sha256")
                    != hashlib.sha256(
                        "\n".join(sorted(fallback_keys)).encode("utf-8")
                    ).hexdigest()
                or contract.get("prefilter_enabled") is not False
                or contract.get("cigar_projection") is not True
                or contract.get("mapq_projection") is not True
                or contract.get("excluded_base_alignment_algorithms")
                    != ["ksw2_batched"]
                or contract.get("deterministic_first_ingestion") != (
                    "bench/e2e_algorithm_matrix_deadline_v2_shards/"
                    "bench_e2e_algorithm_matrix_deadline_v2_s056_q1024_"
                    "q1024_anchor_m2_d4_k15_w10_no_index_minimap2.json")):
            return None, "q1024 packed hybrid ownership contract is invalid"

        if singleton_stable_key is not None:
            if singleton_stable_key in replacement_keys:
                return None, (
                    "q1024 singleton is superseded by exact packed/adopted ownership")
            return None, None
        packed_pattern = profile.get("packed_pattern")
        if selected is None:
            if (isinstance(packed_pattern, str)
                    and re.fullmatch(packed_pattern, config_path) is not None):
                return None, "q1024 packed path is not manifest-owned"
            return None, None
        selected_path = _campaign_regular_repository_file(
            config_path, "q1024 packed config")
        if sha256_file(selected_path) != selected.get("config_sha256"):
            return None, "q1024 packed config bytes differ from the manifest"
    except (OSError, UnicodeError, ValueError, KeyError, TypeError) as exc:
        return None, f"cannot validate q1024 packed partition: {exc}"
    return {
        "execution_role": "q1024_packed",
        "partition_manifest": manifest_relative,
        "partition_manifest_sha256": expected_manifest_sha,
        "packed_member": copy.deepcopy(selected),
    }, None


def _walk_nested_items(value: object):
    """Yield nested JSON object fields for closed configuration checks."""
    if isinstance(value, dict):
        for key, child in value.items():
            yield key, child
            yield from _walk_nested_items(child)
    elif isinstance(value, list):
        for child in value:
            yield from _walk_nested_items(child)


def _q1024_singleton0058_extended_timeout_authority(
        config_path: str,
        campaign_authority: Mapping[str, object],
) -> tuple[dict | None, str | None]:
    """Validate the additive benchmark-only timeout for canonical child 58."""
    if config_path != _ALGORITHM_MATRIX_Q1024_SINGLETON0058_CONFIG:
        return None, None
    try:
        manifest_path = _campaign_regular_repository_file(
            _ALGORITHM_MATRIX_Q1024_SINGLETON0058_EXTENDED_TIMEOUT_MANIFEST,
            "q1024 singleton0058 extended-timeout authority")
        manifest, manifest_raw = _strict_json_object(
            manifest_path,
            "q1024 singleton0058 extended-timeout authority")
        manifest_sha = hashlib.sha256(manifest_raw).hexdigest()
        if manifest_sha != (
                _ALGORITHM_MATRIX_Q1024_SINGLETON0058_EXTENDED_TIMEOUT_MANIFEST_SHA256):
            return None, (
                "q1024 singleton0058 extended-timeout authority SHA-256 is "
                "not pinned current")
        if set(manifest) != {
                "schema", "version", "authority", "campaign_binding",
                "runtime_contract", "selected_config", "preservation"}:
            return None, (
                "q1024 singleton0058 extended-timeout authority has the wrong "
                "exact shape")
        authority = manifest.get("authority")
        campaign_binding = manifest.get("campaign_binding")
        runtime = manifest.get("runtime_contract")
        selected = manifest.get("selected_config")
        preservation = manifest.get("preservation")
        if (manifest.get("schema")
                != "bioseq.e2e.algorithm-matrix-q1024-extended-timeout-authority"
                or manifest.get("version") != 1
                or authority != {
                    "authority_id": (
                        "bioseq-e2e-q1024-singleton0058-"
                        "extended-timeout-v1"),
                    "authority_digest_algorithm": (
                        "sha256-canonical-json-without-authority_digest"),
                    "authority_digest": (
                        _ALGORITHM_MATRIX_Q1024_SINGLETON0058_EXTENDED_TIMEOUT_AUTHORITY_DIGEST),
                }
                or campaign_binding != {
                    "campaign_id": _ALGORITHM_MATRIX_CAMPAIGN_ID,
                    "campaign_digest": _ALGORITHM_MATRIX_CAMPAIGN_DIGEST,
                    "singleton_partition_manifest": (
                        "bench/bench_e2e_algorithm_matrix_q1024_"
                        "singleton_partition_manifest.json"),
                    "singleton_partition_manifest_sha256": (
                        _ALGORITHM_MATRIX_CAMPAIGN_PROFILES[0][
                            "singleton_partition_manifest_sha256"]),
                    "child_ordinal": 58,
                }
                or runtime != {
                    "mode": "benchmark-e2e",
                    "run_only_required": True,
                    "exact_benchmark_filter": (
                        _ALGORITHM_MATRIX_EXACT_BENCH_FILTER),
                    "normal_timeout_seconds": (
                        _ALGORITHM_MATRIX_DEADLINE_NORMAL_TIMEOUT_SECONDS),
                    "extended_timeout_seconds": (
                        _ALGORITHM_MATRIX_DEADLINE_EXTENDED_TIMEOUT_SECONDS),
                    "selected_config_count": 1,
                    "benchmark_only": True,
                }
                or selected != {
                    "path": _ALGORITHM_MATRIX_Q1024_SINGLETON0058_CONFIG,
                    "config_sha256": (
                        _ALGORITHM_MATRIX_Q1024_SINGLETON0058_CONFIG_SHA256),
                    "stable_key": (
                        _ALGORITHM_MATRIX_Q1024_SINGLETON0058_STABLE_KEY),
                    "stable_key_sha256": (
                        _ALGORITHM_MATRIX_Q1024_SINGLETON0058_STABLE_KEY_SHA256),
                    "query_count": 1024,
                    "repetitions": 5,
                    "expected_records": 5,
                    "cigar_projection": True,
                    "mapq_projection": True,
                    "prefilter_enabled": False,
                    "ksw2_batched_excluded": True,
                }
                or preservation != {
                    "existing_deadline_v1_manifest_unchanged": True,
                    "existing_deadline_v2_manifest_unchanged": True,
                    "existing_results_and_artifact_seals_unchanged": True,
                    "nonselected_configs_keep_normal_timeout": True,
                }):
            return None, (
                "q1024 singleton0058 extended-timeout authority contract is "
                "invalid")
        digest_payload = copy.deepcopy(manifest)
        digest_payload["authority"].pop("authority_digest", None)
        if canonical_json_sha256(digest_payload) != (
                _ALGORITHM_MATRIX_Q1024_SINGLETON0058_EXTENDED_TIMEOUT_AUTHORITY_DIGEST):
            return None, (
                "q1024 singleton0058 extended-timeout canonical digest is "
                "invalid")
        config = _campaign_regular_repository_file(
            config_path, "q1024 singleton0058 extended-timeout config")
        if sha256_file(config) != (
                _ALGORITHM_MATRIX_Q1024_SINGLETON0058_CONFIG_SHA256):
            return None, (
                "q1024 singleton0058 config bytes differ from the timeout "
                "authority seal")
        if hashlib.sha256(
                _ALGORITHM_MATRIX_Q1024_SINGLETON0058_STABLE_KEY.encode(
                    "utf-8")).hexdigest() != (
                        _ALGORITHM_MATRIX_Q1024_SINGLETON0058_STABLE_KEY_SHA256):
            return None, (
                "q1024 singleton0058 timeout stable-key seal is invalid")
        singleton = campaign_authority.get("singleton")
        if (campaign_authority.get("name") != "q1024"
                or campaign_authority.get("execution_role")
                    != "q1024_singleton"
                or campaign_authority.get("singleton_partition_manifest_sha256")
                    != campaign_binding["singleton_partition_manifest_sha256"]
                or not isinstance(singleton, dict)
                or singleton.get("ordinal") != 58
                or singleton.get("path") != config_path
                or singleton.get("config_sha256")
                    != selected["config_sha256"]
                or singleton.get("stable_key") != selected["stable_key"]
                or singleton.get("stable_key_sha256")
                    != selected["stable_key_sha256"]
                or singleton.get("repetitions_per_cell") != 5
                or singleton.get("expected_raw_records") != 5
                or singleton.get("expected_all_records_disposition") is not None):
            return None, (
                "q1024 singleton0058 timeout authority is not bound to the "
                "canonical supported-timing child")
    except (OSError, UnicodeError, ValueError, KeyError, TypeError) as exc:
        return None, (
            "cannot validate q1024 singleton0058 extended-timeout authority: "
            f"{exc}")
    return {
        "name": "q1024_singleton0058_extended_timeout",
        "manifest": (
            _ALGORITHM_MATRIX_Q1024_SINGLETON0058_EXTENDED_TIMEOUT_MANIFEST),
        "manifest_sha256": manifest_sha,
        "authority_digest": (
            _ALGORITHM_MATRIX_Q1024_SINGLETON0058_EXTENDED_TIMEOUT_AUTHORITY_DIGEST),
        "config_sha256": (
            _ALGORITHM_MATRIX_Q1024_SINGLETON0058_CONFIG_SHA256),
        "stable_key_sha256": (
            _ALGORITHM_MATRIX_Q1024_SINGLETON0058_STABLE_KEY_SHA256),
        "runtime_timeout_seconds": (
            _ALGORITHM_MATRIX_DEADLINE_EXTENDED_TIMEOUT_SECONDS),
    }, None


def _q1024_singleton0068_extended_timeout_authority(
        config_path: str,
        campaign_authority: Mapping[str, object],
) -> tuple[dict | None, str | None]:
    """Validate the deadline-capped benchmark timeout for child 68."""
    if config_path != _ALGORITHM_MATRIX_Q1024_SINGLETON0068_CONFIG:
        return None, None
    try:
        manifest_path = _campaign_regular_repository_file(
            _ALGORITHM_MATRIX_Q1024_SINGLETON0068_EXTENDED_TIMEOUT_MANIFEST,
            "q1024 singleton0068 extended-timeout authority")
        manifest, manifest_raw = _strict_json_object(
            manifest_path,
            "q1024 singleton0068 extended-timeout authority")
        manifest_sha = hashlib.sha256(manifest_raw).hexdigest()
        if manifest_sha != (
                _ALGORITHM_MATRIX_Q1024_SINGLETON0068_EXTENDED_TIMEOUT_MANIFEST_SHA256):
            return None, (
                "q1024 singleton0068 extended-timeout authority SHA-256 is "
                "not pinned current")
        if set(manifest) != {
                "schema", "version", "authority", "campaign_binding",
                "runtime_contract", "selected_config", "preservation"}:
            return None, (
                "q1024 singleton0068 extended-timeout authority has the wrong "
                "exact shape")
        authority = manifest.get("authority")
        campaign_binding = manifest.get("campaign_binding")
        runtime = manifest.get("runtime_contract")
        selected = manifest.get("selected_config")
        preservation = manifest.get("preservation")
        if (manifest.get("schema")
                != "bioseq.e2e.algorithm-matrix-q1024-extended-timeout-authority"
                or manifest.get("version") != 1
                or authority != {
                    "authority_id": (
                        "bioseq-e2e-q1024-singleton0068-"
                        "extended-timeout-v1"),
                    "authority_digest_algorithm": (
                        "sha256-canonical-json-without-authority_digest"),
                    "authority_digest": (
                        _ALGORITHM_MATRIX_Q1024_SINGLETON0068_EXTENDED_TIMEOUT_AUTHORITY_DIGEST),
                }
                or campaign_binding != {
                    "campaign_id": _ALGORITHM_MATRIX_CAMPAIGN_ID,
                    "campaign_digest": _ALGORITHM_MATRIX_CAMPAIGN_DIGEST,
                    "singleton_partition_manifest": (
                        "bench/bench_e2e_algorithm_matrix_q1024_"
                        "singleton_partition_manifest.json"),
                    "singleton_partition_manifest_sha256": (
                        _ALGORITHM_MATRIX_CAMPAIGN_PROFILES[0][
                            "singleton_partition_manifest_sha256"]),
                    "child_ordinal": 68,
                }
                or runtime != {
                    "mode": "benchmark-e2e",
                    "run_only_required": True,
                    "exact_benchmark_filter": (
                        _ALGORITHM_MATRIX_EXACT_BENCH_FILTER),
                    "normal_timeout_seconds": (
                        _ALGORITHM_MATRIX_DEADLINE_NORMAL_TIMEOUT_SECONDS),
                    "extended_timeout_seconds": (
                        _ALGORITHM_MATRIX_DEADLINE_EXTENDED_TIMEOUT_SECONDS),
                    "hard_stop_at": (
                        _ALGORITHM_MATRIX_Q1024_SINGLETON0068_HARD_STOP_AT),
                    "hard_stop_epoch": (
                        _ALGORITHM_MATRIX_Q1024_SINGLETON0068_HARD_STOP_EPOCH),
                    "selected_config_count": 1,
                    "benchmark_only": True,
                }
                or selected != {
                    "path": _ALGORITHM_MATRIX_Q1024_SINGLETON0068_CONFIG,
                    "config_sha256": (
                        _ALGORITHM_MATRIX_Q1024_SINGLETON0068_CONFIG_SHA256),
                    "stable_key": (
                        _ALGORITHM_MATRIX_Q1024_SINGLETON0068_STABLE_KEY),
                    "stable_key_sha256": (
                        _ALGORITHM_MATRIX_Q1024_SINGLETON0068_STABLE_KEY_SHA256),
                    "query_count": 1024,
                    "repetitions": 5,
                    "expected_records": 5,
                    "cigar_projection": True,
                    "mapq_projection": True,
                    "prefilter_enabled": False,
                    "ksw2_batched_excluded": True,
                }
                or preservation != {
                    "existing_deadline_v1_manifest_unchanged": True,
                    "existing_deadline_v2_manifest_unchanged": True,
                    "existing_results_and_artifact_seals_unchanged": True,
                    "nonselected_configs_keep_normal_timeout": True,
                }):
            return None, (
                "q1024 singleton0068 extended-timeout authority contract is "
                "invalid")
        digest_payload = copy.deepcopy(manifest)
        digest_payload["authority"].pop("authority_digest", None)
        if canonical_json_sha256(digest_payload) != (
                _ALGORITHM_MATRIX_Q1024_SINGLETON0068_EXTENDED_TIMEOUT_AUTHORITY_DIGEST):
            return None, (
                "q1024 singleton0068 extended-timeout canonical digest is "
                "invalid")
        config = _campaign_regular_repository_file(
            config_path, "q1024 singleton0068 extended-timeout config")
        if sha256_file(config) != (
                _ALGORITHM_MATRIX_Q1024_SINGLETON0068_CONFIG_SHA256):
            return None, (
                "q1024 singleton0068 config bytes differ from the timeout "
                "authority seal")
        if hashlib.sha256(
                _ALGORITHM_MATRIX_Q1024_SINGLETON0068_STABLE_KEY.encode(
                    "utf-8")).hexdigest() != (
                        _ALGORITHM_MATRIX_Q1024_SINGLETON0068_STABLE_KEY_SHA256):
            return None, (
                "q1024 singleton0068 timeout stable-key seal is invalid")
        singleton = campaign_authority.get("singleton")
        if (campaign_authority.get("name") != "q1024"
                or campaign_authority.get("execution_role")
                    != "q1024_singleton"
                or campaign_authority.get("singleton_partition_manifest_sha256")
                    != campaign_binding["singleton_partition_manifest_sha256"]
                or not isinstance(singleton, dict)
                or singleton.get("ordinal") != 68
                or singleton.get("path") != config_path
                or singleton.get("config_sha256")
                    != selected["config_sha256"]
                or singleton.get("stable_key") != selected["stable_key"]
                or singleton.get("stable_key_sha256")
                    != selected["stable_key_sha256"]
                or singleton.get("repetitions_per_cell") != 5
                or singleton.get("expected_raw_records") != 5
                or singleton.get("expected_all_records_disposition") is not None):
            return None, (
                "q1024 singleton0068 timeout authority is not bound to the "
                "canonical supported-timing child")
        if time.time() >= _ALGORITHM_MATRIX_Q1024_SINGLETON0068_HARD_STOP_EPOCH:
            return None, (
                "q1024 singleton0068 extended-timeout authority has expired")
    except (OSError, UnicodeError, ValueError, KeyError, TypeError) as exc:
        return None, (
            "cannot validate q1024 singleton0068 extended-timeout authority: "
            f"{exc}")
    return {
        "name": "q1024_singleton0068_extended_timeout",
        "manifest": (
            _ALGORITHM_MATRIX_Q1024_SINGLETON0068_EXTENDED_TIMEOUT_MANIFEST),
        "manifest_sha256": manifest_sha,
        "authority_digest": (
            _ALGORITHM_MATRIX_Q1024_SINGLETON0068_EXTENDED_TIMEOUT_AUTHORITY_DIGEST),
        "config_sha256": (
            _ALGORITHM_MATRIX_Q1024_SINGLETON0068_CONFIG_SHA256),
        "stable_key_sha256": (
            _ALGORITHM_MATRIX_Q1024_SINGLETON0068_STABLE_KEY_SHA256),
        "runtime_timeout_seconds": (
            _ALGORITHM_MATRIX_DEADLINE_EXTENDED_TIMEOUT_SECONDS),
        "hard_stop_at": _ALGORITHM_MATRIX_Q1024_SINGLETON0068_HARD_STOP_AT,
        "hard_stop_epoch": (
            _ALGORITHM_MATRIX_Q1024_SINGLETON0068_HARD_STOP_EPOCH),
    }, None


def _q1024_extended_timeout_authority(
        config_path: str,
        campaign_authority: Mapping[str, object],
) -> tuple[dict | None, str | None]:
    """Validate either independent q1024 extended-timeout authority."""
    for validator in (
            _q1024_singleton0058_extended_timeout_authority,
            _q1024_singleton0068_extended_timeout_authority):
        authority, error = validator(config_path, campaign_authority)
        if error is not None or authority is not None:
            return authority, error
    return None, None


def _deadline_campaign_authority(
        config_path: str,
        *,
        validate_current_source_bindings: bool = True,
) -> tuple[dict | None, str | None]:
    """Validate one exact member of the production deadline campaign."""
    directory_prefix = _ALGORITHM_MATRIX_DEADLINE_DIRECTORY + "/"
    textual_member = config_path.startswith(directory_prefix)
    deadline_directory = REPOSITORY_ROOT / _ALGORITHM_MATRIX_DEADLINE_DIRECTORY
    if not textual_member:
        try:
            candidate = Path(config_path).expanduser()
            if not candidate.is_absolute():
                candidate = REPOSITORY_ROOT / candidate
            if candidate.resolve(strict=True).parent == deadline_directory.resolve(
                    strict=True):
                return None, (
                    "deadline campaign config must use one exact manifest-owned "
                    "repository-relative path; aliases are forbidden")
        except (OSError, RuntimeError, ValueError):
            pass
        return None, None

    try:
        manifest_path = _campaign_regular_repository_file(
            _ALGORITHM_MATRIX_DEADLINE_MANIFEST,
            "deadline campaign manifest")
        manifest, manifest_raw = _strict_json_object(
            manifest_path, "deadline campaign manifest")
        manifest_sha = hashlib.sha256(manifest_raw).hexdigest()
        if manifest_sha != _ALGORITHM_MATRIX_DEADLINE_MANIFEST_SHA256:
            return None, "deadline campaign manifest SHA-256 is not pinned current"
        if set(manifest) != {
                "schema", "version", "campaign", "source_bindings",
                "calibration_anchors", "runtime_contract", "coverage",
                "deadline_exclusions", "publication_contract",
                "deterministic_first_shard", "schedule"}:
            return None, "deadline campaign manifest has the wrong exact shape"
        campaign = manifest.get("campaign")
        runtime = manifest.get("runtime_contract")
        publication = manifest.get("publication_contract")
        source_bindings = manifest.get("source_bindings")
        schedule = manifest.get("schedule")
        first = manifest.get("deterministic_first_shard")
        if (manifest.get("schema")
                != "bioseq.e2e.algorithm-matrix-deadline-campaign"
                or manifest.get("version") != 1
                or not isinstance(campaign, dict)
                or campaign.get("campaign_id")
                    != _ALGORITHM_MATRIX_DEADLINE_CAMPAIGN_ID
                or campaign.get("campaign_digest")
                    != _ALGORITHM_MATRIX_DEADLINE_CAMPAIGN_DIGEST
                or campaign.get("q1_membership") != 0
                or campaign.get("q1_is_estimate_reference_only") is not True
                or campaign.get("repetitions_per_cell") != 5
                or campaign.get("expected_shards") != 59
                or campaign.get("expected_result_cells") != 59
                or campaign.get("expected_raw_records") != 295
                or campaign.get("estimated_wall_seconds", 475_200) >= 475_200
                or campaign.get("deadline_seconds") != 475_200
                or campaign.get("operational_margin_seconds", 0) <= 0
                or not isinstance(runtime, dict)
                or runtime.get("mode") != "benchmark-e2e"
                or runtime.get("run_only_required") is not True
                or runtime.get("exact_benchmark_filter")
                    != _ALGORITHM_MATRIX_EXACT_BENCH_FILTER
                or runtime.get("prefilter_enabled") is not False
                or runtime.get("cigar_projection") is not True
                or runtime.get("mapq_projection") is not True
                or runtime.get("ksw2_batched_excluded") is not True
                or runtime.get("runtime_proof_counters_retained") is not True
                or runtime.get("normal_timeout_seconds")
                    != _ALGORITHM_MATRIX_DEADLINE_NORMAL_TIMEOUT_SECONDS
                or runtime.get("extended_timeout_seconds")
                    != _ALGORITHM_MATRIX_DEADLINE_EXTENDED_TIMEOUT_SECONDS
                or runtime.get("extended_timeout_shard_count") != 1
                or runtime.get("sequential_only") is not True
                or publication != {
                    "classification": "production_all_results_candidate_rows",
                    "q1_calibration": False,
                    "merge_into_ALL_RESULTS_yaml":
                        "eligible_after_terminal_per_shard_audit",
                    "automatic_merge_during_run_only": False,
                    "required_terminal_gate": (
                        "five records, accepted or harness-proven rejection, exact "
                        "parity, runtime proof counters, fallback comments, and "
                        "sealed run receipt"),
                    "merge_into_Q8_RESULTS_yaml": False,
                    "existing_ledgers_unchanged_by_generation": True,
                }
                or not isinstance(source_bindings, dict)
                or not isinstance(schedule, list) or len(schedule) != 59
                or not isinstance(first, dict)):
            return None, "deadline campaign runtime/publication contract is invalid"
        digest_payload = copy.deepcopy(manifest)
        digest_payload["campaign"].pop("campaign_digest", None)
        if canonical_json_sha256(digest_payload) \
                != _ALGORITHM_MATRIX_DEADLINE_CAMPAIGN_DIGEST:
            return None, "deadline campaign canonical digest is invalid"
        if validate_current_source_bindings:
            for relative, expected_sha in source_bindings.items():
                source_path = _campaign_regular_repository_file(
                    relative, "deadline source binding")
                if (not isinstance(expected_sha, str)
                        or re.fullmatch(r"[0-9a-f]{64}", expected_sha) is None
                        or sha256_file(source_path) != expected_sha):
                    return None, "deadline campaign source binding is stale"

        exact_entry_keys = {
            "ordinal", "path", "config_sha256", "stable_key",
            "stable_key_sha256", "identity", "repetitions",
            "expected_records", "allowed_statuses", "expected_disposition",
            "metric_names", "timed_estimate_seconds", "fixed_seconds",
            "contingency_multiplier", "estimated_wall_seconds",
            "runtime_timeout_seconds",
        }
        selected = None
        unique_paths: set[str] = set()
        unique_keys: set[str] = set()
        extended_entries = []
        estimated_total = 0
        for ordinal, entry in enumerate(schedule):
            if (not isinstance(entry, dict) or set(entry) != exact_entry_keys
                    or entry.get("ordinal") != ordinal
                    or entry.get("repetitions") != 5
                    or entry.get("expected_records") != 5
                    or not isinstance(entry.get("path"), str)
                    or not entry["path"].startswith(directory_prefix)
                    or not isinstance(entry.get("config_sha256"), str)
                    or re.fullmatch(
                        r"[0-9a-f]{64}", entry["config_sha256"]) is None
                    or not isinstance(entry.get("stable_key"), str)
                    or hashlib.sha256(
                        entry["stable_key"].encode("utf-8")).hexdigest()
                        != entry.get("stable_key_sha256")
                    or not isinstance(entry.get("identity"), dict)
                    or entry["identity"].get("campaign_id")
                        != _ALGORITHM_MATRIX_DEADLINE_CAMPAIGN_ID
                    or not isinstance(entry.get("metric_names"), list)
                    or entry.get("runtime_timeout_seconds") not in {
                        _ALGORITHM_MATRIX_DEADLINE_NORMAL_TIMEOUT_SECONDS,
                        _ALGORITHM_MATRIX_DEADLINE_EXTENDED_TIMEOUT_SECONDS}
                    or entry.get("estimated_wall_seconds", 0) <= 0
                    or entry["estimated_wall_seconds"]
                        >= entry["runtime_timeout_seconds"]
                    or entry["path"] in unique_paths
                    or entry["stable_key"] in unique_keys):
                return None, "deadline campaign schedule entry is malformed"
            unique_paths.add(entry["path"])
            unique_keys.add(entry["stable_key"])
            estimated_total += entry["estimated_wall_seconds"]
            candidate_path = _campaign_regular_repository_file(
                entry["path"], "deadline campaign config")
            if sha256_file(candidate_path) != entry["config_sha256"]:
                return None, "deadline campaign config bytes differ from the seal"
            if entry["runtime_timeout_seconds"] \
                    == _ALGORITHM_MATRIX_DEADLINE_EXTENDED_TIMEOUT_SECONDS:
                extended_entries.append(entry)
            if entry["path"] == config_path:
                selected = entry
        if (estimated_total != campaign.get("estimated_wall_seconds")
                or len(extended_entries) != 1
                or extended_entries[0].get("ordinal") != 58
                or extended_entries[0]["identity"] != {
                    "campaign_id": _ALGORITHM_MATRIX_DEADLINE_CAMPAIGN_ID,
                    "tier": "q1024_anchor",
                    "query_count": 1024,
                    "dataset": "M2_D4_K15_W10",
                    "index_mode": "just_index",
                    "engine": "bioseq",
                    "bioseq_index_name": "minimizers_per_fragment",
                    "requested_minimizer_algorithm": "scalar",
                    "requested_chaining_algorithm": "scalar_minimap2",
                    "requested_base_alignment_algorithm": "ksw2",
                }
                or first.get("path") != schedule[0]["path"]
                or first.get("config_sha256") != schedule[0]["config_sha256"]
                or selected is None):
            return None, "deadline campaign membership/timeout seal is invalid"

        config_path_object = _campaign_regular_repository_file(
            config_path, "selected deadline config")
        config, _ = _strict_json_object(
            config_path_object, "selected deadline config")
        benchmark = config.get("bench_e2e")
        cases = config.get("test_cases")
        if (not isinstance(benchmark, dict)
                or benchmark.get("fast") is not False
                or not isinstance(cases, dict) or len(cases) != 1
                or benchmark.get("matrix", {}).get("for")
                    != {next(iter(cases)): 5}
                or len(benchmark.get("matrix", {}).get("over", {})) != 1
                or "ksw2_batched" in json.dumps(config, sort_keys=True)):
            return None, "selected deadline config execution shape is invalid"
        data = next(iter(cases.values())).get("data")
        if (not isinstance(data, dict)
                or data.get("fragment_count")
                    != selected["identity"].get("query_count")
                or data.get("cigar_projection") is not True
                or data.get("mapq_projection") is not True):
            return None, "selected deadline config query/projection is invalid"
        for key, value in _walk_nested_items(config):
            if (key.lower() in {"prefilter", "prefilter_enabled", "use_prefilter"}
                    and value not in (False, 0, None, "off")):
                return None, "selected deadline config enables prefilter"
    except (OSError, UnicodeError, ValueError, KeyError, TypeError) as exc:
        return None, f"cannot validate deadline campaign authority: {exc}"
    return {
        "name": "deadline",
        "execution_role": "deadline_singleton",
        "manifest": _ALGORITHM_MATRIX_DEADLINE_MANIFEST,
        "manifest_sha256": manifest_sha,
        "campaign_digest": _ALGORITHM_MATRIX_DEADLINE_CAMPAIGN_DIGEST,
        "selected_shard": copy.deepcopy(selected),
        "runtime_timeout_seconds": selected["runtime_timeout_seconds"],
        "automatic_merge": False,
        "all_results_eligible_after_terminal_audit": True,
        "q8_results_eligible": False,
    }, None


def _deadline_campaign_v2_authority(
        config_path: str) -> tuple[dict | None, str | None]:
    """Validate one exact v2 member and its immutable v1 lineage."""
    directory_prefix = _ALGORITHM_MATRIX_DEADLINE_V2_DIRECTORY + "/"
    textual_member = config_path.startswith(directory_prefix)
    deadline_directory = (
        REPOSITORY_ROOT / _ALGORITHM_MATRIX_DEADLINE_V2_DIRECTORY)
    if not textual_member:
        try:
            candidate = Path(config_path).expanduser()
            if not candidate.is_absolute():
                candidate = REPOSITORY_ROOT / candidate
            if candidate.resolve(strict=True).parent == deadline_directory.resolve(
                    strict=True):
                return None, (
                    "deadline v2 config must use one exact manifest-owned "
                    "repository-relative path; aliases are forbidden")
        except (OSError, RuntimeError, ValueError):
            pass
        return None, None

    try:
        manifest_path = _campaign_regular_repository_file(
            _ALGORITHM_MATRIX_DEADLINE_V2_MANIFEST,
            "deadline v2 manifest")
        manifest, manifest_raw = _strict_json_object(
            manifest_path, "deadline v2 manifest")
        manifest_sha = hashlib.sha256(manifest_raw).hexdigest()
        if manifest_sha != _ALGORITHM_MATRIX_DEADLINE_V2_MANIFEST_SHA256:
            return None, "deadline v2 manifest SHA-256 is not pinned current"
        if set(manifest) != {
                "schema", "version", "lineage", "campaign", "source_bindings",
                "calibration_anchors", "runtime_contract", "coverage",
                "deadline_exclusions", "publication_contract",
                "deterministic_first_shard", "schedule"}:
            return None, "deadline v2 manifest has the wrong exact shape"
        lineage = manifest.get("lineage")
        campaign = manifest.get("campaign")
        runtime = manifest.get("runtime_contract")
        publication = manifest.get("publication_contract")
        source_bindings = manifest.get("source_bindings")
        schedule = manifest.get("schedule")
        first = manifest.get("deterministic_first_shard")
        expected_shards = campaign.get("expected_shards") \
            if isinstance(campaign, dict) else None
        expected_raw_records = campaign.get("expected_raw_records") \
            if isinstance(campaign, dict) else None
        if (manifest.get("schema")
                != "bioseq.e2e.algorithm-matrix-deadline-campaign"
                or manifest.get("version") != 2
                or lineage != _ALGORITHM_MATRIX_DEADLINE_V2_LINEAGE
                or not isinstance(campaign, dict)
                or campaign.get("campaign_id")
                    != _ALGORITHM_MATRIX_DEADLINE_V2_CAMPAIGN_ID
                or campaign.get("campaign_digest")
                    != _ALGORITHM_MATRIX_DEADLINE_V2_CAMPAIGN_DIGEST
                or campaign.get("q1_membership") != 0
                or campaign.get("q1_is_estimate_reference_only") is not True
                or campaign.get("repetitions_per_cell") != 5
                or type(expected_shards) is not int
                or expected_shards
                    != _ALGORITHM_MATRIX_DEADLINE_V2_EXPECTED_SHARDS
                or campaign.get("expected_result_cells") != expected_shards
                or type(expected_raw_records) is not int
                or expected_raw_records
                    != _ALGORITHM_MATRIX_DEADLINE_V2_EXPECTED_RAW_RECORDS
                or expected_raw_records != expected_shards * 5
                or campaign.get("estimated_wall_seconds")
                    != _ALGORITHM_MATRIX_DEADLINE_V2_ESTIMATED_WALL_SECONDS
                or campaign.get("deadline_seconds")
                    != _ALGORITHM_MATRIX_DEADLINE_V2_DEADLINE_SECONDS
                or campaign.get("estimated_wall_seconds")
                    >= campaign.get("deadline_seconds")
                or campaign.get("operational_margin_seconds")
                    != campaign.get("deadline_seconds")
                        - campaign.get("estimated_wall_seconds")
                or not isinstance(runtime, dict)
                or runtime.get("mode") != "benchmark-e2e"
                or runtime.get("run_only_required") is not True
                or runtime.get("exact_benchmark_filter")
                    != _ALGORITHM_MATRIX_EXACT_BENCH_FILTER
                or runtime.get("prefilter_enabled") is not False
                or runtime.get("cigar_projection") is not True
                or runtime.get("mapq_projection") is not True
                or runtime.get("ksw2_batched_excluded") is not True
                or runtime.get("runtime_proof_counters_retained") is not True
                or runtime.get("normal_timeout_seconds")
                    != _ALGORITHM_MATRIX_DEADLINE_NORMAL_TIMEOUT_SECONDS
                or runtime.get("extended_timeout_seconds")
                    != _ALGORITHM_MATRIX_DEADLINE_EXTENDED_TIMEOUT_SECONDS
                or runtime.get("extended_timeout_shard_count") != 2
                or runtime.get("sequential_only") is not True
                or publication != {
                    "classification": "production_all_results_candidate_rows",
                    "q1_calibration": False,
                    "merge_into_ALL_RESULTS_yaml":
                        "eligible_after_terminal_per_shard_audit",
                    "automatic_merge_during_run_only": False,
                    "required_terminal_gate": (
                        "five records, accepted or harness-proven rejection, exact "
                        "parity, runtime proof counters, fallback comments, and "
                        "sealed run receipt"),
                    "merge_into_Q8_RESULTS_yaml": False,
                    "existing_ledgers_unchanged_by_generation": True,
                }
                or not isinstance(source_bindings, dict)
                or not isinstance(schedule, list)
                or len(schedule) != expected_shards
                or not isinstance(first, dict)):
            return None, "deadline v2 runtime/publication contract is invalid"

        parent_manifest_path = _campaign_regular_repository_file(
            _ALGORITHM_MATRIX_DEADLINE_MANIFEST,
            "deadline v2 parent manifest")
        parent_manifest, parent_raw = _strict_json_object(
            parent_manifest_path, "deadline v2 parent manifest")
        if hashlib.sha256(parent_raw).hexdigest() \
                != _ALGORITHM_MATRIX_DEADLINE_MANIFEST_SHA256:
            return None, "deadline v2 parent manifest SHA-256 changed"
        parent_schedule = parent_manifest.get("schedule")
        if (not isinstance(parent_schedule, list) or len(parent_schedule) != 59
                or not isinstance(parent_schedule[0], dict)
                or not isinstance(parent_schedule[0].get("path"), str)):
            return None, "deadline v2 parent schedule is malformed"
        parent_authority, parent_error = _deadline_campaign_authority(
            parent_schedule[0]["path"],
            validate_current_source_bindings=False,
        )
        if (parent_error is not None or parent_authority is None
                or parent_authority.get("manifest_sha256")
                    != _ALGORITHM_MATRIX_DEADLINE_MANIFEST_SHA256
                or parent_authority.get("campaign_digest")
                    != _ALGORITHM_MATRIX_DEADLINE_CAMPAIGN_DIGEST):
            return None, (
                "deadline v2 immutable parent authority is invalid: "
                f"{parent_error or 'identity mismatch'}")

        for relative, expected_sha in source_bindings.items():
            source_path = _campaign_regular_repository_file(
                relative, "deadline v2 source binding")
            if (not isinstance(expected_sha, str)
                    or re.fullmatch(r"[0-9a-f]{64}", expected_sha) is None
                    or sha256_file(source_path) != expected_sha):
                return None, "deadline v2 source binding is stale"

        exact_entry_keys = {
            "ordinal", "path", "config_sha256", "stable_key",
            "stable_key_sha256", "identity", "repetitions",
            "expected_records", "allowed_statuses", "expected_disposition",
            "metric_names", "timed_estimate_seconds", "fixed_seconds",
            "contingency_multiplier", "estimated_wall_seconds",
            "runtime_timeout_seconds",
        }
        selected = None
        unique_paths: set[str] = set()
        unique_keys: set[str] = set()
        unique_hashes: set[str] = set()
        extended_entries = []
        estimated_total = 0
        for ordinal, entry in enumerate(schedule):
            if (not isinstance(entry, dict) or set(entry) != exact_entry_keys
                    or entry.get("ordinal") != ordinal
                    or entry.get("repetitions") != 5
                    or entry.get("expected_records") != 5
                    or not isinstance(entry.get("path"), str)
                    or not entry["path"].startswith(directory_prefix)
                    or not isinstance(entry.get("config_sha256"), str)
                    or re.fullmatch(
                        r"[0-9a-f]{64}", entry["config_sha256"]) is None
                    or not isinstance(entry.get("stable_key"), str)
                    or not entry["stable_key"].startswith(
                        "bioseq-e2e-deadline-cell-v2:")
                    or hashlib.sha256(
                        entry["stable_key"].encode("utf-8")).hexdigest()
                        != entry.get("stable_key_sha256")
                    or not isinstance(entry.get("identity"), dict)
                    or entry["identity"].get("campaign_id")
                        != _ALGORITHM_MATRIX_DEADLINE_V2_CAMPAIGN_ID
                    or not isinstance(entry.get("metric_names"), list)
                    or entry.get("runtime_timeout_seconds") not in {
                        _ALGORITHM_MATRIX_DEADLINE_NORMAL_TIMEOUT_SECONDS,
                        _ALGORITHM_MATRIX_DEADLINE_EXTENDED_TIMEOUT_SECONDS}
                    or entry.get("estimated_wall_seconds", 0) <= 0
                    or entry["estimated_wall_seconds"]
                        >= entry["runtime_timeout_seconds"]
                    or entry["path"] in unique_paths
                    or entry["stable_key"] in unique_keys
                    or entry["config_sha256"] in unique_hashes):
                return None, "deadline v2 schedule entry is malformed"
            unique_paths.add(entry["path"])
            unique_keys.add(entry["stable_key"])
            unique_hashes.add(entry["config_sha256"])
            estimated_total += entry["estimated_wall_seconds"]
            candidate_path = _campaign_regular_repository_file(
                entry["path"], "deadline v2 config")
            if sha256_file(candidate_path) != entry["config_sha256"]:
                return None, "deadline v2 config bytes differ from the seal"
            if entry["runtime_timeout_seconds"] \
                    == _ALGORITHM_MATRIX_DEADLINE_EXTENDED_TIMEOUT_SECONDS:
                extended_entries.append(entry)
            if entry["path"] == config_path:
                selected = entry
        expected_extended_identities = [
            {
                "campaign_id": _ALGORITHM_MATRIX_DEADLINE_V2_CAMPAIGN_ID,
                "tier": "q1024_anchor",
                "query_count": 1024,
                "dataset": "M2_D4_K15_W10",
                "index_mode": "just_index",
                "engine": "bioseq",
                "bioseq_index_name": "minimizers_per_fragment",
                "requested_minimizer_algorithm": "scalar",
                "requested_chaining_algorithm": "scalar_minimap2",
                "requested_base_alignment_algorithm": "ksw2",
            },
            {
                "campaign_id": _ALGORITHM_MATRIX_DEADLINE_V2_CAMPAIGN_ID,
                "tier": "q1024_anchor",
                "query_count": 1024,
                "dataset": "M2_D4_K15_W10",
                "index_mode": "no_index",
                "engine": "bioseq",
                "bioseq_index_name": "minimizers_per_fragment",
                "requested_minimizer_algorithm": "scalar",
                "requested_chaining_algorithm": "scalar_minimap2",
                "requested_base_alignment_algorithm": "ksw2",
            },
        ]
        if (estimated_total != campaign.get("estimated_wall_seconds")
                or len(unique_paths) != expected_shards
                or len(unique_keys) != expected_shards
                or len(unique_hashes) != expected_shards
                or len(extended_entries)
                    != runtime.get("extended_timeout_shard_count")
                or [entry.get("ordinal") for entry in extended_entries]
                    != [58, 59]
                or [entry.get("identity") for entry in extended_entries]
                    != expected_extended_identities
                or first.get("path") != schedule[0]["path"]
                or first.get("config_sha256") != schedule[0]["config_sha256"]
                or selected is None):
            return None, "deadline v2 membership/timeout seal is invalid"

        config_path_object = _campaign_regular_repository_file(
            config_path, "selected deadline v2 config")
        config, _ = _strict_json_object(
            config_path_object, "selected deadline v2 config")
        benchmark = config.get("bench_e2e")
        cases = config.get("test_cases")
        if (not isinstance(benchmark, dict)
                or benchmark.get("fast") is not False
                or not isinstance(cases, dict) or len(cases) != 1
                or benchmark.get("matrix", {}).get("for")
                    != {next(iter(cases)): 5}
                or len(benchmark.get("matrix", {}).get("over", {})) != 1
                or "ksw2_batched" in json.dumps(config, sort_keys=True)):
            return None, "selected deadline v2 config execution shape is invalid"
        data = next(iter(cases.values())).get("data")
        if (not isinstance(data, dict)
                or data.get("fragment_count")
                    != selected["identity"].get("query_count")
                or data.get("cigar_projection") is not True
                or data.get("mapq_projection") is not True):
            return None, "selected deadline v2 query/projection is invalid"
        for key, value in _walk_nested_items(config):
            if (key.lower() in {"prefilter", "prefilter_enabled", "use_prefilter"}
                    and value not in (False, 0, None, "off")):
                return None, "selected deadline v2 config enables prefilter"
    except (OSError, UnicodeError, ValueError, KeyError, TypeError) as exc:
        return None, f"cannot validate deadline v2 authority: {exc}"
    return {
        "name": "deadline",
        "execution_role": "deadline_singleton",
        "manifest": _ALGORITHM_MATRIX_DEADLINE_V2_MANIFEST,
        "manifest_sha256": manifest_sha,
        "campaign_id": _ALGORITHM_MATRIX_DEADLINE_V2_CAMPAIGN_ID,
        "campaign_version": 2,
        "campaign_digest": _ALGORITHM_MATRIX_DEADLINE_V2_CAMPAIGN_DIGEST,
        "lineage": copy.deepcopy(lineage),
        "selected_shard": copy.deepcopy(selected),
        "runtime_timeout_seconds": selected["runtime_timeout_seconds"],
        "automatic_merge": False,
        "all_results_eligible_after_terminal_audit": True,
        "q8_results_eligible": False,
    }, None


def _q1_calibration_panel_authority(
        config_path: str) -> tuple[dict | None, str | None]:
    """Validate one exact member of the pinned, nonpublication q1 panel."""
    selected = config_path in _ALGORITHM_MATRIX_Q1_CALIBRATION_CONFIGS
    q1_directory = (
        REPOSITORY_ROOT / "bench/e2e_algorithm_matrix_q1_calibration_shards")
    if not selected:
        try:
            candidate = Path(config_path).expanduser()
            if not candidate.is_absolute():
                candidate = REPOSITORY_ROOT / candidate
            resolved = candidate.resolve(strict=True)
            if resolved.parent == q1_directory.resolve(strict=True):
                return None, (
                    "q1 calibration config must use one exact manifest-owned "
                    "repository-relative path; aliases and unsealed members are "
                    "forbidden")
        except (OSError, RuntimeError, ValueError):
            pass
        return None, None

    try:
        manifest_path = _campaign_regular_repository_file(
            _ALGORITHM_MATRIX_Q1_CALIBRATION_MANIFEST,
            "q1 calibration manifest")
        manifest, manifest_raw = _strict_json_object(
            manifest_path, "q1 calibration manifest")
        manifest_sha = hashlib.sha256(manifest_raw).hexdigest()
        if manifest_sha != _ALGORITHM_MATRIX_Q1_CALIBRATION_MANIFEST_SHA256:
            return None, "q1 calibration manifest SHA-256 is not pinned current"
        if set(manifest) != {
                "schema", "schema_version", "created_at", "updated_at",
                "identity", "status", "purpose", "panel_contract",
                "prefix_proof", "transformation_contract", "cells",
                "runtime_contract", "nonpublication_rule"}:
            return None, "q1 calibration manifest has the wrong exact shape"
        panel = manifest.get("panel_contract")
        runtime = manifest.get("runtime_contract")
        nonpublication = manifest.get("nonpublication_rule")
        proof_binding = manifest.get("prefix_proof")
        cells = manifest.get("cells")
        if (manifest.get("schema")
                != "bioseq.e2e_algorithm_matrix_q1_calibration_manifest"
                or manifest.get("schema_version") != 2
                or manifest.get("identity") != "Q1_CALIBRATION_PANEL"
                or manifest.get("status") != "local_only_static_configuration"
                or not isinstance(panel, dict)
                or panel.get("result_cells") != 5
                or panel.get("repetitions_per_cell") != 5
                or panel.get("repeat_indices") != list(range(5))
                or panel.get("expected_raw_records") != 25
                or panel.get("exact_nonoverlapping") is not True
                or panel.get("cell_ids") != [
                    "minimap2_no_index", "fragment_ksw2",
                    "fragment_block_aligner", "sequence_ksw2",
                    "sequence_block_aligner"]
                or not isinstance(runtime, dict)
                or runtime.get("mode") != "benchmark-e2e"
                or runtime.get("exact_benchmark_filter")
                    != _ALGORITHM_MATRIX_EXACT_BENCH_FILTER
                or runtime.get("fast") is not False
                or runtime.get("prefilter") is not False
                or runtime.get("pipeline_mode") != "task_parallel"
                or runtime.get("recompute_async_persistence") != "off"
                or runtime.get("cigar_projection") is not True
                or runtime.get("mapq_projection") is not True
                or runtime.get("keep_benchmark_artifacts") is not False
                or runtime.get("repetitions_per_cell") != 5
                or runtime.get("ksw2_batched_permitted") is not False
                or runtime.get(
                    "exact_parity_and_fallback_attestation_required") is not True
                or not isinstance(runtime.get("priority"), dict)
                or runtime["priority"].get("darwin_worker_nice") != 0
                or runtime["priority"].get(
                    "fail_before_launch_if_worker_nice_is_not_zero") is not True
                or runtime["priority"].get(
                    "runner_and_postgresql_backend_nice_observation_per_run")
                    is not True
                or runtime["priority"].get(
                    "postgresql_inheritance_claimed") is not False
                or not isinstance(nonpublication, dict)
                or nonpublication.get("merge_authority") != "none"
                or nonpublication.get(
                    "q1_must_not_enter_Q8_RESULTS_yaml") is not True
                or nonpublication.get(
                    "q1_must_not_enter_ALL_RESULTS_yaml") is not True
                or not isinstance(cells, list) or len(cells) != 5
                or not isinstance(proof_binding, dict)
                or proof_binding.get("path")
                    != "bench/bench_e2e_algorithm_matrix_q1_calibration_prefix_proof.json"
                or proof_binding.get("verdict")
                    != "proven_exact_first_query_prefix_of_q8_against_current_source"):
            return None, "q1 calibration panel/runtime contract is invalid"

        exact_cell_keys = {
            "ordinal", "id", "role", "path", "config_sha256",
            "q8_source_path", "q8_source_sha256", "test_case",
            "execution_identity", "metric_names", "metric_count",
            "repetitions", "expected_records", "measured_evidence",
        }
        selected_cell = None
        expected_bindings = []
        data_projection = None
        required_attestation_metrics = {
            "tuple_accepted", "match_count", "expected_match_count",
            "correct_match_count", "exact_primary_count_accepted",
            "correct_cigar_count", "cigar_replay_count", "mapq_result_count",
            "pipeline_validation_accepted", "stage_engine_matrix_valid",
            "backend_declines", "backend_fallbacks", "prefilter_activity_count",
        }
        for ordinal, (cell, expected_path) in enumerate(zip(
                cells, _ALGORITHM_MATRIX_Q1_CALIBRATION_CONFIGS,
                strict=True)):
            if (not isinstance(cell, dict) or set(cell) != exact_cell_keys
                    or cell.get("ordinal") != ordinal
                    or cell.get("path") != expected_path
                    or cell.get("test_case") != "M2_D4_K15_W10_Q1_CALIBRATION"
                    or cell.get("repetitions") != 5
                    or cell.get("expected_records") != 5
                    or not isinstance(cell.get("config_sha256"), str)
                    or re.fullmatch(
                        r"[0-9a-f]{64}", cell["config_sha256"]) is None
                    or not isinstance(cell.get("q8_source_path"), str)
                    or not isinstance(cell.get("q8_source_sha256"), str)
                    or re.fullmatch(
                        r"[0-9a-f]{64}", cell["q8_source_sha256"]) is None
                    or not isinstance(cell.get("metric_names"), list)
                    or cell.get("metric_count") != len(cell["metric_names"])
                    or not isinstance(cell.get("execution_identity"), dict)):
                return None, "q1 calibration cell binding is malformed"
            q1_path = _campaign_regular_repository_file(
                expected_path, "q1 calibration config")
            if sha256_file(q1_path) != cell["config_sha256"]:
                return None, "q1 calibration config bytes differ from the manifest"
            q8_path = _campaign_regular_repository_file(
                cell["q8_source_path"], "q1 calibration q8 source")
            if sha256_file(q8_path) != cell["q8_source_sha256"]:
                return None, "q1 calibration q8 source bytes differ from the manifest"
            config, _ = _strict_json_object(q1_path, "q1 calibration config")
            cases = config.get("test_cases")
            bench = config.get("bench_e2e")
            if (not isinstance(cases, dict)
                    or set(cases) != {"M2_D4_K15_W10_Q1_CALIBRATION"}
                    or not isinstance(bench, dict)
                    or bench.get("fast") is not False
                    or bench.get("tags") != [
                        "e2e", "algorithm-matrix", "calibration-shard",
                        "q1-calibration"]
                    or bench.get("matrix", {}).get("for")
                        != {"M2_D4_K15_W10_Q1_CALIBRATION": 5}):
                return None, "q1 calibration config execution shape is invalid"
            data = cases["M2_D4_K15_W10_Q1_CALIBRATION"].get("data")
            metrics = bench.get("matrix", {}).get("metrics")
            over = bench.get("matrix", {}).get("over")
            if (not isinstance(data, dict)
                    or data.get("fragment_count") != 1
                    or data.get("cigar_projection") is not True
                    or data.get("mapq_projection") is not True
                    or data.get("keep_benchmark_artifacts") is True
                    or not isinstance(metrics, dict)
                    or list(metrics) != cell["metric_names"]
                    or not isinstance(over, dict)):
                return None, "q1 calibration query/metrics contract is invalid"
            serialized = json.dumps(config, sort_keys=True)
            if "ksw2_batched" in serialized:
                return None, "q1 calibration forbids ksw2_batched"
            for key, value in _walk_nested_items(config):
                if (key.lower() in {
                        "prefilter", "prefilter_enabled", "use_prefilter"}
                        and value not in (False, 0, None, "off")):
                    return None, "q1 calibration prefilter must remain disabled"
            projection = copy.deepcopy(data)
            projection.pop("fragment_count")
            if data_projection is None:
                data_projection = projection
            elif data_projection != projection:
                return None, "q1 calibration generation data differs by cell"
            identity = cell["execution_identity"]
            if identity.get("engine") == "minimap2":
                if (set(over) != {"minimap2_no_index"}
                        or cell["metric_count"] != 21):
                    return None, "q1 minimap2 baseline identity is invalid"
            elif identity.get("engine") == "bioseq":
                if (set(over) != {"bioseq_algorithm_matrix_no_index"}
                        or cell["metric_count"] != 29
                        or not required_attestation_metrics.issubset(metrics)):
                    return None, "q1 Bioseq attestation metrics are incomplete"
                axes = over["bioseq_algorithm_matrix_no_index"]
                actual_identity = {
                    "algorithm": "bioseq_algorithm_matrix_no_index",
                    "engine": "bioseq",
                    "index_mode": "no_index",
                    "minimizer_algorithm": axes.get("minimizer_algorithm", [None])[0],
                    "chaining_algorithm": axes.get("chaining_algorithm", [None])[0],
                    "base_alignment_algorithm": axes.get(
                        "base_alignment_algorithm", [None])[0],
                    "minimizer_index": axes.get("minimizer_index", [None])[0],
                }
                if (identity != actual_identity
                        or axes.get("pipeline_mode") != ["task_parallel"]
                        or axes.get("recompute_async_persistence") != ["off"]):
                    return None, "q1 Bioseq execution identity is invalid"
            else:
                return None, "q1 calibration engine identity is invalid"
            expected_bindings.append({
                "ordinal": ordinal,
                "id": cell["id"],
                "q1_path": expected_path,
                "q1_sha256": cell["config_sha256"],
                "q8_source_path": cell["q8_source_path"],
                "q8_source_sha256": cell["q8_source_sha256"],
                "q1_fragment_count": 1,
                "q8_fragment_count": 8,
            })
            if expected_path == config_path:
                selected_cell = cell
        if selected_cell is None:
            return None, "q1 calibration config is not manifest-owned"

        proof_path = _campaign_regular_repository_file(
            proof_binding["path"], "q1 calibration prefix proof")
        proof, proof_raw = _strict_json_object(
            proof_path, "q1 calibration prefix proof")
        proof_sha = hashlib.sha256(proof_raw).hexdigest()
        source_bindings = proof.get("current_source_bindings")
        generation = proof.get("generation_data_without_fragment_count")
        validation = proof.get("validation_scope")
        reasoning = proof.get("deterministic_prefix_reasoning")
        if (proof_sha != proof_binding.get("sha256")
                or set(proof) != {
                    "schema", "schema_version", "identity", "verdict",
                    "config_bindings", "current_source_bindings",
                    "generation_data_without_fragment_count",
                    "deterministic_prefix_reasoning", "validation_scope"}
                or proof.get("schema")
                    != "bioseq.e2e_algorithm_matrix_q1_calibration_prefix_proof"
                or proof.get("schema_version") != 2
                or proof.get("identity") != "Q1_CALIBRATION_PANEL"
                or proof.get("verdict") != proof_binding.get("verdict")
                or proof.get("config_bindings") != expected_bindings
                or not isinstance(source_bindings, dict)
                or set(source_bindings) != {
                    "bench/e2e/index.cppm",
                    "bioseq/backend/sequences/generation/sequence_generator.cppm"}
                or any(sha256_file(_campaign_regular_repository_file(
                    path, "q1 prefix-proof source")) != digest
                    for path, digest in source_bindings.items())
                or not isinstance(generation, dict)
                or generation.get("value") != data_projection
                or generation.get("canonical_json_sha256")
                    != canonical_json_sha256(data_projection)
                or not isinstance(reasoning, dict)
                or reasoning.get("fragment_count_is_query_count") is not True
                or reasoning.get("reference_independent_of_fragment_count")
                    is not True
                or reasoning.get("q1_fragment_indices") != [0]
                or reasoning.get("q8_fragment_indices") != list(range(8))
                or reasoning.get("formal_relation")
                    != "q1_queries == q8_queries[0:1]"
                or not isinstance(validation, dict)
                or validation.get("pair_count") != 5
                or validation.get("q1_cells") != 5
                or validation.get("q1_records") != 25
                or validation.get("exact_q8_to_q1_transformation") is not True
                or validation.get("all_other_json_content_matches") is not True):
            return None, "q1 calibration prefix-proof binding is invalid"
    except (OSError, UnicodeError, ValueError, KeyError, TypeError, IndexError) as exc:
        return None, f"cannot validate q1 calibration authority: {exc}"
    return {
        "name": "q1",
        "execution_role": "q1_calibration_singleton",
        "manifest": _ALGORITHM_MATRIX_Q1_CALIBRATION_MANIFEST,
        "manifest_sha256": manifest_sha,
        "query_prefix_proof": proof_binding["path"],
        "query_prefix_proof_sha256": proof_sha,
        "selected_cell": copy.deepcopy(selected_cell),
        "nonpublication": True,
    }, None


def _observed_rung_singleton_partition_authority(
        *, config_path: str, profile: Mapping[str, object],
        campaign_contract: Mapping[str, object],
        campaign_contract_raw: bytes,
) -> tuple[dict | None, str | None]:
    """Validate one selected child of a pinned observed-rung partition."""
    pattern = profile.get("singleton_pattern")
    if not isinstance(pattern, str) or re.fullmatch(pattern, config_path) is None:
        return None, "observed singleton path does not match the reserved syntax"
    try:
        query_count = int(profile["query_count"])
        manifest_relative = str(profile["singleton_partition_manifest"])
        manifest_path = _campaign_regular_repository_file(
            manifest_relative,
            f"q{query_count} observed singleton partition manifest")
        manifest, manifest_raw = _strict_json_object(
            manifest_path,
            f"q{query_count} observed singleton partition manifest")
        expected_manifest_sha = profile.get(
            "singleton_partition_manifest_sha256")
        if (not isinstance(expected_manifest_sha, str)
                or hashlib.sha256(manifest_raw).hexdigest()
                    != expected_manifest_sha):
            return None, (
                "observed singleton partition SHA-256 is not pinned current")
        if set(manifest) != {
                "schema", "version", "campaign_binding",
                "partition_contract", "parents", "children"}:
            return None, "observed singleton partition has the wrong exact shape"
        binding = manifest.get("campaign_binding")
        partition = manifest.get("partition_contract")
        parents = manifest.get("parents")
        children = manifest.get("children")
        campaign = campaign_contract.get("campaign")
        source = campaign_contract.get("source")
        rung = campaign_contract.get("rung")
        ownership = campaign_contract.get("shard_ownership")
        entries = campaign_contract.get("configs")
        if (manifest.get("schema") != (
                "bioseq.e2e.algorithm-matrix-observed-rung-"
                "singleton-partition")
                or manifest.get("version") != 1
                or not isinstance(binding, dict)
                or not isinstance(campaign, dict)
                or not isinstance(source, dict)
                or not isinstance(rung, dict)
                or not isinstance(ownership, dict)
                or not isinstance(entries, list)
                or binding != {
                    "campaign_id": _ALGORITHM_MATRIX_CAMPAIGN_ID,
                    "campaign_digest": profile["campaign_digest"],
                    "execution_contract_id": (
                        f"bioseq-e2e-algorithm-matrix-local-q{query_count}-"
                        "observed-v1"),
                    "result_status_contract": "observed_status_v1",
                    "query_count": query_count,
                    "canonical_manifest": profile["manifest"],
                    "canonical_manifest_sha256": hashlib.sha256(
                        campaign_contract_raw).hexdigest(),
                    "canonical_config": source.get("full_config"),
                    "canonical_config_sha256": source.get(
                        "full_config_sha256"),
                    "canonical_parent_config_count": 256,
                    "canonical_result_cells": 1296,
                    "canonical_raw_records": 6480,
                    "canonical_stable_keys_sha256": ownership.get(
                        "stable_keys_sha256"),
                    "fixture_projection_sha256": rung.get(
                        "fixture_projection_sha256"),
                    "predecessor_execution_contract_id": rung.get(
                        "predecessor_execution_contract_id"),
                }
                or not isinstance(partition, dict)
                or not isinstance(parents, list) or len(parents) != 256
                or not isinstance(children, list) or len(children) != 1296):
            return None, (
                "observed singleton campaign binding/cardinality is invalid")
        claimed_partition_digest = partition.get("partition_digest")
        digest_payload = copy.deepcopy(manifest)
        digest_payload["partition_contract"].pop("partition_digest", None)
        first_launch = (
            f"bench/e2e_algorithm_matrix_q{query_count}_observed_singletons/"
            f"bench_e2e_algorithm_matrix_q{query_count}_observed_singleton_"
            "0000_0ed6b7419bc8e21f.json")
        if (not isinstance(claimed_partition_digest, str)
                or re.fullmatch(
                    r"[0-9a-f]{64}", claimed_partition_digest) is None
                or canonical_json_sha256(digest_payload)
                    != claimed_partition_digest
                or partition != {
                    "child_config_count": 1296,
                    "cells_per_child": 1,
                    "records_per_child": 5,
                    "repetitions_per_cell": 5,
                    "repeat_indices": list(range(5)),
                    "normalization_axes": [
                        "test_case_index", "tuple_index"],
                    "canonical_ownership_preserved": True,
                    "parent_unions_exact": True,
                    "global_union_exact": True,
                    "stable_keys_sha256": binding.get(
                        "canonical_stable_keys_sha256"),
                    "metric_count_distribution": {"21": 16, "29": 1280},
                    "cell_disposition_distribution": {
                        "timed_bioseq": 1280,
                        "minimap2_baseline": 16,
                    },
                    "accepted_only": True,
                    "prefilter_enabled": False,
                    "pipeline_mode": "task_parallel",
                    "cold_no_index_persistence": "off",
                    "cigar_projection": True,
                    "mapq_projection": True,
                    "paired_minimap2_oracle": False,
                    "base_alignment_algorithms": ["ksw2", "block_aligner"],
                    "excluded_base_alignment_algorithms": ["ksw2_batched"],
                    "cross_rung_membership_permitted": False,
                    "reserved_filename_regex": pattern,
                    "deterministic_first_launch": first_launch,
                    "partition_digest_algorithm": (
                        "sha256-canonical-json-without-partition_digest"),
                    "partition_digest": claimed_partition_digest,
                }):
            return None, "observed singleton partition contract is invalid"

        canonical_parents = {
            entry.get("path"): entry for entry in entries
            if isinstance(entry, dict) and entry.get("role") == "shard"}
        if len(canonical_parents) != 256:
            return None, "observed singleton canonical parent membership changed"
        parent_ordinals: dict[str, set[int]] = {}
        all_claimed_ordinals: list[int] = []
        for parent_ordinal, proof in enumerate(parents):
            if (not isinstance(proof, dict)
                    or set(proof) != {
                        "ordinal", "path", "config_sha256",
                        "expected_result_cells", "expected_raw_records",
                        "expected_stable_keys_sha256", "child_ordinals"}
                    or proof.get("ordinal") != parent_ordinal):
                return None, "observed singleton parent proof is malformed"
            parent = canonical_parents.get(proof.get("path"))
            child_ordinals = proof.get("child_ordinals")
            if (parent is None or not isinstance(child_ordinals, list)
                    or len(child_ordinals)
                        != parent.get("expected_result_cells")
                    or any(isinstance(value, bool) or not isinstance(value, int)
                           for value in child_ordinals)
                    or proof.get("config_sha256")
                        != parent.get("config_sha256")
                    or proof.get("expected_result_cells")
                        != parent.get("expected_result_cells")
                    or proof.get("expected_raw_records")
                        != parent.get("expected_raw_records")
                    or proof.get("expected_stable_keys_sha256")
                        != parent.get("expected_stable_keys_sha256")
                    or parent.get("expected_all_records_disposition") is not None
                    or parent.get("allowed_nonblocking_rule_names") != []):
                return None, "observed singleton parent binding is invalid"
            parent_ordinals[str(proof["path"])] = set(child_ordinals)
            all_claimed_ordinals.extend(child_ordinals)
        if (len(all_claimed_ordinals) != 1296
                or sorted(all_claimed_ordinals) != list(range(1296))):
            return None, "observed singleton parent ordinal partition is invalid"

        exact_child_keys = {
            "ordinal", "path", "config_sha256", "parent_path",
            "parent_config_sha256", "parent_expected_stable_keys_sha256",
            "parent_test_case_index", "parent_tuple_index", "stable_key",
            "stable_key_sha256", "expected_stable_keys_sha256",
            "expected_result_cells", "expected_raw_records",
            "repetitions_per_cell", "declared_metric_names",
            "expected_all_records_disposition", "canonical_cell",
            "execution_cell",
        }
        selected = None
        all_keys: list[str] = []
        parent_keys: dict[str, list[str]] = {
            path: [] for path in canonical_parents}
        metric_counts: dict[int, int] = {}
        dispositions = {"timed_bioseq": 0, "minimap2_baseline": 0}
        for ordinal, child in enumerate(children):
            if (not isinstance(child, dict) or set(child) != exact_child_keys
                    or child.get("ordinal") != ordinal):
                return None, "observed singleton child proof is malformed"
            path = child.get("path")
            parent_path = child.get("parent_path")
            stable_key = child.get("stable_key")
            canonical_cell = child.get("canonical_cell")
            execution_cell = child.get("execution_cell")
            parent = canonical_parents.get(parent_path)
            if (not isinstance(path, str) or re.fullmatch(pattern, path) is None
                    or not isinstance(stable_key, str)
                    or not isinstance(canonical_cell, dict)
                    or not isinstance(execution_cell, dict)
                    or parent is None
                    or ordinal not in parent_ordinals[str(parent_path)]):
                return None, "observed singleton child identity is invalid"
            stable_sha = hashlib.sha256(stable_key.encode("utf-8")).hexdigest()
            expected_path = (
                f"bench/e2e_algorithm_matrix_q{query_count}_"
                "observed_singletons/"
                f"bench_e2e_algorithm_matrix_q{query_count}_"
                "observed_singleton_"
                f"{ordinal:04d}_{stable_sha[:16]}.json")
            normalized = copy.deepcopy(canonical_cell)
            normalized["test_case_index"] = 0
            normalized["tuple_index"] = 0
            parent_cells = parent.get("cells")
            parent_matches = [
                cell for cell in parent_cells
                if isinstance(cell, dict)
                and cell.get("test_case_index")
                    == child.get("parent_test_case_index")
                and cell.get("tuple_index")
                    == child.get("parent_tuple_index")
            ] if isinstance(parent_cells, list) else []
            identity = canonical_cell.get("identity")
            metrics = child.get("declared_metric_names")
            expected_metric_count = (
                21 if isinstance(identity, dict)
                and identity.get("engine") == "minimap2" else 29)
            if (path != expected_path
                    or child.get("stable_key_sha256") != stable_sha
                    or child.get("expected_stable_keys_sha256") != stable_sha
                    or child.get("expected_result_cells") != 1
                    or child.get("expected_raw_records") != 5
                    or child.get("repetitions_per_cell") != 5
                    or child.get("expected_all_records_disposition") is not None
                    or len(parent_matches) != 1
                    or canonical_cell != parent_matches[0]
                    or execution_cell != normalized
                    or canonical_cell.get("stable_key") != stable_key
                    or canonical_cell.get("allowed_statuses") != ["accepted"]
                    or not isinstance(identity, dict)
                    or tuple(identity) != tuple(
                        _ALGORITHM_MATRIX_STABLE_KEY_FIELDS)
                    or identity.get("campaign_id")
                        != _ALGORITHM_MATRIX_CAMPAIGN_ID
                    or child.get("parent_config_sha256")
                        != parent.get("config_sha256")
                    or child.get("parent_expected_stable_keys_sha256")
                        != parent.get("expected_stable_keys_sha256")
                    or not isinstance(metrics, list)
                    or metrics != parent.get("declared_metric_names")
                    or len(metrics) != expected_metric_count):
                return None, (
                    "observed singleton child ownership/status proof is invalid")
            bucket = (
                "minimap2_baseline"
                if identity.get("engine") == "minimap2" else "timed_bioseq")
            dispositions[bucket] += 1
            metric_counts[len(metrics)] = metric_counts.get(len(metrics), 0) + 1
            all_keys.append(stable_key)
            parent_keys[str(parent_path)].append(stable_key)
            if path == config_path:
                if selected is not None:
                    return None, "observed singleton path is duplicated"
                selected = child
        if (len(set(all_keys)) != 1296
                or hashlib.sha256(
                    "\n".join(sorted(all_keys)).encode("utf-8")).hexdigest()
                    != partition.get("stable_keys_sha256")
                or metric_counts != {21: 16, 29: 1280}
                or dispositions != {
                    "timed_bioseq": 1280,
                    "minimap2_baseline": 16,
                }):
            return None, "observed singleton global union is invalid"
        for parent_path, keys in parent_keys.items():
            parent = canonical_parents[parent_path]
            if hashlib.sha256(
                    "\n".join(sorted(keys)).encode("utf-8")).hexdigest() \
                    != parent.get("expected_stable_keys_sha256"):
                return None, "observed singleton parent union is invalid"
        if selected is None:
            return None, "observed singleton config is not manifest-owned"
        selected_path = _campaign_regular_repository_file(
            config_path, "observed singleton config")
        if sha256_file(selected_path) != selected.get("config_sha256"):
            return None, "observed singleton config bytes differ from the manifest"

        parent_path = _campaign_regular_repository_file(
            str(selected["parent_path"]), "observed singleton parent config")
        parent_config, _ = _strict_json_object(
            parent_path, "observed singleton parent config")
        selected_config, _ = _strict_json_object(
            selected_path, "observed singleton config")
        canonical_cell = selected["canonical_cell"]
        test_case = canonical_cell["test_case"]
        algorithm = canonical_cell["algorithm"]
        expected_config = copy.deepcopy(parent_config)
        expected_config["test_cases"] = {
            test_case: copy.deepcopy(parent_config["test_cases"][test_case])}
        expected_config["params"] = {
            algorithm: copy.deepcopy(parent_config["params"][algorithm])}
        parent_axes = parent_config["bench_e2e"]["matrix"]["over"][algorithm]
        child_axes = {
            parameter: [canonical_cell["params"][parameter]]
            for parameter in parent_axes}
        benchmark = expected_config["bench_e2e"]
        benchmark["tags"] = [
            "e2e", "algorithm-matrix", "campaign-singleton",
            f"q{query_count}-observed"]
        benchmark["description"] = (
            f"Sealed observed q{query_count} singleton execution descriptor "
            f"for canonical cell {selected['stable_key_sha256']}.")
        benchmark["matrix"]["for"] = {test_case: 5}
        benchmark["matrix"]["over"] = {algorithm: child_axes}
        expected_config["regression_rules"] = []
        data = expected_config["test_cases"][test_case]["data"]
        if (selected_config != expected_config
                or data.get("fragment_count") != query_count
                or data.get("cigar_projection") is not True
                or data.get("mapq_projection") is not True
                or data.get("paired_minimap2_oracle", False) is not False):
            return None, (
                "observed singleton config changed its parent projection/stats")
    except (OSError, RuntimeError, UnicodeError, ValueError, KeyError,
            TypeError, IndexError) as exc:
        return None, f"cannot validate observed singleton partition: {exc}"
    return {
        "execution_role": "observed_rung_singleton",
        "partition_manifest": manifest_relative,
        "partition_manifest_sha256": expected_manifest_sha,
        "selected_config": copy.deepcopy(selected),
        "canonical_parent": copy.deepcopy(
            canonical_parents[str(selected["parent_path"])]),
        "nonpublication": True,
    }, None


def _observed_rung_campaign_authority(
        config_path: str) -> tuple[dict | None, str | None]:
    """Bind one v2 observed-rung member without authorizing a ledger merge."""
    selected_profiles = [
        profile for profile in _ALGORITHM_MATRIX_OBSERVED_RUNG_PROFILES
        if config_path == profile["monolith"]
        or config_path.startswith(f"{profile['shard_directory']}/")
        or config_path.startswith(f"{profile['singleton_directory']}/")
    ]
    if not selected_profiles:
        # Exact canonical absolute paths are normalized by the public override
        # validator before this helper is called. Direct aliases into a reserved
        # observed-rung location must not fall through as ordinary overrides.
        try:
            candidate = Path(config_path).expanduser()
            if not candidate.is_absolute():
                candidate = REPOSITORY_ROOT / candidate
            resolved = candidate.resolve(strict=True)
            canonical_root = REPOSITORY_ROOT.resolve(strict=True)
            for profile in _ALGORITHM_MATRIX_OBSERVED_RUNG_PROFILES:
                monolith = canonical_root / str(profile["monolith"])
                shard_directory = (
                    canonical_root / str(profile["shard_directory"]))
                singleton_directory = (
                    canonical_root / str(profile["singleton_directory"]))
                if (resolved == monolith
                        or shard_directory in resolved.parents
                        or singleton_directory in resolved.parents):
                    return None, (
                        "observed-rung config must use one exact manifest-owned "
                        "repository-relative path; absolute, lexical, and symlink "
                        "aliases are forbidden")
        except (OSError, RuntimeError, ValueError):
            # Missing and out-of-tree ordinary overrides remain owned by the
            # public override validator.
            pass
        return None, None
    if len(selected_profiles) != 1:
        return None, "observed-rung config identity matches multiple profiles"
    profile = selected_profiles[0]

    try:
        config_path = _canonical_campaign_relative_path(
            config_path, "observed-rung config")
        query_count = int(profile["query_count"])
        rung_query_counts = tuple(
            int(candidate["query_count"])
            for candidate in _ALGORITHM_MATRIX_OBSERVED_RUNG_PROFILES)
        rung_index = rung_query_counts.index(query_count)
        predecessor_query_count = (
            rung_query_counts[rung_index - 1] if rung_index else None)
        predecessor_execution_contract_id = (
            f"bioseq-e2e-algorithm-matrix-local-q{predecessor_query_count}-"
            "observed-v1"
            if predecessor_query_count is not None else None)

        manifest_path = _campaign_regular_repository_file(
            str(profile["manifest"]),
            f"q{query_count} observed-rung manifest",
        )
        manifest, manifest_raw = _strict_json_object(
            manifest_path, f"q{query_count} observed-rung manifest")
        if set(manifest) != {
                "schema", "version", "campaign", "source", "configs",
                "shard_ownership", "rung"}:
            return None, "observed-rung manifest has the wrong exact shape"

        campaign = manifest.get("campaign")
        exact_campaign_keys = {
            "campaign_id", "campaign_digest_algorithm", "stable_key_schema",
            "stable_key_fields", "repetitions_per_cell",
            "expected_result_cells", "expected_raw_records",
            "required_shard_configs", "publication_status_contract",
            "query_count", "result_status_contract", "execution_contract_id",
            "campaign_digest",
        }
        if not isinstance(campaign, dict) or set(campaign) != exact_campaign_keys:
            return None, "observed-rung campaign has the wrong exact shape"
        digest_payload = copy.deepcopy(manifest)
        del digest_payload["campaign"]["campaign_digest"]
        expected_campaign_digest = str(profile["campaign_digest"])
        if (manifest.get("schema")
                != "bioseq.e2e.algorithm-matrix-campaign"
                or manifest.get("version") != 2
                or campaign.get("campaign_id")
                    != _ALGORITHM_MATRIX_CAMPAIGN_ID
                or campaign.get("campaign_digest_algorithm")
                    != "sha256-canonical-json-without-campaign_digest"
                or campaign.get("stable_key_schema") != "bioseq-e2e-cell-v2"
                or campaign.get("stable_key_fields")
                    != _ALGORITHM_MATRIX_STABLE_KEY_FIELDS
                or campaign.get("repetitions_per_cell") != 5
                or campaign.get("expected_result_cells") != 1296
                or campaign.get("expected_raw_records") != 6480
                or campaign.get("required_shard_configs") != 256
                or campaign.get("publication_status_contract") != {
                    "status_source": "observed",
                    "minimap2": ["accepted"],
                    "bioseq": ["accepted"],
                    "rejected_dispositions": ["rejected_setup", "rejected_sql"],
                    "nonpublishable_statuses": [
                        "rejected", "invalid", "pending"],
                }
                or campaign.get("query_count") != query_count
                or campaign.get("result_status_contract")
                    != "observed_status_v1"
                or campaign.get("execution_contract_id") != (
                    f"bioseq-e2e-algorithm-matrix-local-q{query_count}-"
                    "observed-v1")
                or campaign.get("campaign_digest") != expected_campaign_digest
                or canonical_json_sha256(digest_payload)
                    != expected_campaign_digest):
            return None, "observed-rung campaign identity/digest is invalid"

        source = manifest.get("source")
        if (not isinstance(source, dict)
                or set(source) != {
                    "full_config", "full_config_sha256", "generator"}
                or source.get("full_config") != profile["monolith"]
                or not isinstance(source.get("full_config_sha256"), str)
                or re.fullmatch(
                    r"[0-9a-f]{64}", source["full_config_sha256"]) is None
                or source.get("generator")
                    != "bench/generate_e2e_algorithm_matrix_shards.py"):
            return None, "observed-rung source binding is invalid"
        full_config_path = _campaign_regular_repository_file(
            source["full_config"], "observed-rung full config")
        if sha256_file(full_config_path) != source["full_config_sha256"]:
            return None, "observed-rung full-config digest changed"

        rung = manifest.get("rung")
        exact_rung_keys = {
            "schema", "version", "query_count", "ladder_query_counts",
            "predecessor_query_count", "predecessor_execution_contract_id",
            "query_ordinal_interval", "projection",
            "fixture_projection_sha256", "matrix_contract",
            "canonical_campaign_binding",
        }
        if (not isinstance(rung, dict) or set(rung) != exact_rung_keys
                or rung.get("schema")
                    != "bioseq.e2e.algorithm-matrix-observed-rung"
                or rung.get("version") != 1
                or rung.get("query_count") != query_count
                or rung.get("ladder_query_counts") != list(rung_query_counts)
                or rung.get("predecessor_query_count")
                    != predecessor_query_count
                or rung.get("predecessor_execution_contract_id")
                    != predecessor_execution_contract_id
                or rung.get("query_ordinal_interval") != {
                    "begin": 0, "end_exclusive": query_count}
                or rung.get("projection")
                    != "exact_fixed_seed_ordered_prefix_of_q1024"
                or not isinstance(rung.get("fixture_projection_sha256"), str)
                or re.fullmatch(
                    r"[0-9a-f]{64}", rung["fixture_projection_sha256"]) is None
                or rung.get("matrix_contract") != {
                    "all_axes_preserved": True,
                    "repetitions_per_cell": 5,
                    "prefilter_enabled": False,
                    "base_alignment_algorithms": ["ksw2", "block_aligner"],
                    "excluded_base_alignment_algorithms": ["ksw2_batched"],
                }):
            return None, "observed-rung ladder/projection contract is invalid"

        canonical_binding = rung.get("canonical_campaign_binding")
        if (not isinstance(canonical_binding, dict)
                or set(canonical_binding) != {
                    "campaign_id", "campaign_digest", "manifest",
                    "manifest_sha256", "config", "config_sha256"}
                or canonical_binding.get("campaign_id")
                    != _ALGORITHM_MATRIX_CAMPAIGN_ID
                or canonical_binding.get("campaign_digest")
                    != _ALGORITHM_MATRIX_CAMPAIGN_DIGEST
                or canonical_binding.get("manifest")
                    != "bench/bench_e2e_algorithm_matrix_campaign_contract.json"
                or canonical_binding.get("config")
                    != "bench/bench_e2e_algorithm_matrix_campaign.json"
                or not isinstance(
                    canonical_binding.get("manifest_sha256"), str)
                or re.fullmatch(
                    r"[0-9a-f]{64}",
                    canonical_binding["manifest_sha256"]) is None
                or not isinstance(canonical_binding.get("config_sha256"), str)
                or re.fullmatch(
                    r"[0-9a-f]{64}",
                    canonical_binding["config_sha256"]) is None):
            return None, "observed-rung canonical q1024 binding is invalid"
        canonical_manifest_path = _campaign_regular_repository_file(
            canonical_binding["manifest"], "observed-rung canonical manifest")
        canonical_config_path = _campaign_regular_repository_file(
            canonical_binding["config"], "observed-rung canonical config")
        if (sha256_file(canonical_manifest_path)
                != canonical_binding["manifest_sha256"]
                or sha256_file(canonical_config_path)
                    != canonical_binding["config_sha256"]):
            return None, "observed-rung canonical q1024 source bytes changed"

        entries = manifest.get("configs")
        ownership = manifest.get("shard_ownership")
        exact_entry_keys = {
            "path", "config_sha256", "role", "shard_kind",
            "expected_result_cells", "expected_raw_records",
            "expected_stable_keys_sha256", "allowed_nonblocking_rule_names",
            "declared_metric_names", "expected_all_records_disposition", "cells",
        }
        if (not isinstance(entries, list) or len(entries) != 257
                or not isinstance(ownership, dict)
                or set(ownership) != {
                    "exact_nonoverlapping", "shard_paths",
                    "stable_keys_sha256", "resume_semantics"}
                or ownership.get("exact_nonoverlapping") is not True
                or not isinstance(ownership.get("shard_paths"), list)
                or len(ownership["shard_paths"]) != 256):
            return None, "observed-rung config ownership is invalid"
        monolith_count = 0
        shard_paths = []
        for entry in entries:
            if (not isinstance(entry, dict) or set(entry) != exact_entry_keys
                    or not isinstance(entry.get("path"), str)
                    or not isinstance(entry.get("config_sha256"), str)
                    or re.fullmatch(
                        r"[0-9a-f]{64}", entry["config_sha256"]) is None):
                return None, "observed-rung config entry shape is invalid"
            if entry.get("role") == "monolithic":
                monolith_count += 1
                if (entry["path"] != profile["monolith"]
                        or entry.get("shard_kind") is not None):
                    return None, "observed-rung monolith identity changed"
            elif entry.get("role") == "shard":
                if re.fullmatch(
                        str(profile["shard_pattern"]), entry["path"]) is None:
                    return None, "observed-rung shard path escaped its profile"
                shard_paths.append(entry["path"])
            else:
                return None, "observed-rung config role is invalid"
        if (monolith_count != 1 or len(shard_paths) != 256
                or ownership["shard_paths"] != shard_paths):
            return None, "observed-rung monolith/shard topology changed"

        if re.fullmatch(
                str(profile["singleton_pattern"]), config_path) is not None:
            singleton_authority, singleton_error = (
                _observed_rung_singleton_partition_authority(
                    config_path=config_path,
                    profile=profile,
                    campaign_contract=manifest,
                    campaign_contract_raw=manifest_raw,
                ))
            if singleton_error is not None or singleton_authority is None:
                return None, (
                    singleton_error
                    or "observed singleton execution authority is missing")
            authority = dict(profile)
            authority.update(singleton_authority)
            authority["manifest_sha256"] = hashlib.sha256(
                manifest_raw).hexdigest()
            return authority, None

        selected_entries = [
            entry for entry in entries if entry["path"] == config_path]
        if len(selected_entries) != 1:
            return None, "observed-rung config path is not manifest-owned"
        selected_entry = selected_entries[0]
        expected_role = (
            "monolithic" if config_path == profile["monolith"] else "shard")
        if selected_entry["role"] != expected_role:
            return None, "observed-rung selected config role changed"
        canonical_config_path = _campaign_regular_repository_file(
            config_path, "observed-rung selected config")
        if sha256_file(canonical_config_path) != selected_entry["config_sha256"]:
            return None, "observed-rung selected config bytes changed"
    except (OSError, RuntimeError, UnicodeError, ValueError, KeyError, TypeError) as exc:
        return None, f"cannot validate observed-rung authority: {exc}"

    authority = dict(profile)
    authority.update({
        "execution_role": "observed_rung_topology",
        "manifest_sha256": hashlib.sha256(manifest_raw).hexdigest(),
        "selected_config": copy.deepcopy(selected_entry),
        "nonpublication": True,
    })
    return authority, None


def _algorithm_matrix_campaign_authority_status(
        config_path: str | None) -> tuple[dict | None, str | None]:
    """Bind one sealed algorithm-matrix config to its execution authority."""
    if config_path is None:
        return None, None
    if not isinstance(config_path, str) or not config_path:
        return None, "campaign config path must be a non-empty string"

    deadline_authority, deadline_error = _deadline_campaign_authority(config_path)
    if deadline_error is not None or deadline_authority is not None:
        return deadline_authority, deadline_error

    deadline_v2_authority, deadline_v2_error = (
        _deadline_campaign_v2_authority(config_path))
    if deadline_v2_error is not None or deadline_v2_authority is not None:
        return deadline_v2_authority, deadline_v2_error

    q1_authority, q1_error = _q1_calibration_panel_authority(config_path)
    if q1_error is not None or q1_authority is not None:
        return q1_authority, q1_error

    observed_authority, observed_error = (
        _observed_rung_campaign_authority(config_path))
    if observed_error is not None or observed_authority is not None:
        return observed_authority, observed_error

    selected_profiles = [
        profile for profile in _ALGORITHM_MATRIX_CAMPAIGN_PROFILES
        if config_path == profile["monolith"]
        or re.fullmatch(profile["shard_pattern"], config_path) is not None
        or (isinstance(profile.get("packed_pattern"), str)
            and re.fullmatch(profile["packed_pattern"], config_path) is not None)
    ]
    if not selected_profiles:
        # Campaign artifacts are irrelevant to ordinary explicit overrides.
        # Still reject an alias that resolves into either sealed campaign.
        try:
            candidate = Path(config_path).expanduser()
            if not candidate.is_absolute():
                candidate = REPOSITORY_ROOT / candidate
            resolved = candidate.resolve(strict=True)
            canonical_root = REPOSITORY_ROOT.resolve(strict=True)
            for profile in _ALGORITHM_MATRIX_CAMPAIGN_PROFILES:
                monolith = canonical_root / profile["monolith"]
                shard_directory = canonical_root / profile["shard_directory"]
                packed_pattern = profile.get("packed_pattern")
                relative = resolved.relative_to(canonical_root).as_posix()
                if (resolved == monolith
                        or resolved.parent == shard_directory
                        and re.fullmatch(
                            profile["shard_pattern"], relative) is not None
                        or isinstance(packed_pattern, str)
                        and re.fullmatch(packed_pattern, relative) is not None):
                    return None, (
                        "campaign config must use its exact canonical repository-"
                        "relative manifest path; absolute, lexical, and symlink "
                        "aliases are forbidden")
        except (OSError, RuntimeError, ValueError):
            # The ordinary override validator owns missing/out-of-tree errors.
            pass
        return None, None
    if len(selected_profiles) != 1:
        return None, "campaign config identity matches multiple profiles"
    profile = selected_profiles[0]

    try:
        manifest_path = REPOSITORY_ROOT / profile["manifest"]
        contract, contract_raw = _strict_json_object(
            manifest_path, f"{profile['name']} campaign manifest")
        if set(contract) != {
                "schema", "version", "campaign", "source", "configs",
                "shard_ownership"}:
            return None, "canonical campaign contract has the wrong exact shape"
        campaign = contract["campaign"]
        if not isinstance(campaign, dict) or set(campaign) != {
                "campaign_id", "campaign_digest_algorithm", "stable_key_schema",
                "stable_key_fields", "repetitions_per_cell",
                "expected_result_cells", "expected_raw_records",
                "required_shard_configs", "publication_status_contract",
                "campaign_digest"}:
            return None, "canonical campaign identity has the wrong exact shape"
        claimed_digest = campaign["campaign_digest"]
        digest_payload = copy.deepcopy(contract)
        del digest_payload["campaign"]["campaign_digest"]
        recomputed_digest = canonical_json_sha256(digest_payload)
        if (contract.get("schema") != "bioseq.e2e.algorithm-matrix-campaign"
                or contract.get("version") != 1
                or campaign.get("campaign_id") != profile["campaign_id"]
                or claimed_digest != profile["campaign_digest"]
                or claimed_digest != recomputed_digest):
            return None, "canonical campaign contract identity/digest is invalid"
        required_shards = profile["required_shards"]
        if (campaign.get("campaign_digest_algorithm")
                != "sha256-canonical-json-without-campaign_digest"
                or campaign.get("stable_key_schema") != "bioseq-e2e-cell-v2"
                or campaign.get("stable_key_fields")
                    != _ALGORITHM_MATRIX_STABLE_KEY_FIELDS
                or campaign.get("repetitions_per_cell") != 5
                or campaign.get("expected_result_cells") != 1296
                or campaign.get("expected_raw_records") != 6480
                or campaign.get("required_shard_configs") != required_shards):
            return None, "canonical campaign cardinality/key contract is invalid"
        entries = contract.get("configs")
        ownership = contract.get("shard_ownership")
        if (not isinstance(entries, list)
                or len(entries) != required_shards + 1
                or not isinstance(ownership, dict)
                or ownership.get("exact_nonoverlapping") is not True
                or not isinstance(ownership.get("shard_paths"), list)
                or len(ownership["shard_paths"]) != required_shards):
            return None, "canonical campaign config membership is invalid"
        exact_entry_keys = {
            "path", "config_sha256", "role", "shard_kind",
            "expected_result_cells", "expected_raw_records",
            "expected_stable_keys_sha256", "allowed_nonblocking_rule_names",
            "declared_metric_names", "expected_all_records_disposition", "cells",
        }
        monolith_count = 0
        shard_paths = []
        for entry in entries:
            if (not isinstance(entry, dict) or set(entry) != exact_entry_keys
                    or not isinstance(entry.get("path"), str)
                    or not isinstance(entry.get("config_sha256"), str)
                    or not re.fullmatch(r"[0-9a-f]{64}", entry["config_sha256"])):
                return None, (
                    "canonical campaign config entries have the wrong exact shape")
            if entry.get("role") == "monolithic":
                monolith_count += 1
                if (entry["path"] != profile["monolith"]
                        or entry.get("shard_kind") is not None):
                    return None, "canonical campaign monolith identity changed"
            elif entry.get("role") == "shard":
                if re.fullmatch(
                        profile["shard_pattern"], entry["path"]) is None:
                    return None, "canonical campaign shard path escaped its profile"
                shard_paths.append(entry["path"])
            else:
                return None, "canonical campaign config role is invalid"
            _campaign_regular_repository_file(
                entry["path"], "canonical campaign config entry")
        if (monolith_count != 1 or len(shard_paths) != required_shards
                or ownership["shard_paths"] != shard_paths):
            return None, "canonical campaign monolith/shard topology changed"

        singleton_authority = None
        packed_authority = None
        singleton_pattern = profile.get("singleton_pattern")
        is_singleton = (
            isinstance(singleton_pattern, str)
            and re.fullmatch(singleton_pattern, config_path) is not None)
        if is_singleton:
            singleton_authority, singleton_error = (
                _q1024_singleton_partition_authority(
                    config_path=config_path,
                    profile=profile,
                    campaign_contract=contract,
                    campaign_contract_raw=contract_raw,
                ))
            if singleton_error is not None or singleton_authority is None:
                return None, singleton_error or (
                    "q1024 singleton partition authority is missing")
            if profile.get("name") == "q1024":
                selected_singleton = singleton_authority.get("singleton")
                singleton_stable_key = (
                    selected_singleton.get("stable_key")
                    if isinstance(selected_singleton, dict) else None)
                _, packed_error = _q1024_packed_partition_authority(
                    config_path=config_path,
                    profile=profile,
                    campaign_contract=contract,
                    campaign_contract_raw=contract_raw,
                    singleton_stable_key=singleton_stable_key,
                )
                if packed_error is not None:
                    return None, packed_error
                singleton_authority["singleton_partition_manifest"] = (
                    singleton_authority["partition_manifest"])
                singleton_authority["singleton_partition_manifest_sha256"] = (
                    singleton_authority["partition_manifest_sha256"])
                singleton_authority["partition_manifest"] = profile[
                    "packed_partition_manifest"]
                singleton_authority["partition_manifest_sha256"] = profile[
                    "packed_partition_manifest_sha256"]
            canonical_path = _campaign_regular_repository_file(
                config_path, "q1024 singleton config")
        else:
            if profile.get("name") == "q1024":
                packed_authority, packed_error = (
                    _q1024_packed_partition_authority(
                        config_path=config_path,
                        profile=profile,
                        campaign_contract=contract,
                        campaign_contract_raw=contract_raw,
                    ))
                if packed_error is not None:
                    return None, packed_error
            if packed_authority is not None:
                canonical_path = _campaign_regular_repository_file(
                    config_path, "q1024 packed config")
            else:
                selected_entries = [
                    entry for entry in entries if entry.get("path") == config_path]
                if len(selected_entries) != 1:
                    return None, (
                        "campaign config must use its exact canonical repository-relative "
                        "contract, packed-partition, or singleton-partition path; "
                        "absolute, lexical, symlink, hardlink, and copied aliases are "
                        "forbidden")
                canonical_path = _campaign_regular_repository_file(
                    config_path, "campaign config")
                candidate_digest = sha256_file(canonical_path)
                expected_digest = selected_entries[0].get("config_sha256")
                if (not isinstance(expected_digest, str)
                        or not re.fullmatch(r"[0-9a-f]{64}", expected_digest)
                        or candidate_digest != expected_digest):
                    return None, (
                        "campaign config bytes differ from the canonical digest")

        prefix_proof_sha256 = None
        proof_relative = profile["query_prefix_proof"]
        if proof_relative is not None:
            proof_path = REPOSITORY_ROOT / proof_relative
            proof, proof_raw = _strict_json_object(
                proof_path, "q8 query-prefix proof")
            campaign_binding = proof.get("campaign_binding")
            q1024_binding = proof.get("q1024_binding")
            query_contract = proof.get("query_generation_contract")
            timeout_contract = proof.get("timeout_and_shard_contract")
            ownership_contract = proof.get("ownership_and_resume_contract")
            rescue_authority = proof.get("rescue_execution_authority")
            rescue_manifest_relative = (
                rescue_authority.get("manifest")
                if isinstance(rescue_authority, dict) else None)
            rescue_manifest_raw = None
            if (rescue_manifest_relative
                    == "bench/bench_e2e_algorithm_matrix_q8_ordinal1_rescue_manifest.json"):
                rescue_manifest_path = _campaign_regular_repository_file(
                    rescue_manifest_relative, "q8 rescue manifest")
                _, rescue_manifest_raw = _strict_json_object(
                    rescue_manifest_path, "q8 rescue manifest")
            source = contract.get("source")
            if (set(proof) != {
                    "schema", "version", "verdict", "campaign_binding",
                    "q1024_binding", "query_generation_contract",
                    "matrix_and_repeat_contract", "timeout_and_shard_contract",
                    "ownership_and_resume_contract",
                    "rescue_execution_authority"}
                    or proof.get("schema")
                        != "bioseq.e2e.algorithm-matrix-q8-prefix-proof"
                    or proof.get("version") != 2
                    or proof.get("verdict")
                        != "proven_exact_fixed_seed_ordered_prefix"
                    or not isinstance(campaign_binding, dict)
                    or campaign_binding.get("campaign_id")
                        != profile["campaign_id"]
                    or campaign_binding.get("campaign_digest")
                        != profile["campaign_digest"]
                    or campaign_binding.get("manifest") != profile["manifest"]
                    or campaign_binding.get("manifest_sha256")
                        != hashlib.sha256(contract_raw).hexdigest()
                    or not isinstance(source, dict)
                    or campaign_binding.get("config") != source.get("full_config")
                    or campaign_binding.get("config_sha256")
                        != source.get("full_config_sha256")
                    or not isinstance(q1024_binding, dict)
                    or q1024_binding.get("campaign_id")
                        != _ALGORITHM_MATRIX_CAMPAIGN_ID
                    or q1024_binding.get("campaign_digest")
                        != _ALGORITHM_MATRIX_CAMPAIGN_DIGEST
                    or not isinstance(query_contract, dict)
                    or query_contract.get("fragment_count_is_query_count") is not True
                    or query_contract.get("q8_fragment_indices") != list(range(8))
                    or query_contract.get("q8_is_subset_of_q1024") is not True
                    or query_contract.get("q8_is_ordered_prefix_of_q1024") is not True
                    or not isinstance(query_contract.get("datasets"), list)
                    or len(query_contract["datasets"]) != 8
                    or any(dataset.get("q8_fragment_count") != 8
                           or dataset.get("q1024_fragment_count") != 1024
                           for dataset in query_contract["datasets"]
                           if isinstance(dataset, dict))
                    or any(not isinstance(dataset, dict)
                           for dataset in query_contract["datasets"])
                    or not isinstance(timeout_contract, dict)
                    or timeout_contract.get("required_shards") != 256
                    or timeout_contract.get("no_index_micro_shards") != 160
                    or timeout_contract.get("isolated_minimap2_baseline_shards") != 16
                    or timeout_contract.get("persisted_index_shards") != 80
                    or not isinstance(ownership_contract, dict)
                    or ownership_contract.get("stable_keys_sha256")
                        != ownership.get("stable_keys_sha256")
                    or ownership_contract.get("exact_nonoverlapping") is not True
                    or ownership_contract.get(
                        "q8_is_eligible_for_q1024_automatic_merge") is not False
                    or not isinstance(rescue_authority, dict)
                    or set(rescue_authority) != {
                        "schema", "version", "ordinal", "manifest",
                        "manifest_sha256", "canonical_parent_union_cells",
                        "canonical_parent_union_records"}
                    or rescue_authority.get("schema")
                        != "bioseq.e2e.algorithm-matrix-q8-rescue"
                    or rescue_authority.get("version") != 2
                    or rescue_authority.get("ordinal") != 1
                    or rescue_authority.get("canonical_parent_union_cells") != 4
                    or rescue_authority.get("canonical_parent_union_records") != 20
                    or rescue_manifest_raw is None
                    or rescue_authority.get("manifest_sha256")
                        != hashlib.sha256(rescue_manifest_raw).hexdigest()):
                return None, "q8 query-prefix proof binding is invalid"
            q8_config = _strict_json_object(
                canonical_path, "q8 selected campaign config")[0]
            test_cases = q8_config.get("test_cases")
            if not isinstance(test_cases, dict) or not test_cases:
                return None, "q8 config has no exact test-case query contract"
            for test_case in test_cases.values():
                data = test_case.get("data") if isinstance(test_case, dict) else None
                if (not isinstance(data, dict)
                        or data.get("fragment_count") != 8
                        or data.get("keep_benchmark_artifacts") is True
                        or data.get("skip_index_baseline_setup") is True
                        or data.get("skip_minimap2_baseline_setup") is True):
                    return None, (
                        "q8 config query/artifact cleanup contract is invalid")
            prefix_proof_sha256 = hashlib.sha256(proof_raw).hexdigest()

        results_path = REPOSITORY_ROOT / profile["ledger"]
        results, _ = _strict_json_object(results_path, profile["ledger"])
        results_contract = results.get("contract")
        results_campaign = results.get("campaign")
        current_ledger_revision = (
            results.get("schema_version") == profile["ledger_schema_version"]
            and isinstance(results_contract, dict)
            and results_contract.get("version")
                == profile["ledger_contract_version"])
        legacy_state = results.get("aggregation_state")
        legacy_empty_revision = (
            profile.get("legacy_empty_ledger_schema_version") is not None
            and _legacy_q1024_ledger_is_exactly_empty(
                results,
                profile["campaign_id"],
                profile["campaign_digest"],
            ))
        proof_binding_current = (
            proof_relative is None
            or (isinstance(results_contract, dict)
                and results_contract.get("query_prefix_proof") == proof_relative
                and results_contract.get("query_prefix_proof_sha256")
                    == prefix_proof_sha256))
        proof_binding_refreshable = (
            profile.get("name") == "q8"
            and isinstance(results_contract, dict)
            and results_contract.get("query_prefix_proof") == proof_relative
            and isinstance(
                results_contract.get("query_prefix_proof_sha256"), str)
            and re.fullmatch(
                r"[0-9a-f]{64}",
                results_contract["query_prefix_proof_sha256"]) is not None)
        if (not (current_ledger_revision or legacy_empty_revision)
                or not isinstance(results_contract, dict)
                or results_contract.get("schema")
                    != profile["ledger_contract_schema"]
                or results_contract.get("campaign_id")
                    != profile["campaign_id"]
                or results_contract.get("campaign_digest")
                    != profile["campaign_digest"]
                or results_contract.get("campaign_contract")
                    != profile["manifest"]
                or results_contract.get("campaign_contract_sha256")
                    != hashlib.sha256(contract_raw).hexdigest()
                or not (proof_binding_current or proof_binding_refreshable)
                or not isinstance(results_campaign, dict)
                or results_campaign.get("campaign_id")
                    != profile["campaign_id"]
                or results_campaign.get("campaign_digest")
                    != profile["campaign_digest"]):
            return None, (
                f"{profile['ledger']} does not bind the audited campaign authority")
        processed_reports = (
            legacy_state.get("processed_reports")
            if isinstance(legacy_state, dict) else None)
        if ((singleton_authority is not None or packed_authority is not None)
                and processed_reports == []
                and config_path != (
                    "bench/e2e_algorithm_matrix_deadline_v2_shards/"
                    "bench_e2e_algorithm_matrix_deadline_v2_s056_q1024_"
                    "q1024_anchor_m2_d4_k15_w10_no_index_minimap2.json")):
            return None, (
                "the empty q1024 ledger must start with deterministic baseline "
                "adoption s056 before any new packed/singleton execution")
    except (OSError, UnicodeError, ValueError, KeyError, TypeError) as exc:
        return None, f"cannot validate canonical campaign contract: {exc}"
    authority = dict(profile)
    authority["manifest_sha256"] = hashlib.sha256(contract_raw).hexdigest()
    authority["query_prefix_proof_sha256"] = prefix_proof_sha256
    authority["execution_role"] = (
        "q1024_singleton" if singleton_authority is not None
        else "q1024_packed" if packed_authority is not None
        else "canonical_topology")
    if singleton_authority is not None:
        authority.update(singleton_authority)
    if packed_authority is not None:
        authority.update(packed_authority)
    authority["ledger_migration_required"] = (
        legacy_empty_revision or not proof_binding_current)
    return authority, None


def _campaign_config_contract_status(
        config_path: str | None) -> tuple[bool, str | None]:
    """Compatibility wrapper for callers that need only campaign selection."""
    authority, error = _algorithm_matrix_campaign_authority_status(config_path)
    return authority is not None, error


def _is_algorithm_matrix_campaign_config(config_path: str | None) -> bool:
    """Select only path+digest-authorized campaign overrides."""
    selected, error = _campaign_config_contract_status(config_path)
    if error is not None:
        raise RuntimeError(error)
    return selected


def _e2e_config_requests_nice_attestation(config_path: str | None) -> bool:
    """Return whether an explicit E2E config opts into exact NI=0 evidence."""
    if config_path is None:
        return False
    open_path = _validated_benchmark_e2e_config_open_path(config_path)
    try:
        payload = json.loads(Path(open_path).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(
            "could not inspect explicit E2E priority-attestation config") from exc
    test_cases = payload.get("test_cases")
    if not isinstance(test_cases, dict) or not test_cases:
        raise RuntimeError(
            "explicit E2E priority-attestation config has no test cases")
    requested = False
    for test_case_name, test_case in test_cases.items():
        if not isinstance(test_case, dict) or not isinstance(
                test_case.get("data"), dict):
            raise RuntimeError(
                f"E2E test case {test_case_name!r} has no data object")
        value = test_case["data"].get("require_nice_attestation", False)
        if type(value) is not bool:
            raise RuntimeError(
                f"E2E test case {test_case_name!r} has a non-boolean "
                "require_nice_attestation")
        requested = requested or value
    return requested


def _ensure_darwin_algorithm_matrix_launch_nice(
        mode: str,
        config_path: str | None,
        log_handle,
) -> None:
    """Establish NI=0 before a canonical or explicitly attested E2E launch.

    This is intentionally narrower than the build-worker scheduling guard. It
    never requests negative niceness and never runs for an ordinary E2E/product
    path unless its data explicitly requires E2E priority attestation.
    """
    if mode != "benchmark-e2e" or config_path is None:
        return
    if host_platform.system() != "Darwin":
        return
    authority, authority_error = _algorithm_matrix_campaign_authority_status(
        config_path)
    if authority_error is not None:
        raise RuntimeError(authority_error)
    canonical_priority_launch = authority is not None and authority.get(
        "name") in {"deadline", "observed_rung", "q1", "q8", "q1024"}
    explicit_attestation_launch = authority is None and (
        _e2e_config_requests_nice_attestation(config_path))
    if not canonical_priority_launch and not explicit_attestation_launch:
        return
    try:
        process_selector = os.PRIO_PROCESS
        os.setpriority(process_selector, 0, 0)
        nice_value = os.getpriority(process_selector, 0)
    except (AttributeError, OSError) as exc:
        raise RuntimeError(
            "Darwin algorithm-matrix launch could not establish worker NI=0") from exc
    if type(nice_value) is not int or nice_value != 0:
        raise RuntimeError(
            "Darwin algorithm-matrix launch requires verified worker NI=0; "
            f"observed={nice_value!r}")
    log_handle.write(
        "# algorithm-matrix Darwin launch priority worker_NI=0 verified; "
        f"authority={'canonical' if canonical_priority_launch else 'explicit_e2e_attestation'}; "
        "exec_children_inherit_worker_nice=true; "
        "postgres_backend_priority_requires_exact_e2e_observation=true\n")
    log_handle.flush()


def _scrub_benchmark_e2e_config_environment(
        environment: dict[str, str]) -> None:
    """Remove ambient config identity/open paths before worker selection."""
    for key in _BENCHMARK_E2E_CONFIG_ENVIRONMENT_KEYS:
        environment.pop(key, None)


def _validated_benchmark_e2e_config_open_path(
        bench_e2e_config: str) -> str:
    """Derive one absolute read path from the exact validated identity."""
    validated_identity, validation_error = _validate_bench_e2e_config_override(
        "benchmark-e2e", bench_e2e_config)
    if validation_error is not None or validated_identity != bench_e2e_config:
        raise RuntimeError(
            "E2E config open path requires the exact validated identity: "
            f"{validation_error or 'identity spelling changed'}")

    campaign_selected, campaign_error = _campaign_config_contract_status(
        validated_identity)
    if campaign_error is not None:
        raise RuntimeError(campaign_error)
    if campaign_selected:
        config_path = _campaign_regular_repository_file(
            validated_identity, "E2E config open path")
    else:
        config_path = Path(validated_identity)
        if not config_path.is_absolute():
            raise RuntimeError(
                "ordinary validated E2E config identity must be absolute")
        try:
            resolved = config_path.resolve(strict=True)
        except (OSError, RuntimeError) as exc:
            raise RuntimeError(
                f"E2E config open path cannot be resolved: {exc}") from exc
        if resolved != config_path:
            raise RuntimeError(
                "ordinary E2E config open path may not retain a path alias")
        if not resolved.is_file() or resolved.suffix.lower() != ".json":
            raise RuntimeError(
                "ordinary E2E config open path must be a regular .json file")
    if not config_path.is_absolute():
        raise RuntimeError("E2E config open path must be absolute")
    return str(config_path)


def _benchmark_e2e_runtime_config_identity(
        bench_e2e_config: str, validated_open_path: str) -> str:
    """Select the C++-recognized identity for an already-authorized config."""
    if any(
            re.fullmatch(str(profile["singleton_pattern"]), bench_e2e_config)
            is not None
            for profile in _ALGORITHM_MATRIX_OBSERVED_RUNG_PROFILES):
        # The current C++ loader's generic absolute-file branch accepts this
        # exact file, while its relative sealed-family branch predates the
        # observed-rung singleton overlays.  The open-path validator above has
        # already proved the manifest authority and canonical repository path.
        return validated_open_path
    return bench_e2e_config


def _validate_benchmark_e2e_runtime_config_environment(
        environment: Mapping[str, str],
        bench_e2e_config: str | None) -> str | None:
    """Prove the emitted identity/open pair came from the validated request."""
    identity_present = _BENCHMARK_E2E_CONFIG_IDENTITY_ENV in environment
    open_path_present = _BENCHMARK_E2E_CONFIG_OPEN_ENV in environment
    if bench_e2e_config is None:
        if identity_present or open_path_present:
            raise RuntimeError(
                "E2E config environment appeared without an explicit override")
        return None
    if not identity_present or not open_path_present:
        raise RuntimeError(
            "explicit E2E config requires both identity and absolute open path")

    observed_identity = environment[_BENCHMARK_E2E_CONFIG_IDENTITY_ENV]
    observed_open_path = environment[_BENCHMARK_E2E_CONFIG_OPEN_ENV]
    expected_open_path = _validated_benchmark_e2e_config_open_path(
        bench_e2e_config)
    expected_identity = _benchmark_e2e_runtime_config_identity(
        bench_e2e_config, expected_open_path)
    if observed_identity != expected_identity:
        raise RuntimeError(
            "E2E config environment identity contradicts the validated request")
    if (not isinstance(observed_open_path, str)
            or not observed_open_path
            or observed_open_path != observed_open_path.strip()
            or any(ord(character) < 0x20 or ord(character) == 0x7F
                   for character in observed_open_path)
            or not Path(observed_open_path).is_absolute()):
        raise RuntimeError("E2E config environment open path is malformed")
    if observed_open_path != expected_open_path:
        raise RuntimeError(
            "E2E config environment open path contradicts the validated identity")
    return expected_open_path


def _benchmark_e2e_runtime_environment(
        environment: dict[str, str],
        active_build_dir: Path,
        active_preset: str,
        bench_e2e_config: str | None,
        postgres_application_name: str | None = None,
        q1024_run_token: str | None = None) -> dict[str, str]:
    """Apply deterministic E2E identity/read authority without changing cwd."""
    restart_command = _postgresql_e2e_restart_shell_command(
        active_build_dir,
        active_preset,
        boundary_mode=(
            "benchmark-e2e"
            if postgres_application_name is not None else None),
        boundary_run_token=q1024_run_token,
        boundary_application_name=postgres_application_name,
    )
    run_environment = environment.copy()
    _scrub_benchmark_e2e_config_environment(run_environment)
    campaign_authority, authority_error = (
        _algorithm_matrix_campaign_authority_status(bench_e2e_config))
    if authority_error is not None:
        raise RuntimeError(authority_error)
    if postgres_application_name is not None:
        if not _is_q1024_execution_authority(campaign_authority):
            raise RuntimeError(
                "PostgreSQL owner labels are reserved for q1024 execution")
        run_environment["PGAPPNAME"] = (
            _validated_q1024_postgres_application_name(
                postgres_application_name))
    if campaign_authority is not None:
        if bench_e2e_config is None:
            raise RuntimeError("canonical campaign lost its config authority")
        run_environment.update(_ALGORITHM_MATRIX_RUNTIME_ENVIRONMENT)
        selected_config = campaign_authority.get("selected_config")
        canonical_cell = (
            selected_config.get("canonical_cell")
            if isinstance(selected_config, dict) else None)
        cell_identity = (
            canonical_cell.get("identity")
            if isinstance(canonical_cell, dict) else None)
        retain_observed_just_index_target = (
            campaign_authority.get("name") == "observed_rung"
            and campaign_authority.get("execution_role")
                == "observed_rung_singleton"
            and isinstance(cell_identity, dict)
            and cell_identity.get("index_mode") == "just_index"
            and cell_identity.get("engine") == "bioseq"
            and canonical_cell.get("algorithm")
                == "bioseq_algorithm_matrix_just_index"
        )
        if retain_observed_just_index_target:
            query_count = campaign_authority.get("query_count")
            if not isinstance(query_count, int) or query_count < 2:
                raise RuntimeError(
                    "observed just-index target retention lost its query rung")
            run_environment[
                "BIOSEQ_E2E_RETAIN_MATRIX_JUST_INDEX_TARGET"] = "1"
            run_environment["BIOSEQ_E2E_SKIP_INDEX_BASELINE_SETUP"] = "1"
        run_environment["BIOSEQ_E2E_PG_RESTART_CMD"] = restart_command
    else:
        # Ordinary E2E behavior remains unchanged: ambient reuse/setup
        # overrides are preserved, and an ambient restart command retains
        # precedence.
        run_environment.setdefault("BIOSEQ_E2E_PG_RESTART_CMD", restart_command)
    if bench_e2e_config is None:
        _validate_benchmark_e2e_runtime_config_environment(
            run_environment, None)
        return run_environment

    config_open_path = _validated_benchmark_e2e_config_open_path(
        bench_e2e_config)
    run_environment[_BENCHMARK_E2E_CONFIG_IDENTITY_ENV] = (
        _benchmark_e2e_runtime_config_identity(
            bench_e2e_config, config_open_path))
    run_environment[_BENCHMARK_E2E_CONFIG_OPEN_ENV] = config_open_path
    _validate_benchmark_e2e_runtime_config_environment(
        run_environment, bench_e2e_config)
    return run_environment


def _merge_algorithm_matrix_all_results(
        *,
        mode: str,
        report_path: Path | None,
        bench_e2e_config: str | None,
        environment: dict[str, str],
        log_handle,
        deadline: float,
        hard_kill_deadline: float | None = None,
        run_receipt_id: str | None = None) -> None:
    """Run the fail-closed raw-report consumer for explicit campaign runs."""
    _require_control_deadline(deadline, "algorithm-matrix consumer setup")
    authority, authority_error = _algorithm_matrix_campaign_authority_status(
        bench_e2e_config)
    _require_control_deadline(deadline, "algorithm-matrix authority validation")
    if authority_error is not None:
        raise RuntimeError(authority_error)
    if mode != "benchmark-e2e" or authority is None:
        return
    if authority.get("name") == "q1":
        log_handle.write(
            "# algorithm-matrix q1 calibration is nonpublication evidence; "
            "Q8_RESULTS.yaml and ALL_RESULTS.yaml merge skipped\n")
        log_handle.flush()
        return
    if authority.get("name") == "deadline":
        log_handle.write(
            "# algorithm-matrix deadline report is an ALL_RESULTS.yaml candidate "
            "only after terminal per-shard audit; automatic run_only merge skipped "
            "and Q8_RESULTS.yaml remains unauthorized\n")
        log_handle.flush()
        return
    if report_path is None or bench_e2e_config is None:
        raise RuntimeError(
            "algorithm-matrix campaign completed without an exact report/config")
    merger = REPOSITORY_ROOT / "bench" / "aggregate_e2e_algorithm_matrix.py"
    try:
        _require_control_deadline(deadline, "algorithm-matrix evidence validation")
        _campaign_internal_regular_file(merger, "algorithm-matrix merger")
        verified_report = _campaign_internal_regular_file(
            report_path, "algorithm-matrix framework report")
        report_relative = verified_report.relative_to(REPOSITORY_ROOT).as_posix()
    except (OSError, ValueError) as exc:
        raise RuntimeError(
            f"algorithm-matrix evidence path is not canonical: {exc}") from exc
    if (verified_report.parent
            != REPOSITORY_ROOT / "reporting/bioseq_benchmark_e2e"
            or re.fullmatch(r"report_[0-9]+\.json", verified_report.name) is None):
        raise RuntimeError(
            "algorithm-matrix framework report must be the exact canonical "
            "reporting/bioseq_benchmark_e2e/report_<N>.json path")

    if authority.get("name") == "observed_rung":
        if (not isinstance(run_receipt_id, str)
                or re.fullmatch(r"rr_[0-9a-f]{32}", run_receipt_id) is None):
            raise RuntimeError(
                "observed-rung validation requires its persisted run receipt id")
        command = [
            sys.executable,
            str(merger),
            "validate-observed-rung-report",
            "--manifest", str(authority["manifest"]),
            "--expected-campaign-digest", str(authority["campaign_digest"]),
            "--query-count", str(authority["query_count"]),
            "--report", report_relative,
            "--config", bench_e2e_config,
            "--run-receipt-id", run_receipt_id,
        ]
        _update_entry(
            mode,
            step=(
                f"validating observed algorithm-matrix q"
                f"{authority['query_count']} rung"),
        )
        log_handle.write(
            "# algorithm-matrix observed-rung validator="
            f"{json.dumps(command, ensure_ascii=True)}\n")
        log_handle.flush()
        rc = _stream_subprocess(
            command,
            log_handle,
            deadline,
            cwd=REPOSITORY_ROOT,
            env=environment,
            hard_kill_deadline=hard_kill_deadline,
            state_mode=mode,
            phase="algorithm_matrix_observed_rung_validation",
        )
        if rc != 0:
            raise RuntimeError(
                f"algorithm-matrix observed q{authority['query_count']} "
                f"validator rejected report {report_path} with exit code {rc}")
        return

    ledger_relative = authority["ledger"]
    all_results = REPOSITORY_ROOT / ledger_relative
    try:
        _campaign_internal_regular_file(
            all_results, f"{authority['name']} algorithm-matrix ledger")
    except (OSError, ValueError) as exc:
        raise RuntimeError(
            f"algorithm-matrix ledger path is not canonical: {exc}") from exc
    command = [
        sys.executable,
        str(merger),
        "--config", bench_e2e_config,
        "--report", report_relative,
        "--all-results", ledger_relative,
    ]
    if authority.get("execution_role") in {"q1024_singleton", "q1024_packed"}:
        if (not isinstance(run_receipt_id, str)
                or re.fullmatch(r"rr_[0-9a-f]{32}", run_receipt_id) is None):
            raise RuntimeError(
                "q1024 hybrid merge requires its persisted run receipt id")
        command.extend(["--run-receipt-id", run_receipt_id])
    elif run_receipt_id is not None:
        raise RuntimeError(
            "run receipt merge argument is reserved for q1024 execution members")
    _update_entry(mode, step="merging validated algorithm-matrix results")
    log_handle.write(
        f"# algorithm-matrix {ledger_relative} consumer="
        f"{json.dumps(command, ensure_ascii=True)}\n")
    log_handle.flush()
    rc = _stream_subprocess(
        command,
        log_handle,
        deadline,
        cwd=REPOSITORY_ROOT,
        env=environment,
        hard_kill_deadline=hard_kill_deadline,
        state_mode=mode,
        phase="algorithm_matrix_result_merge",
    )
    if rc != 0:
        raise RuntimeError(
            f"algorithm-matrix {ledger_relative} consumer rejected report "
            f"{report_path} with exit code {rc}")

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
        *,
        hard_kill_deadline: float | None = None,
        state_mode: str | None = None,
        phase: str | None = None,
        service_boundary_provider=None,
        initial_service_boundary_provider=None,
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
    runtime_phase = phase or f"{mode}_runtime"

    for command in mode_details.commands:
        _require_control_deadline(deadline, f"{mode} runtime command setup")
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
                    hard_kill_deadline=hard_kill_deadline,
                    state_mode=state_mode,
                    phase=runtime_phase,
                    require_verified_termination=(
                        hard_kill_deadline is not None),
                    service_boundary_provider=service_boundary_provider,
                    initial_service_boundary_provider=(
                        initial_service_boundary_provider),
                )
        else:
            try:
                rc = _stream_subprocess(
                    full_command,
                    log_handle,
                    command_deadline,
                    cwd=active_build_dir,
                    env=run_env,
                    hard_kill_deadline=hard_kill_deadline,
                    state_mode=state_mode,
                    phase=runtime_phase,
                    require_verified_termination=(
                        hard_kill_deadline is not None),
                    service_boundary_provider=service_boundary_provider,
                    initial_service_boundary_provider=(
                        initial_service_boundary_provider),
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
            _require_control_deadline(
                deadline, f"{mode} runtime report validation")
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
            if service_boundary_provider is not None:
                raise RuntimeError(
                    "q1024 guarded client failed with exit code "
                    f"{rc}: {' '.join(command)}")
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
        _require_control_deadline(deadline, f"{mode} runtime receipt finalization")
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
    # Lint modes produce analysis reports rather than reusable runtime trees.
    return (
        build_only
        and mode != POSTGRES_ERROR_SAFETY_MODE
        and mode not in LINT_MODES
    )


def _cleanup_armed_q1024_postgresql_owner(
        *,
        mode: str,
        active_build_dir: Path,
        active_preset: str,
        postgres_application_name: str,
        log_handle,
        cleanup_deadline: float,
        run_token: str | None = None) -> bool:
    """Disarm one exact owner only after PostgreSQL proves it absent."""
    claim_token: str | None = None
    if run_token is not None:
        try:
            claim = _claim_q1024_postgresql_cleanup(
                mode, run_token, actor="worker")
        except RuntimeError as exc:
            claim = None
            _update_entry(
                mode,
                postgres_cleanup_unverified=True,
                cleanup_unverified=True,
                postgres_cleanup_evidence={
                    "cleanup_verified": False,
                    "error": str(exc),
                },
            )
        if claim is None:
            return False
        claim_token, current = claim
    else:
        # Compatibility seam for the existing focused helper tests. Production
        # q1024 callers always provide the reserved run token and take a claim.
        current = _load_state().get(mode)
    identity = (
        current.get("postgres_backend_identity")
        if isinstance(current, dict) else None)
    if (not isinstance(current, dict)
            or current.get("postgres_backend_owner_started") is not True
            or not isinstance(identity, dict)
            or identity.get("application_name") != postgres_application_name
            or not isinstance(identity.get("database"), str)
            or not isinstance(identity.get("user"), str)):
        evidence = {
            "cleanup_verified": False,
            "error": "armed q1024 PostgreSQL owner scope is missing or contradictory",
        }
    else:
        try:
            evidence = _cleanup_owned_postgresql_backends(
                active_build_dir,
                active_preset,
                application_name=postgres_application_name,
                database_name=identity["database"],
                database_user=identity["user"],
                log_handle=log_handle,
                deadline=cleanup_deadline,
            )
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            evidence = {"cleanup_verified": False, "error": str(exc)}
    verified = bool(evidence.get("cleanup_verified"))
    termination_evidence = (
        current.get("termination_evidence")
        if isinstance(current, dict) else None)
    process_cleanup_unverified = bool(
        isinstance(current, dict)
        and (
            current.get("cleanup_unverified_before_postgres_owner")
            or current.get("process_cleanup_unverified")
        ))
    if termination_evidence is not None:
        process_cleanup_unverified = (
            process_cleanup_unverified
            or not isinstance(termination_evidence, dict)
            or termination_evidence.get("cleanup_verified") is not True
            or any(
                not isinstance(termination_evidence.get(field, []), list)
                or bool(termination_evidence.get(field, []))
                for field in (
                    "survivor_pids",
                    "marker_survivor_pids",
                    "survivor_identities",
                )
            )
        )
    if claim_token is not None and run_token is not None:
        return _finish_q1024_postgresql_cleanup(
            mode, run_token, claim_token, evidence)
    _update_entry(
        mode,
        postgres_backend_owner_started=not verified,
        postgres_cleanup_evidence=evidence,
        postgres_cleanup_unverified=not verified,
        cleanup_unverified=(not verified or process_cleanup_unverified),
    )
    return verified

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
        hard_kill_deadline: float | None = None,
        cleanup_deadline: float | None = None,
        postgres_application_name: str | None = None,
        q1024_run_token: str | None = None,
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
    _require_control_deadline(deadline, "run-only setup")
    _ensure_darwin_algorithm_matrix_launch_nice(
        mode, bench_e2e_config, log_handle)
    env = _build_worker_descendant_environment()
    _scrub_benchmark_e2e_config_environment(env)
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
    _require_control_deadline(deadline, "run-only report setup")

    if mode_details.install_extension:
        install_receipt, install_receipt_error = _validate_build_receipt(
            str(receipt["build_receipt_id"]),
            expected_mode=mode,
            expected_debug=bool(receipt["debug"]),
            deadline=deadline,
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
            hard_kill_deadline=hard_kill_deadline,
            state_mode=mode,
            phase="run_only_install",
        )
        if rc != 0:
            raise RuntimeError(
                "receipt-pinned extension install exited with code "
                f"{rc}")
        _restart_postgresql(
            active_build_dir,
            active_preset,
            log_handle,
            deadline,
            hard_kill_deadline=hard_kill_deadline,
            state_mode=mode,
            phase="run_only_postgresql_restart",
        )
        _prepare_postgresql_test_database(
            active_build_dir,
            active_preset,
            log_handle,
            deadline,
            env=env,
            hard_kill_deadline=hard_kill_deadline,
            state_mode=mode,
            phase="run_only_postgresql_database_prepare",
        )

    _update_entry(mode, step="running from build receipt")
    run_env = env
    if mode == "benchmark-e2e":
        _require_control_deadline(deadline, "benchmark E2E environment setup")
        run_env = _benchmark_e2e_runtime_environment(
            env,
            active_build_dir,
            active_preset,
            bench_e2e_config,
            postgres_application_name=postgres_application_name,
            q1024_run_token=q1024_run_token,
        )
        _require_control_deadline(deadline, "benchmark E2E environment setup")
        if postgres_application_name is not None:
            if cleanup_deadline is None:
                raise RuntimeError("q1024 execution has no cleanup deadline")
            current_cleanup_state = _load_state().get(mode)
            _update_entry(
                mode,
                postgres_backend_owner_started=True,
                postgres_backend_identity={
                    "application_name": postgres_application_name,
                    "database": run_env.get("BIOSEQ_TEST_PGDATABASE", "bioseq"),
                    "user": run_env.get("BIOSEQ_TEST_PGUSER", "postgres"),
                },
                postgres_cleanup_evidence={
                    "cleanup_verified": False,
                    "reason": "armed before E2E client launch",
                },
                postgres_cleanup_unverified=True,
                cleanup_unverified_before_postgres_owner=bool(
                    isinstance(current_cleanup_state, dict)
                    and current_cleanup_state.get("cleanup_unverified")),
                cleanup_unverified=True,
            )
    initial_service_boundary_provider = None
    service_boundary_provider = None
    if postgres_application_name is not None:
        if not isinstance(q1024_run_token, str):
            raise RuntimeError(
                "q1024 service boundary has no reserved run token")
        armed_entry = _load_state().get(mode)
        if not isinstance(armed_entry, dict):
            raise RuntimeError(
                "q1024 service boundary lost its armed state")
        if armed_entry.get("postgres_service_boundary") is not None:
            raise RuntimeError(
                "q1024 pre-arm state retained a stale service boundary")
        initial_service_boundary_provider = (
            _postgresql_recovery_service_boundary_provider(armed_entry))
        if initial_service_boundary_provider is None:
            raise RuntimeError(
                "q1024 service boundary has no pre-arm classifier")
        service_boundary_provider = (
            _postgresql_guard_service_boundary_provider(
                mode=mode,
                run_token=q1024_run_token,
                application_name=postgres_application_name,
                active_build_dir=active_build_dir,
                active_preset=active_preset,
                log_handle=log_handle,
            ))
    cleanup_verified = True
    postgres_cleanup_attempted = False
    try:
        report_path, report_files, _, _ = _run_mode_commands(
            mode,
            active_bin_dir,
            active_build_dir,
            run_env,
            log_handle,
            deadline,
            pre_reports,
            hard_kill_deadline=hard_kill_deadline,
            state_mode=mode,
            phase="run_only_runtime",
            service_boundary_provider=service_boundary_provider,
            initial_service_boundary_provider=(
                initial_service_boundary_provider),
            bench_flags=bench_flags,
            test_flags=test_flags,
        )
        if postgres_application_name is not None:
            postgres_cleanup_attempted = True
            cleanup_verified = _cleanup_armed_q1024_postgresql_owner(
                mode=mode,
                active_build_dir=active_build_dir,
                active_preset=active_preset,
                postgres_application_name=postgres_application_name,
                log_handle=log_handle,
                cleanup_deadline=cleanup_deadline,
                run_token=q1024_run_token,
            )
            if not cleanup_verified:
                raise RuntimeError(
                    "q1024 PostgreSQL owner cleanup did not prove zero matches")
        revalidated_receipt, receipt_error = _validate_build_receipt(
            str(receipt["build_receipt_id"]),
            expected_mode=mode,
            expected_debug=bool(receipt["debug"]),
            deadline=deadline,
        )
        if receipt_error is not None or revalidated_receipt is None:
            raise RuntimeError(
                "build receipt changed during run_only execution: "
                f"{receipt_error}")
        _require_control_deadline(deadline, "campaign authority validation")
        authority, authority_error = _algorithm_matrix_campaign_authority_status(
            bench_e2e_config)
        _require_control_deadline(deadline, "campaign authority validation")
        if authority_error is not None:
            raise RuntimeError(authority_error)
        receipt_before_consumer_flow = (
            authority is not None
            and (authority.get("name") == "observed_rung"
                 or authority.get("execution_role") in {
                    "deadline_singleton", "q1024_singleton", "q1024_packed"}))
        if receipt_before_consumer_flow:
            # This immutable receipt attests completed client execution and
            # exact raw-report bytes. The independent consumer loads it by id
            # to decide result approval; consumer failure never invalidates
            # those already-completed execution facts.
            run_receipt = _seal_run_receipt(
                mode=mode,
                build_receipt=revalidated_receipt,
                report_files=report_files,
                bench_flags=bench_flags,
                test_flags=test_flags,
                bench_e2e_config=bench_e2e_config,
                deadline=deadline,
            )
            run_receipt_id = str(run_receipt["build_receipt_id"])
            _update_entry(
                mode,
                step=(
                    "run receipt sealed; validating observed rung"
                    if authority.get("name") == "observed_rung"
                    else "run receipt sealed; ingesting sealed execution member"),
                run_receipt_id=run_receipt_id,
            )
            _merge_algorithm_matrix_all_results(
                mode=mode,
                report_path=report_path,
                bench_e2e_config=bench_e2e_config,
                environment=run_env,
                log_handle=log_handle,
                deadline=deadline,
                hard_kill_deadline=hard_kill_deadline,
                run_receipt_id=run_receipt_id,
            )
        else:
            _merge_algorithm_matrix_all_results(
                mode=mode,
                report_path=report_path,
                bench_e2e_config=bench_e2e_config,
                environment=run_env,
                log_handle=log_handle,
                deadline=deadline,
                hard_kill_deadline=hard_kill_deadline,
            )
            run_receipt = _seal_run_receipt(
                mode=mode,
                build_receipt=revalidated_receipt,
                report_files=report_files,
                bench_flags=bench_flags,
                test_flags=test_flags,
                bench_e2e_config=bench_e2e_config,
                deadline=deadline,
            )
    finally:
        if (postgres_application_name is not None
                and not postgres_cleanup_attempted):
            postgres_cleanup_attempted = True
            cleanup_verified = _cleanup_armed_q1024_postgresql_owner(
                mode=mode,
                active_build_dir=active_build_dir,
                active_preset=active_preset,
                postgres_application_name=postgres_application_name,
                log_handle=log_handle,
                cleanup_deadline=cleanup_deadline,
                run_token=q1024_run_token,
            )
    if not cleanup_verified:
        raise RuntimeError(
            "q1024 PostgreSQL owner cleanup did not prove zero matches")
    _require_control_deadline(
        deadline,
        "run-only terminal state publication",
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


def _algorithm_matrix_authority_query_count(
        authority: Mapping[str, object] | None) -> int | None:
    """Read the executed query count from any canonical authority shape."""
    if not isinstance(authority, Mapping):
        return None
    direct = authority.get("query_count")
    if type(direct) is int:
        return direct
    selected_shard = authority.get("selected_shard")
    identity = (
        selected_shard.get("identity")
        if isinstance(selected_shard, Mapping) else None)
    nested = identity.get("query_count") if isinstance(identity, Mapping) else None
    if type(nested) is int:
        return nested
    name = authority.get("name")
    match = re.fullmatch(r"q([1-9][0-9]*)", name) \
        if isinstance(name, str) else None
    return int(match.group(1)) if match is not None else None


def _is_q1024_execution_authority(
        authority: Mapping[str, object] | None) -> bool:
    """Recognize executed q1024 work without config-path/ordinal coupling."""
    return (
        _algorithm_matrix_authority_query_count(authority) == 1024
        and authority.get("execution_role") in {
            "deadline_singleton",
            "observed_rung_singleton",
            "q1024_packed",
            "q1024_singleton",
        }
    ) if isinstance(authority, Mapping) else False


def _q1024_nonreceipt_execution_forbidden(
        authority: Mapping[str, object] | None,
        *, build_only: bool, run_only: bool) -> bool:
    """Reject q1024 command execution unless it is receipt-pinned run-only."""
    return (
        _is_q1024_execution_authority(authority)
        and not run_only
        and not build_only
    )


def _q1024_postgres_application_name(run_token: str) -> str:
    """Derive one opaque libpq backend owner from the reserved run token."""
    if re.fullmatch(r"[0-9a-f]{32}", run_token) is None:
        raise RuntimeError("q1024 PostgreSQL owner token is malformed")
    return _Q1024_POSTGRES_APPLICATION_NAME_PREFIX + run_token


def _validated_q1024_postgres_application_name(value: object) -> str:
    if (not isinstance(value, str)
            or re.fullmatch(
                re.escape(_Q1024_POSTGRES_APPLICATION_NAME_PREFIX)
                + r"[0-9a-f]{32}", value) is None
            or len(value.encode("utf-8")) > 63):
        raise RuntimeError("reserved q1024 PostgreSQL owner is malformed")
    return value


def _run_only_runtime_timeout_seconds(
        mode: str,
        bench_e2e_config: str | None,
        default_seconds: float,
        *,
        started_at: float | None = None) -> float:
    """Resolve the whole invocation allowance for one run-only selection."""
    if mode != "benchmark-e2e" or bench_e2e_config is None:
        return default_seconds
    authority, error = _algorithm_matrix_campaign_authority_status(
        bench_e2e_config)
    if error is not None:
        raise RuntimeError(error)
    if authority is None:
        return default_seconds
    resolved_timeout = default_seconds
    if authority.get("name") != "observed_rung":
        timeout_authority = authority
        if authority.get("name") != "deadline":
            timeout_authority, error = (
                _q1024_extended_timeout_authority(
                    bench_e2e_config, authority))
            if error is not None:
                raise RuntimeError(error)
        if timeout_authority is not None:
            timeout = timeout_authority.get("runtime_timeout_seconds")
            if timeout not in {
                    _ALGORITHM_MATRIX_DEADLINE_NORMAL_TIMEOUT_SECONDS,
                    _ALGORITHM_MATRIX_DEADLINE_EXTENDED_TIMEOUT_SECONDS}:
                raise RuntimeError(
                    "deadline campaign timeout escaped its closed allowlist")
            resolved_timeout = max(default_seconds, float(timeout))
            hard_stop_epoch = timeout_authority.get("hard_stop_epoch")
            if hard_stop_epoch is not None:
                if type(hard_stop_epoch) is not int:
                    raise RuntimeError("q1024 hard-stop epoch is malformed")
                timeout_origin = (
                    time.time() if started_at is None else started_at)
                remaining_seconds = (
                    float(hard_stop_epoch) - timeout_origin - 1.0)
                if remaining_seconds <= 0.0:
                    raise RuntimeError(
                        "q1024 extended-timeout authority has expired")
                resolved_timeout = min(resolved_timeout, remaining_seconds)
    if _is_q1024_execution_authority(authority):
        resolved_timeout = min(
            resolved_timeout, _Q1024_WHOLE_INVOCATION_TIMEOUT_SECONDS)
    return resolved_timeout

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
                      test_flags=None, lint_paths=None,
                      bench_e2e_config=None,
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

    `lint_paths` contains exact normalized compile units and is accepted only
    by the focused lint mode.

    `bench_e2e_config` is server-validated inside this repository and is
    exposed to the benchmark-e2e executable. Ordinary overrides retain their
    historical canonical absolute form; sealed campaign members retain their
    exact contract-listed repository-relative form. The worker derives a
    separate absolute file-open path from that validated identity.
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
    lint_paths = lint_paths or []
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

    # Revalidate and canonicalize both sides of the detached handoff.  A live
    # MCP server may have reserved an exact absolute spelling for a sealed
    # campaign member; the worker and every downstream receipt must use the
    # manifest-owned repository-relative identity instead.
    canonical_worker_config, worker_config_error = (
        _validate_bench_e2e_config_override(mode, bench_e2e_config))
    canonical_reserved_config, reserved_config_error = (
        _validate_bench_e2e_config_override(
            mode, entry.get("bench_e2e_config")))
    if worker_config_error is not None or reserved_config_error is not None:
        _log(
            "detached worker rejected E2E config path: "
            f"{worker_config_error or reserved_config_error}")
        return 1
    if canonical_worker_config != canonical_reserved_config:
        _log(
            "detached worker config differs from the reserved request after "
            "canonicalization")
        return 1
    original_reserved_config = entry.get("bench_e2e_config")
    bench_e2e_config = canonical_worker_config
    entry = dict(entry)
    entry["bench_e2e_config"] = canonical_reserved_config
    reserved_arguments = {
        "build_only": build_only,
        "debug": debug,
        "fresh": fresh,
        "run_only": run_only,
        "build_timeout_sec": build_timeout_sec,
        "reuse_build_receipt": reuse_build_receipt,
        "lint_paths": lint_paths,
        "bench_flags": bench_flags,
        "focused_test_args": test_flags,
        "bench_e2e_config": bench_e2e_config,
    }
    for field_name, observed_value in reserved_arguments.items():
        if field_name in entry and entry.get(field_name) != observed_value:
            _log(
                "detached worker arguments differ from the reserved request: "
                f"{field_name} scheduled={entry.get(field_name)!r} "
                f"worker={observed_value!r}")
            return 1
    if original_reserved_config != canonical_reserved_config:
        _update_entry(
            mode, bench_e2e_config=canonical_reserved_config)
    mode_details = MODES[mode]
    campaign_authority, campaign_error = _algorithm_matrix_campaign_authority_status(
        bench_e2e_config)
    if campaign_error is not None:
        _log(f"detached worker rejected campaign authority: {campaign_error}")
        return 1
    campaign_selected = campaign_authority is not None
    q1024_execution = (
        run_only and _is_q1024_execution_authority(campaign_authority))
    if _q1024_nonreceipt_execution_forbidden(
            campaign_authority, build_only=build_only, run_only=run_only):
        _log(
            "q1024 execution requires run_only=true and a current "
            "debug=false bench-e2e-opt build receipt")
        return 1
    try:
        postgres_application_name = (
            _validated_q1024_postgres_application_name(
                entry.get("postgres_application_name"))
            if q1024_execution else None)
    except RuntimeError as exc:
        _log(f"detached worker rejected PostgreSQL owner: {exc}")
        return 1
    if (not q1024_execution
            and entry.get("postgres_application_name") is not None):
        _log("detached worker rejected an out-of-scope PostgreSQL owner")
        return 1
    q1024_run_token = entry.get("q1024_run_token")
    if q1024_execution:
        if (not isinstance(q1024_run_token, str)
                or re.fullmatch(r"[0-9a-f]{32}", q1024_run_token) is None
                or _q1024_postgres_application_name(q1024_run_token)
                    != postgres_application_name):
            _log("detached worker rejected q1024 watchdog ownership")
            return 1
        try:
            entry = _await_q1024_watchdog_arm(mode, q1024_run_token)
        except RuntimeError as exc:
            _log(f"detached worker q1024 watchdog arm failed: {exc}")
            _update_entry(
                mode,
                status="failed",
                step="q1024_watchdog_arm_failed",
                completed_at=time.time(),
                notes=str(exc),
                cleanup_unverified=True,
            )
            return 1
    elif q1024_run_token is not None:
        _log("detached worker rejected an out-of-scope q1024 watchdog token")
        return 1
    if (campaign_authority is not None
            and campaign_authority.get("name") == "observed_rung"
            and not build_only and not run_only):
        _log(
            "observed-rung execution requires run_only=true and a current "
            "debug=false bench-e2e-opt build receipt")
        return 1
    allowed_campaign_flags = (
        _ALGORITHM_MATRIX_EXACT_BENCH_FLAGS,
        [*_ALGORITHM_MATRIX_EXACT_BENCH_FLAGS, "--debug"],
    )
    if campaign_selected and bench_flags not in allowed_campaign_flags:
        _log(
            "detached worker requires the exact benchmark=bench_e2e selection "
            "for canonical algorithm-matrix campaign execution")
        return 1
    if mode == LINT_FOCUSED_MODE:
        normalized_lint_paths, lint_error = lint_filter_to_paths(
            {"paths": lint_paths}
        )
        if lint_error is not None or normalized_lint_paths != lint_paths:
            _log(
                f"{mode} worker received invalid lint paths: "
                f"{lint_error or 'paths are not normalized and unique'}"
            )
            return 1
    elif mode == LINT_FULL_MODE:
        if lint_paths:
            _log(f"{mode} worker does not accept focused lint paths")
            return 1
    elif lint_paths:
        _log(f"{mode} worker does not accept lint paths")
        return 1
    if mode == ALL_ALPHABETS_UNIT_INTEGRATION_MODE and debug:
        _log(f"{mode} uses a fixed optimized preset and rejects debug=true")
        return 1
    if mode_details.requires_build_only and (
        not build_only
        or run_only
        or (mode in LINT_MODES and build_timeout_sec is not None)
        or debug
        or bench_flags
        or test_flags
        or bench_e2e_config
    ):
        _log(
            f"{mode} worker requires build_only=true and rejects every "
            "unsupported runtime-selection input")
        return 1
    if mode == PACKAGE_LINUX_MODE:
        if (
            build_only or run_only or fresh or build_timeout_sec is not None
            or reuse_build_receipt is not None
            or debug or bench_flags or test_flags
            or lint_paths or bench_e2e_config
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
    if run_only:
        mode_timeout_sec = _run_only_runtime_timeout_seconds(
            mode,
            bench_e2e_config,
            mode_timeout_sec,
            started_at=started_at,
        )
    whole_deadline = started_at + mode_timeout_sec
    q1024_whole_deadline = (
        whole_deadline
        if q1024_execution else None)
    deadline = (
        q1024_whole_deadline
        - _Q1024_CLEANUP_RESERVE_SECONDS
        - BUILD_TIMEOUT_TERM_GRACE_SEC
        if q1024_whole_deadline is not None else whole_deadline)
    if q1024_whole_deadline is not None and deadline <= started_at:
        _log("q1024 authority leaves no bounded execution window")
        return 1
    subprocess_hard_kill_deadline = (
        q1024_whole_deadline - _Q1024_CLEANUP_RESERVE_SECONDS
        if q1024_whole_deadline is not None else None)
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
        f"# lint_paths={json.dumps(lint_paths, ensure_ascii=True)}\n"
        f"# bench_e2e_config={bench_e2e_config or ''}\n"
        f"# clang_rt_priority={entry.get('rt_priority')}\n"
        f"# hard_timeout_min={mode_timeout_sec / 60.0}\n"
        f"# q1024_whole_deadline={q1024_whole_deadline or ''}\n"
        f"# q1024_work_deadline={deadline if q1024_execution else ''}\n"
        "# q1024_process_kill_deadline="
        f"{subprocess_hard_kill_deadline or ''}\n"
        "# q1024_cleanup_reserve_seconds="
        f"{_Q1024_CLEANUP_RESERVE_SECONDS if q1024_execution else ''}\n"
        f"# postgres_application_name={postgres_application_name or ''}\n"
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
                deadline=deadline,
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
                hard_kill_deadline=subprocess_hard_kill_deadline,
                cleanup_deadline=q1024_whole_deadline,
                postgres_application_name=postgres_application_name,
                q1024_run_token=(
                    q1024_run_token if isinstance(q1024_run_token, str)
                    else None),
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
        _scrub_benchmark_e2e_config_environment(env)
        env["CMAKE_EXPORT_COMPILE_COMMANDS"] = "1"
        # The local MCP may itself run on the provisioned Linux build host.
        # Keep the provisioned environment when present, but also recover
        # VCPKG_ROOT from an existing configured tree for a fresh sibling
        # test/debug tree.
        _ensure_local_toolchain_environment(env, active_build_dir)
        if mode == "website":
            _update_entry(
                mode,
                step="provisioning exact Doxygen 1.9.8",
                build_only=build_only,
                debug=debug,
                preset=active_preset,
                build_dir=str(active_build_dir),
            )
        managed_cache_variables, managed_toolchain_identity = (
            _prepare_mode_toolchain(
                mode,
                environment=env,
                log_handle=log_handle,
            )
        )
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
        required_cache = dict(mode_details.required_cache_variables or {})
        overlapping_cache_variables = (
            set(required_cache) & set(managed_cache_variables)
        )
        if overlapping_cache_variables:
            raise RuntimeError(
                "managed mode toolchain cache variables overlap the static "
                "mode contract: "
                f"{sorted(overlapping_cache_variables)}"
            )
        required_cache.update(managed_cache_variables)
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
            managed_toolchain_identity=managed_toolchain_identity,
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
        final_managed_toolchain_identity = managed_toolchain_identity
        if mode == "website":
            (
                final_managed_cache_variables,
                final_managed_toolchain_identity,
            ) = _prepare_mode_toolchain(
                mode,
                environment=env,
                log_handle=log_handle,
            )
            if final_managed_cache_variables != managed_cache_variables:
                raise RuntimeError(
                    "website Doxygen cache selection changed during configure"
                )
        final_configure_stamp = _cmake_configure_stamp_document(
            active_preset,
            mode_cache_variables=configure_cache_contract,
            managed_arguments=initial_clang_rt_args,
            environment=env,
            cmake_executable_identity=_cmake_executable_identity(env),
            managed_toolchain_identity=final_managed_toolchain_identity,
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

        lint_report_files: list[Path] = []
        lint_summary: dict[str, object] | None = None
        if mode in LINT_MODES:
            lint_report_path = _clang_tidy_report_path(active_build_dir, mode)
            lint_report_path.unlink(missing_ok=True)
            _update_entry(
                mode,
                step=f"running clang-tidy ({_clang_tidy_scope(mode)})",
                lint_paths=list(lint_paths),
            )
            lint_command = _clang_tidy_command(
                active_build_dir,
                mode,
                list(lint_paths),
            )
            rc = _stream_subprocess(
                lint_command,
                log_handle,
                build_soft_deadline,
                cwd=REPOSITORY_ROOT,
                env=env,
                hard_kill_deadline=build_hard_deadline,
                state_mode=mode,
                phase=f"clang-tidy:{_clang_tidy_scope(mode)}",
                require_verified_termination=guarded_build,
                deadline_clock=build_deadline_clock,
            )
            if lint_report_path.is_file():
                lint_report_files = [lint_report_path]
                lint_summary = _clang_tidy_report_summary(
                    lint_report_path,
                    expected_scope=_clang_tidy_scope(mode),
                )
                _update_entry(
                    mode,
                    report_files=[str(lint_report_path)],
                    clang_tidy=lint_summary,
                )
            if rc != 0:
                raise RuntimeError(
                    f"clang-tidy {_clang_tidy_scope(mode)} analysis exited "
                    f"with code {rc}; report={lint_report_path}"
                )

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
                report_files=[str(path) for path in lint_report_files],
                build_only=True,
                debug=debug,
                clang_tidy=lint_summary,
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
            run_env = env
            if mode == "highway-gcc-canary":
                run_env = HIGHWAY_GCC_CANARY_RUNTIME_ENVIRONMENT.copy()
                log_handle.write(
                    "# highway-gcc-canary runtime environment="
                    "PATH,LC_ALL,LANG\n")
                log_handle.flush()
            elif mode == "benchmark-e2e":
                run_env = _benchmark_e2e_runtime_environment(
                    env, active_build_dir, active_preset, bench_e2e_config)
                log_handle.write(
                    "# BIOSEQ_E2E_PG_RESTART_CMD="
                    f"{run_env['BIOSEQ_E2E_PG_RESTART_CMD']}\n")
                if _is_algorithm_matrix_campaign_config(bench_e2e_config):
                    log_handle.write(
                        "# canonical algorithm-matrix E2E environment="
                        + json.dumps({
                            key: run_env[key]
                            for key in (
                                *_ALGORITHM_MATRIX_RUNTIME_ENVIRONMENT,
                                _BENCHMARK_E2E_CONFIG_IDENTITY_ENV,
                                _BENCHMARK_E2E_CONFIG_OPEN_ENV,
                                "BIOSEQ_E2E_PG_RESTART_CMD",
                            )
                        }, ensure_ascii=True, sort_keys=True)
                        + "\n")
                elif bench_e2e_config is not None:
                    log_handle.write(
                        "# BIOSEQ_E2E_CONFIG_OVERRIDE="
                        f"{bench_e2e_config}\n")
                    log_handle.write(
                        "# BIOSEQ_E2E_CONFIG_OPEN_PATH="
                        f"{run_env[_BENCHMARK_E2E_CONFIG_OPEN_ENV]}\n")
                log_handle.flush()
            _ensure_darwin_algorithm_matrix_launch_nice(
                mode, bench_e2e_config, log_handle)
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
                state_mode=mode,
                phase="runtime",
                bench_flags=bench_flags,
                test_flags=test_flags,
            )
            _merge_algorithm_matrix_all_results(
                mode=mode,
                report_path=report_path,
                bench_e2e_config=bench_e2e_config,
                environment=run_env,
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
        timeout_update: dict[str, object] = {
            "status": "killed_timeout",
            "step": "killed_timeout",
            "completed_at": completed_at,
            "exit_code": None,
            "notes": str(e),
            "build_only": build_only,
            "debug": debug,
        }
        _update_entry(
            mode,
            **timeout_update,
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

def _revalidate_legacy_q1024_postgresql_cleanup_before_admission(
        *, request_deadline: float | None = None) -> bool:
    """Clear only terminal legacy PG uncertainty proven by a global zero.

    This path is deliberately outside ordinary run receipt acceptance.  It
    validates the historical build authority without requiring today's Python
    source fingerprint, performs no SQL, restart, or signal operation, and
    leaves the original terminal/result facts untouched.
    """
    started_at = time.time()
    deadline = started_at + 15.0
    if request_deadline is not None:
        deadline = min(deadline, request_deadline)
    state = _load_state()
    entry = state.get("benchmark-e2e")
    if not isinstance(entry, dict):
        return False
    run_token = entry.get("q1024_run_token")
    application_name = entry.get("postgres_application_name")
    backend_identity = entry.get("postgres_backend_identity")
    if (
        entry.get("mode") != "benchmark-e2e"
        or entry.get("status") not in {
            "completed", "failed", "aborted", "killed_timeout"}
        or not isinstance(run_token, str)
        or re.fullmatch(r"[0-9a-f]{32}", run_token) is None
        or application_name != f"bioseq-q1024-{run_token}"
        or entry.get("postgres_backend_owner_started") is not True
        or entry.get("postgres_cleanup_unverified") is not True
        or entry.get("postgres_service_boundary") is not None
        or not isinstance(backend_identity, dict)
        or backend_identity.get("application_name") != application_name
        or not isinstance(backend_identity.get("database"), str)
        or not isinstance(backend_identity.get("user"), str)
    ):
        return False
    try:
        authority, authority_error = (
            _algorithm_matrix_campaign_authority_status(
                entry.get("bench_e2e_config")))
    except (OSError, RuntimeError, ValueError, TypeError):
        return False
    if authority_error is not None or not _is_q1024_execution_authority(
            authority):
        return False
    receipt_id = entry.get("reuse_build_receipt")
    if not isinstance(receipt_id, str) or not receipt_id:
        return False
    try:
        receipt, receipt_error = _validate_build_receipt(
            receipt_id,
            expected_mode="benchmark-e2e",
            expected_debug=False,
            verify_source=False,
            deadline=deadline,
        )
    except (OSError, RuntimeError, TimeoutError):
        return False
    if receipt_error is not None or receipt is None:
        return False
    try:
        active_build_dir = Path(str(receipt["build_dir"])).resolve(
            strict=True)
        recorded_build_dir = Path(str(entry.get("build_dir"))).resolve(
            strict=True)
    except (OSError, RuntimeError, ValueError):
        return False
    active_preset = receipt.get("preset")
    if (
        receipt.get("build_receipt_id") != receipt_id
        or receipt.get("mode") != "benchmark-e2e"
        or receipt.get("debug") is not False
        or active_preset != "bench-e2e-opt"
        or receipt.get("preset_family") != "e2e"
        or receipt.get("binary_subdir") != "bioseq"
        or active_build_dir != recorded_build_dir
        or entry.get("preset") != active_preset
        or entry.get("preset_family") != receipt.get("preset_family")
    ):
        return False
    try:
        claim = _claim_q1024_postgresql_revalidation(
            "benchmark-e2e", run_token, deadline=deadline)
    except (OSError, RuntimeError, TimeoutError):
        return False
    if claim is None:
        return False
    claim_token, claimed_entry = claim
    evidence: dict[str, object]
    try:
        if any(
            claimed_entry.get(field) != entry.get(field)
            for field in (
                "status", "exit_code", "q1024_run_token",
                "postgres_application_name", "postgres_backend_identity",
                "build_dir", "preset", "preset_family",
                "reuse_build_receipt", "worker_process_identity",
                "active_process_identity",
                "q1024_watchdog_process_identity",
            )
        ):
            evidence = {
                "cleanup_verified": False,
                "error": "terminal q1024 state changed before global proof",
            }
        else:
            evidence = _legacy_q1024_global_postgresql_zero_evidence(
                entry=claimed_entry,
                active_build_dir=active_build_dir,
                active_preset=str(active_preset),
                deadline=deadline,
            )
    except (OSError, RuntimeError, TimeoutError) as exc:
        evidence = {
            "cleanup_verified": False,
            "error": f"{type(exc).__name__}: {exc}",
        }
    evidence.update({
        "revalidation_started_at": started_at,
        "revalidation_deadline": deadline,
        "historical_build_receipt_id": receipt_id,
        "historical_build_receipt_content_sha256": receipt.get(
            "content_sha256"),
        "source_fingerprint_policy": (
            "terminal-cleanup-only; current source fingerprint intentionally "
            "not accepted for new execution"),
        "preserved_terminal_status": claimed_entry.get("status"),
        "preserved_terminal_exit_code": claimed_entry.get("exit_code"),
    })
    return _finish_q1024_postgresql_revalidation(
        "benchmark-e2e", run_token, claim_token, evidence)


def _unverified_guard_cleanup(
        state: Mapping[str, object], *, persist: bool = False) -> str | None:
    """Re-sweep unique build roots before clearing an unverified cleanup."""
    cleared: dict[str, dict[str, object]] = {}
    for mode, entry in state.items():
        if not isinstance(entry, dict):
            continue
        if not entry.get("cleanup_unverified"):
            continue
        if entry.get("postgres_cleanup_unverified"):
            return (
                "a previous q1024 PostgreSQL backend cleanup did not prove "
                "zero exact owner matches")
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
        cleared_entry = dict(entry)
        cleared_entry.update({
            "process_cleanup_unverified": False,
            "cleanup_unverified": False,
            "guard_cleanup_revalidation_evidence": {
                "cleanup_verified": True,
                "method": "existing_exact_build_root_ownership_sweep",
                "ownership_root": str(ownership_root),
                "completed_at": time.time(),
            },
        })
        cleared[str(mode)] = cleared_entry
    if cleared:
        if not isinstance(state, dict):
            return "cannot persist guarded cleanup revalidation state"
        state.update(cleared)
        if persist:
            _save_state(state)
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
        recorded_entry = state.get(other_mode)
        entry = _refresh(
            state,
            other_mode,
            service_boundary_provider=(
                _postgresql_recovery_service_boundary_provider(
                    recorded_entry)
                if isinstance(recorded_entry, dict) else None),
        )
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

    Ordinary configs retain the existing absolute-or-relative behavior and
    return a canonical absolute path. A sealed algorithm-matrix member returns
    its exact contract-listed repository-relative identity. The caller may
    supply either that identity or its one exact canonical absolute repository
    path; traversal, out-of-repository, and symlink aliases remain rejected.
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

    requested_candidate = Path(requested_path)
    if not requested_candidate.is_absolute():
        campaign_selected, campaign_error = _campaign_config_contract_status(
            requested_path)
        if campaign_error is not None:
            return None, campaign_error
        if campaign_selected:
            return requested_path, None

    candidate = requested_candidate.expanduser()
    if not candidate.is_absolute():
        candidate = REPOSITORY_ROOT / candidate
    try:
        resolved = candidate.resolve(strict=True)
        canonical_root = REPOSITORY_ROOT.resolve(strict=True)
        relative_identity = resolved.relative_to(canonical_root).as_posix()
    except (OSError, RuntimeError, ValueError) as exc:
        return None, (
            "bench_e2e_config must resolve to an existing file inside the "
            f"repository {REPOSITORY_ROOT}: {exc}"
        )
    if not resolved.is_file():
        return None, f"bench_e2e_config is not a regular file: {resolved}"
    if resolved.suffix.lower() != ".json":
        return None, f"bench_e2e_config must name a .json file: {resolved}"

    if requested_candidate.is_absolute():
        campaign_selected, campaign_error = _campaign_config_contract_status(
            relative_identity)
        if campaign_error is not None:
            return None, campaign_error
        if campaign_selected:
            raw_components = requested_path.split("/")
            if (not requested_path.startswith("/")
                    or requested_path.endswith("/")
                    or "\\" in requested_path
                    or any(component in {"", ".", ".."}
                           for component in raw_components[1:])):
                return None, (
                    "absolute campaign config path contains a lexical alias")
            try:
                canonical_file = _campaign_regular_repository_file(
                    relative_identity, "absolute campaign config")
            except (OSError, RuntimeError, ValueError) as exc:
                return None, str(exc)
            if candidate != resolved or canonical_file != resolved:
                return None, (
                    "absolute campaign config path may not traverse a symlink "
                    "or retain another path alias")
            return relative_identity, None
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

    if mode == ALL_ALPHABETS_UNIT_INTEGRATION_MODE and debug:
        return f"{mode} uses a fixed optimized preset and rejects debug=true"

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
    focused_lint_filter = (
        mode == LINT_FOCUSED_MODE
        and filter_supplied
        and bench_filter is not None
    )
    if mode_details.requires_build_only and (
        not build_only
        or run_only
        or reuse_build_receipt is not None
        or (mode in LINT_MODES and build_timeout_sec is not None)
        or debug_supplied or debug
        or (
            (filter_supplied or bench_filter is not None)
            and not focused_lint_filter
        )
        or debug_report_supplied or debug_report
        or bench_e2e_config is not None
    ):
        return (
            f"{mode} is a fixed local compile gate: it requires "
            "build_only=true and accepts no unsupported debug, filter, "
            "debug_report, build_timeout_sec, or bench_e2e_config inputs.")
    if mode == LINT_FOCUSED_MODE and not focused_lint_filter:
        return "lint-focused requires a nonempty filter.paths selection"
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
    campaign_authority = None
    try:
        campaign_authority, campaign_error = (
            _algorithm_matrix_campaign_authority_status(resolved_e2e_config))
        if campaign_error is not None:
            raise RuntimeError(campaign_error)
        campaign_selected = campaign_authority is not None
    except RuntimeError as exc:
        return f"Invalid bench_e2e_config: {exc}"
    if resolved_e2e_config in {
            _ALGORITHM_MATRIX_Q1024_SINGLETON0058_CONFIG,
            _ALGORITHM_MATRIX_Q1024_SINGLETON0068_CONFIG}:
        if campaign_authority is None:
            return (
                "Invalid bench_e2e_config: q1024 extended-timeout config has no "
                "canonical campaign authority")
        timeout_authority, timeout_error = (
            _q1024_extended_timeout_authority(
                resolved_e2e_config, campaign_authority))
        if timeout_error is not None or timeout_authority is None:
            return (
                "Invalid bench_e2e_config: "
                f"{timeout_error or 'q1024 timeout authority missing'}")
    if _q1024_nonreceipt_execution_forbidden(
            campaign_authority, build_only=build_only, run_only=run_only):
        return (
            "q1024 execution requires run_only=true and a current "
            "debug=false bench-e2e-opt build receipt")
    if (campaign_authority is not None
            and campaign_authority.get("name") == "q1024"):
        if campaign_authority.get("execution_role") not in {
                "q1024_singleton", "q1024_packed"}:
            return (
                "Unselected canonical q1024 configs are topology authority only; "
                "execute one exact sealed packed or singleton-fallback member.")
        if not run_only and not build_only:
            return (
                "q1024 execution requires run_only=true and a current "
                "debug=false bench-e2e-opt build receipt")
        if debug_supplied and debug:
            return "q1024 execution requires debug=false"
    if (campaign_authority is not None
            and campaign_authority.get("name") == "observed_rung"):
        if campaign_authority.get(
                "execution_role") != "observed_rung_singleton":
            return (
                "Observed-rung execution requires one exact sealed singleton")
        if not run_only and not build_only:
            return (
                "Observed-rung execution requires run_only=true and a current "
                "debug=false bench-e2e-opt build receipt")
        if debug_supplied and debug:
            return "Observed-rung execution requires debug=false"
    if (campaign_authority is not None
            and campaign_authority.get("name") == "deadline"):
        if campaign_authority.get("execution_role") != "deadline_singleton":
            return "deadline campaign execution requires one sealed singleton"
        if not run_only:
            return (
                "deadline campaign execution requires run_only=true and a current "
                "debug=false bench-e2e-opt build receipt")
        if debug_supplied and debug:
            return "deadline campaign execution requires debug=false"
    if campaign_selected:
        if ((filter_supplied or bench_filter is not None)
                and bench_filter != _ALGORITHM_MATRIX_EXACT_BENCH_FILTER):
            return (
                "Canonical algorithm-matrix campaign configs accept only the "
                "exact filter {'benchmarks': ['bench_e2e']}; case, tag, extra, "
                "or multi-benchmark filters are incompatible with sealed ownership.")
        bench_filter = copy.deepcopy(_ALGORITHM_MATRIX_EXACT_BENCH_FILTER)
    else:
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
            if (campaign_authority is not None
                    and (campaign_authority.get("name") == "observed_rung"
                         or campaign_authority.get("execution_role")
                            in {"q1024_singleton", "q1024_packed"})
                    and (debug is not False
                         or selected_preset != "bench-e2e-opt"
                         or selected_receipt.get("mode") != "benchmark-e2e"
                         or selected_receipt.get("preset_family") != "e2e"
                         or selected_receipt.get("binary_subdir") != "bioseq")):
                return (
                    "Canonical algorithm-matrix execution requires a current "
                    "completed "
                    "debug=false bench-e2e-opt receipt")
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
    lint_paths: list[str] = []
    if mode in LINT_MODES:
        if debug_report_supplied or debug_report:
            return "lint modes do not accept debug_report"
        if mode == LINT_FOCUSED_MODE:
            lint_paths, lint_error = lint_filter_to_paths(bench_filter)
            if lint_error is not None:
                return f"Invalid focused lint filter: {lint_error}"
        elif filter_supplied or bench_filter is not None:
            return "lint-full does not accept filter; it always lints the full owned database"
    elif mode in CATCH_FILTER_MODES:
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

    # Terminal legacy q1024 recovery runs before the scheduling admission
    # lock because its own claim/finish operations are independently CAS-
    # guarded.  It never grants execution authority to the historical receipt.
    _revalidate_legacy_q1024_postgresql_cleanup_before_admission()

    with _global_lock():
        state = _load_state()
        cleanup_blocker = _unverified_guard_cleanup(state, persist=True)
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
        cleanup_blocker = _unverified_guard_cleanup(state, persist=True)
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
        _scrub_benchmark_e2e_config_environment(worker_env)
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
        postgres_application_name = (
            _q1024_postgres_application_name(run_token)
            if (run_only
                and _is_q1024_execution_authority(campaign_authority))
            else None
        )
        q1024_deadlines: dict[str, float] = {}
        if postgres_application_name is not None:
            default_timeout = (
                HARD_TIMEOUT_SEC
                if mode_details.hard_timeout_min is None
                else min(
                    HARD_TIMEOUT_SEC,
                    mode_details.hard_timeout_min * 60.0,
                )
            )
            whole_timeout = _run_only_runtime_timeout_seconds(
                mode,
                worker_e2e_config,
                default_timeout,
                started_at=started_at,
            )
            whole_deadline = started_at + whole_timeout
            q1024_deadlines = {
                "q1024_whole_deadline": whole_deadline,
                "q1024_work_deadline": (
                    whole_deadline
                    - _Q1024_CLEANUP_RESERVE_SECONDS
                    - BUILD_TIMEOUT_TERM_GRACE_SEC),
                "q1024_client_kill_deadline": (
                    whole_deadline - _Q1024_CLEANUP_RESERVE_SECONDS),
            }
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
            "postgres_application_name": postgres_application_name,
            "postgres_backend_owner_started": False,
            "lint_paths": list(lint_paths),
            "rt_priority": configured_rt_priority,
            "effective_run_args":
                (
                    lint_paths
                    if mode in LINT_MODES
                    else (test_flags if mode in CATCH_FILTER_MODES else bench_flags)
                ),
        }
        if postgres_application_name is not None:
            state[mode].update({
                "q1024_run_token": run_token,
                "q1024_watchdog_status": "reserved",
                "q1024_watchdog_process_identity": None,
                "q1024_watchdog_log_file": str(
                    bootstrap_log_path.with_name(
                        f"{bootstrap_log_path.name}.q1024-watchdog")),
                "postgres_cleanup_claim": None,
                **q1024_deadlines,
            })
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
                     "--lint-paths", json.dumps(lint_paths),
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
        detached_identity_required = (
            build_timeout_sec is not None
            or postgres_application_name is not None)
        guarded_identity_missing = (
            detached_identity_required
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
        if detached_identity_required:
            state[mode]["step"] = "armed"
        _save_state(state)

        if postgres_application_name is not None:
            watchdog_path = Path(__file__).with_name(
                "q1024_watchdog.py").resolve()
            watchdog_log_path = Path(str(
                state[mode]["q1024_watchdog_log_file"]))
            watchdog_proc = None
            try:
                if not watchdog_path.is_file():
                    raise RuntimeError(
                        "q1024 watchdog module is not installed")
                watchdog_env = dict(worker_env)
                watchdog_env.pop("PGAPPNAME", None)
                with watchdog_log_path.open(
                        "w", buffering=1) as watchdog_log:
                    watchdog_proc = subprocess.Popen(
                        [sys.executable, str(watchdog_path),
                         "--mode", mode,
                         "--run-token", run_token],
                        stdin=subprocess.DEVNULL,
                        stdout=watchdog_log,
                        stderr=subprocess.STDOUT,
                        start_new_session=True,
                        close_fds=True,
                        env=watchdog_env,
                    )
                watchdog_identity = _capture_process_identity(
                    watchdog_proc.pid)
                if not _valid_detached_worker_identity(
                        watchdog_identity,
                        child_pid=watchdog_proc.pid,
                        parent_pid=os.getpid()):
                    raise RuntimeError(
                        "q1024 watchdog exact pid/start/session capture failed")
                state = _load_state()
                state[mode].update({
                    "q1024_watchdog_status": "armed",
                    "q1024_watchdog_process_identity": (
                        watchdog_identity.__dict__),
                })
                _save_state(state)
            except (OSError, RuntimeError) as exc:
                state = _load_state()
                if mode in state:
                    state[mode].update({
                        "status": "failed",
                        "step": "q1024_watchdog_arm_failed",
                        "completed_at": time.time(),
                        "q1024_watchdog_status": "arm_failed",
                        "q1024_watchdog_termination_evidence": {
                            "cleanup_verified": False,
                            "reason": (
                                "watchdog/worker were not signaled while the "
                                "scheduler state lock was held; both fail "
                                "closed at their pre-setup arm gates"),
                        },
                        "cleanup_unverified": True,
                        "notes": str(exc),
                    })
                    _save_state(state)
                return f"q1024 watchdog arm error: {exc}"
        _log(f"started worker pid={proc.pid} mode={mode} debug={debug} "
             f"rt_priority={configured_rt_priority} "
             f"bench_flags={bench_flags} focused_test_args={test_flags} "
             f"lint_paths={lint_paths} "
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
            + (f" with focused lint paths {shlex.join(lint_paths)}"
               if lint_paths else "")
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
    if mode == LINT_FOCUSED_MODE:
        _log(
            "--run-build-sync: lint-focused requires public MCP filter.paths "
            "and is not an inline remote mode")
        return 2
    if mode == ALL_ALPHABETS_UNIT_INTEGRATION_MODE and debug:
        _log(f"{mode} uses a fixed optimized preset and rejects debug=true")
        return 2
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
    campaign_selected, campaign_error = _campaign_config_contract_status(
        resolved_e2e_config)
    if campaign_error is not None:
        _log(f"--run-build-sync: invalid campaign authority: {campaign_error}")
        return 2
    if campaign_selected and bench_flags not in (
            _ALGORITHM_MATRIX_EXACT_BENCH_FLAGS,
            [*_ALGORITHM_MATRIX_EXACT_BENCH_FLAGS, "--debug"]):
        _log(
            "--run-build-sync: canonical campaign requires exact "
            "benchmark=bench_e2e selection")
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
            "lint_paths": [],
            "bench_e2e_config": resolved_e2e_config,
            "rt_priority": configured_rt_priority,
            "effective_run_args": (
                []
                if mode in LINT_MODES
                else (test_flags if mode in CATCH_FILTER_MODES else bench_flags)
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
        lint_paths=[],
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
        historical_artifact_only = _parse_worker_bool(
            "historical_artifact_only",
            arguments.get("historical_artifact_only", False),
        )
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

    # Artifact locations belong to the target, not necessarily to the mode
    # whose receipt owns it.  In particular, a unit-integration receipt owns
    # both extension targets while the cache DSO remains under
    # bioseq_caching/artifacts/cache_production/<configuration>.
    artifact_binary_subdir = MODES[target_definition.mode].binary_subdir

    # Resolve which preset family owns the requested binary. `debug` is an
    # explicit override; without it, prefer that family's debug tree if built.
    inspect_mode = target_definition.mode
    debug_raw = arguments.get("debug")
    receipt_id = arguments.get("reuse_build_receipt")
    try:
        selected_receipt: dict[str, object] | None = None
        if historical_artifact_only and receipt_id is None:
            raise RuntimeError(
                "historical_artifact_only requires reuse_build_receipt")
        if receipt_id is not None:
            if not isinstance(receipt_id, str) or not receipt_id:
                raise RuntimeError(
                    "reuse_build_receipt must be a non-empty string")
            explicit_debug = (
                _parse_worker_bool("debug", debug_raw)
                if debug_raw is not None else None
            )
            if historical_artifact_only:
                selected_receipt, receipt_error = _validate_build_receipt(
                    receipt_id,
                    expected_debug=explicit_debug,
                    verify_source=False,
                )
            else:
                selected_receipt, receipt_error = _validate_build_receipt(
                    receipt_id,
                    expected_debug=explicit_debug,
                )
            if receipt_error is not None or selected_receipt is None:
                raise RuntimeError(
                    f"inspect receipt validation failed: {receipt_error}")
            receipt_mode = selected_receipt.get("mode")
            receipt_targets = selected_receipt.get("targets")
            if (
                not isinstance(receipt_mode, str)
                or receipt_mode not in MODES
                or not isinstance(receipt_targets, list)
            ):
                raise RuntimeError(
                    "inspect receipt validation returned malformed mode/targets")
            if target not in receipt_targets:
                raise RuntimeError(
                    f"inspect receipt mode {receipt_mode!r} does not own "
                    f"target {target!r}")
            inspect_mode = receipt_mode
            debug_chosen = bool(selected_receipt["debug"])
            if not debug_chosen and not historical_artifact_only:
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

    if historical_artifact_only:
        assert selected_receipt is not None
        authorized_artifacts = selected_receipt.get("authorized_artifacts")
        if not isinstance(authorized_artifacts, list):
            return json.dumps(
                {"error": "historical inspect receipt has malformed "
                          "authorized_artifacts evidence"},
                indent=2,
            )
        authorized_paths = {
            item.get("relative_path")
            for item in authorized_artifacts
            if isinstance(item, dict)
            and isinstance(item.get("relative_path"), str)
        }
        artifact_names = tuple(
            name for name in artifact_names
            if str(Path(artifact_binary_subdir) / name) in authorized_paths
        )
        if not artifact_names:
            return json.dumps(
                {"error": f"historical inspect receipt does not authorize "
                          f"an artifact for target {target!r}"},
                indent=2,
            )
        # Never synthesize a sibling .s file in a historical build tree.
        llc_on_bc = False

    out = _run_inspect_impl(
        function_name=function_name,
        target=target,
        build_dir=inspect_build_dir,
        max_matches=max_matches,
        snippet_lines=snippet_lines,
        source_basename=source_basename,
        llc_on_bc=llc_on_bc,
        max_llc_bc_files=max_llc_bc_files,
        binary_subdir=artifact_binary_subdir,
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
        out["build_metadata"]["mode"] = inspect_mode
        out["build_metadata"]["build_receipt_id"] = receipt_id
        out["build_metadata"]["source_graph_fingerprint"] = (
            selected_receipt["source_graph_fingerprint"])
        if historical_artifact_only:
            out["build_metadata"]["historical_artifact_only"] = True
            out["build_metadata"]["current_worktree_source_verified"] = False
            out["build_metadata"]["artifact_scope"] = (
                "exact receipt-pinned historical artifact")
            out["build_metadata"]["source_scope"] = (
                "receipt-recorded source graph; current worktree not verified")
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

    subagent_activity = subagent_activity_snapshot()
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
        recorded_entry = state.get(mode)
        refreshed_entry = _refresh(
            state,
            mode,
            service_boundary_provider=(
                _postgresql_recovery_service_boundary_provider(
                    recorded_entry)
                if isinstance(recorded_entry, dict) else None),
        )
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
    clang_tidy_summary = entry.get("clang_tidy")
    clang_tidy_suffix = (
        ". Clang-tidy: " + json.dumps(clang_tidy_summary, sort_keys=True)
        if isinstance(clang_tidy_summary, dict)
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
    postgres_cleanup_suffix = (
        ". PostgreSQL owner cleanup: "
        + json.dumps(entry.get("postgres_cleanup_evidence"), sort_keys=True)
        + " cleanup_unverified="
        + str(bool(entry.get("postgres_cleanup_unverified")))
        if entry.get("postgres_cleanup_evidence") is not None
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
                f"{bootstrap_suffix}{guard_suffix}{artifact_suffix}"
                f"{clang_tidy_suffix}{postgres_cleanup_suffix}")

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
        msg += clang_tidy_suffix
        msg += postgres_cleanup_suffix
        return msg

    if status == "killed_timeout":
        return ("Last run was forcefully killed due to timeout. "
                f"{focus_suffix} "
                f"Logs for inspection are available here: "
                f"{log_path}{report_suffix}{qualification_suffix}"
                f"{bootstrap_suffix}{guard_suffix}{artifact_suffix}"
                f"{clang_tidy_suffix}{postgres_cleanup_suffix}. "
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
                f"{clang_tidy_suffix}"
                f"{termination_suffix}{postgres_cleanup_suffix}")

    if status == "aborted":
        return (f"{mode_details.label} run was aborted (worker process died "
                f"unexpectedly after {_format_minutes(duration_min)}). "
                f"{focus_suffix} "
                f"Log: {log_path}{report_suffix}{qualification_suffix}"
                f"{bootstrap_suffix}{guard_suffix}{artifact_suffix}"
                f"{clang_tidy_suffix}"
                f"{termination_suffix}{postgres_cleanup_suffix}")

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
