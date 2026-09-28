"""PostgreSQL error-safety build and runtime validation."""

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
    POSTGRES_ERROR_SAFETY_CASE_TEMPLATE,
    POSTGRES_ERROR_SAFETY_MODE,
    POSTGRES_ERROR_SAFETY_PEER_BUILD_DIR_CACHE_KEY,
    POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
    POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET,
    REPOSITORY_ROOT,
    _cmake_cache_bool,
    _cmake_cache_value,
    _cmake_preset_cache_contract,
)
from postgres_runtime import (
    _postgresql_discovery_cache_reset_args,
)
from process_state import (
    _build_worker_descendant_environment,
    _stream_subprocess,
    _update_entry,
)







POSTGRES_ERROR_SAFETY_RUNTIME_PROFILES_TARGET = (
    "bioseq_sql_postgres_error_safety_runtime_profiles")

POSTGRES_ERROR_SAFETY_EVIDENCE_TARGET = (
    "bioseq_sql_postgres_error_safety_evidence")

POSTGRES_ERROR_SAFETY_RUNTIME_REPORT_RELATIVE_PATH = Path(
    "bioseq/generated/sql/safety/runtime/candidate/runtime-validation.json")

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
                REPOSITORY_ROOT
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
        sequence.append((
            POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
            POSTGRES_ERROR_SAFETY_EVIDENCE_TARGET,
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
        env: dict[str, str], rt_priority: int | None,
        services: Mapping[str, object]) -> None:
    _clang_rt_cmake_cache_repairs = services["clang_rt_cmake_cache_repairs"]
    _cmake_configure_stamp_document = services["cmake_configure_stamp_document"]
    _cmake_executable_identity = services["cmake_executable_identity"]
    _ensure_cmake_preset_tree_fresh = services["ensure_cmake_preset_tree_fresh"]
    _finalize_cmake_configure_stamp = services["finalize_cmake_configure_stamp"]
    _initial_clang_rt_cmake_args = services["initial_clang_rt_cmake_args"]
    _run_cmake_preset_configure = services["run_cmake_preset_configure"]
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

def _run_postgres_error_safety_worker(
        entry: dict, build_only: bool, debug: bool,
        bench_flags: list[str], test_flags: list[str],
        bench_e2e_config: str | None,
        services: Mapping[str, object]) -> int:
    CMAKE_PARALLEL_JOBS = services["cmake_parallel_jobs"]
    HARD_TIMEOUT_MIN = services["hard_timeout_min"]
    HARD_TIMEOUT_SEC = services["hard_timeout_sec"]
    _configured_local_build_rt_priority = services[
        "configured_local_build_rt_priority"]
    _ensure_local_toolchain_environment = services[
        "ensure_local_toolchain_environment"]
    _local_clang_rt_is_enabled = services["local_clang_rt_is_enabled"]
    mode = POSTGRES_ERROR_SAFETY_MODE
    started_at = float(entry["started_at"])
    deadline = started_at + HARD_TIMEOUT_SEC
    log_path = Path(entry["log_file"])
    production_build_dir = (
        REPOSITORY_ROOT / "build" / POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET)
    test_indexes_build_dir = (
        REPOSITORY_ROOT / "build" / POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET)
    qualification_report: Path | None = None

    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_handle = log_path.open("w", buffering=1)
    log_handle.write(
        f"# bioseq build worker — mode={mode} pid={os.getpid()} "
        f"started={time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(started_at))}\n"
        f"# repository_root={REPOSITORY_ROOT}\n"
        f"# production_preset={POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET}\n"
        f"# production_build_dir={production_build_dir}\n"
        f"# test_indexes_preset={POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET}\n"
        f"# test_indexes_build_dir={test_indexes_build_dir}\n"
        f"# build_only={build_only}\n"
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
                services,
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
                cwd=REPOSITORY_ROOT,
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
            evidence_target = build_sequence[2][1]
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
                cwd=REPOSITORY_ROOT,
                env=env,
            )
            if rc != 0:
                raise RuntimeError(
                    "private-cluster evidence target "
                    f"{evidence_target!r} exited "
                    f"with code {rc}")
            qualification_report = (
                production_build_dir
                / POSTGRES_ERROR_SAFETY_RUNTIME_REPORT_RELATIVE_PATH
            )
            try:
                report_metadata = qualification_report.lstat()
            except OSError as exc:
                raise RuntimeError(
                    "PostgreSQL error-safety evidence target published no "
                    f"runtime validation report: {qualification_report}"
                ) from exc
            if (
                qualification_report.is_symlink()
                or not stat.S_ISREG(report_metadata.st_mode)
                or report_metadata.st_nlink != 1
            ):
                raise RuntimeError(
                    "PostgreSQL error-safety runtime validation report must "
                    "be a regular single-link non-symlink file: "
                    f"{qualification_report}")
        completed_at = time.time()
        _update_entry(
            mode,
            status="completed",
            step="completed",
            completed_at=completed_at,
            exit_code=0,
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
