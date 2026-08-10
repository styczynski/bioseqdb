"""PostgreSQL runtime lifecycle support."""

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
    CMAKE_PRESETS_FILE,
    REPOSITORY_ROOT,
    _cmake_cache_value,
    _cmake_preset_cache_contract,
    _is_aws_preset,
)
from process_state import (
    _log,
    _stream_subprocess,
    _trusted_root_owned_executable,
)

_CMAKE_HELPER_DIR = REPOSITORY_ROOT / "cmake"
if str(_CMAKE_HELPER_DIR) not in sys.path:
    sys.path.insert(0, str(_CMAKE_HELPER_DIR))



from sanitizer_runtime import (
    SANITIZER_RUNTIME_ENV, SanitizerRuntimeError,
    postgres_sanitizer_environment, sanitizer_descendant_environment,
)



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
    """Build the shell command passed to bioseq_benchmark_e2e.

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
