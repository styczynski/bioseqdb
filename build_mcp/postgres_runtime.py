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

_LIBPQ_CONNECTION_ENVIRONMENT_KEYS = (
    "PGAPPNAME",
    "PGCHANNELBINDING",
    "PGCLIENTENCODING",
    "PGCONNECT_TIMEOUT",
    "PGDATABASE",
    "PGGSSENCMODE",
    "PGGSSLIB",
    "PGHOST",
    "PGHOSTADDR",
    "PGKRBSRVNAME",
    "PGLOADBALANCEHOSTS",
    "PGOPTIONS",
    "PGPASSFILE",
    "PGPASSWORD",
    "PGPORT",
    "PGREQUIREAUTH",
    "PGREQUIREPEER",
    "PGREQUIRESSL",
    "PGSERVICE",
    "PGSERVICEFILE",
    "PGSSLCERT",
    "PGSSLCOMPRESSION",
    "PGSSLCRL",
    "PGSSLCRLDIR",
    "PGSSLKEY",
    "PGSSLMAXPROTOCOLVERSION",
    "PGSSLMINPROTOCOLVERSION",
    "PGSSLMODE",
    "PGSSLNEGOTIATION",
    "PGSSLROOTCERT",
    "PGSSLSNI",
    "PGSYSCONFDIR",
    "PGTARGETSESSIONATTRS",
    "PGUSER",
)

_REQUIRED_MAX_PREPARED_TRANSACTIONS = 16

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

def _prepare_postgresql_test_database(
        active_build_dir: Path,
        active_preset: str,
        log_handle,
        deadline: float,
        *,
        env: Mapping[str, str] | None = None) -> None:
    """Make PostgreSQL match the integration-test connection defaults."""
    is_aws = (
        sys.platform.startswith("linux")
        and _is_aws_preset(active_preset)
    )
    log_handle.write(
        "\n# Preparing PostgreSQL test auth/database "
        "(postgres/postgres, database bioseq) ...\n")
    log_handle.flush()

    pg_config = _pg_config_from_cache(active_build_dir)
    try:
        bindir_result = subprocess.run(
            [pg_config, "--bindir"],
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(
            f"pg_config --bindir failed while preparing PostgreSQL "
            f"test database: {exc}") from exc
    if bindir_result.returncode != 0:
        raise RuntimeError(
            "pg_config --bindir failed while preparing PostgreSQL "
            f"test database: {bindir_result.stderr.strip()}")

    pg_bindir = Path(bindir_result.stdout.strip())
    psql = pg_bindir / "psql"
    createdb = pg_bindir / "createdb"
    for executable in (psql, createdb):
        if not executable.is_file():
            raise RuntimeError(
                f"PostgreSQL test database tool not found: {executable}")

    # _query_postgresql_data_directory() has already proven that the
    # configured cluster admits this builder (or the AWS postgres uid) over
    # its Unix socket.  Use that administrator path to establish the test
    # role/password; trying the password before setting it would make clean
    # clusters circular.  Ambient libpq selectors must not redirect setup.
    test_env = dict(os.environ if env is None else env)
    for key in _LIBPQ_CONNECTION_ENVIRONMENT_KEYS:
        test_env.pop(key, None)

    role_argv = [
        str(psql),
        "-v", "ON_ERROR_STOP=1",
        "--dbname", "postgres",
        "--command", "ALTER ROLE postgres WITH LOGIN PASSWORD 'postgres';",
    ]
    if is_aws:
        role_argv = ["sudo", "-n", "-u", "postgres", *role_argv]

    rc = _stream_subprocess(
        role_argv, log_handle, deadline, env=test_env)
    if rc != 0:
        raise RuntimeError(
            f"PostgreSQL test role setup exited with code {rc}")

    query = shlex.join([
        str(psql),
        "-Atq", "--dbname", "postgres",
        "--command",
        "SELECT 1 FROM pg_database WHERE datname = 'bioseq';",
    ])
    create = shlex.join([
        str(createdb),
        "--owner", "postgres", "bioseq",
    ])
    create_db_script = (
        "set -euo pipefail\n"
        f"if ! {query} | grep -qx 1; then\n"
        f"  {create}\n"
        "fi\n"
    )
    database_argv = ["/bin/bash", "-lc", create_db_script]
    if is_aws:
        database_argv = [
            "sudo", "-n", "-u", "postgres", *database_argv]
    rc = _stream_subprocess(
        database_argv, log_handle, deadline, env=test_env)

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

def _postgresql_admin_psql_argv(
        psql: str, active_preset: str, *arguments: str) -> list[str]:
    """Address the configured cluster through its proven local admin path."""
    argv = [psql, *arguments]
    if sys.platform.startswith("linux") and _is_aws_preset(active_preset):
        return ["sudo", "-n", "-u", "postgres", *argv]
    return argv

def _postgresql_admin_environment() -> dict[str, str]:
    """Prevent ambient libpq selectors from redirecting cluster mutation."""
    environment = dict(os.environ)
    for key in _LIBPQ_CONNECTION_ENVIRONMENT_KEYS:
        environment.pop(key, None)
    return environment

def _shared_preload_contains_bioseq(setting: str) -> bool:
    for entry in setting.split(","):
        normalized = entry.strip()
        if len(normalized) >= 2 and normalized[0] == normalized[-1] == '"':
            normalized = normalized[1:-1]
        if normalized in {"bioseq", "$libdir/bioseq"}:
            return True
    return False

def _ensure_postgresql_shared_preload(
        psql: str, active_preset: str, log_handle,
        deadline: float) -> None:
    """Persistently add bioseq to shared_preload_libraries before restart."""
    environment = _postgresql_admin_environment()
    show_argv = _postgresql_admin_psql_argv(
        psql, active_preset,
        "-X", "-Atq", "--dbname", "postgres",
        "--command", "SHOW shared_preload_libraries;",
    )
    try:
        shown = subprocess.run(
            show_argv,
            capture_output=True,
            text=True,
            timeout=10,
            env=environment,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(
            "could not inspect PostgreSQL shared_preload_libraries: "
            f"{exc}") from exc
    if shown.returncode != 0:
        detail = (shown.stderr or shown.stdout).strip()
        raise RuntimeError(
            "could not inspect PostgreSQL shared_preload_libraries: "
            f"{detail or f'exit {shown.returncode}'}")

    current = shown.stdout.strip()
    if _shared_preload_contains_bioseq(current):
        return
    desired = "bioseq" if not current else f"{current},bioseq"
    if "\x00" in desired or "\n" in desired or "\r" in desired:
        raise RuntimeError(
            "shared_preload_libraries contains an invalid line break")
    sql_literal = desired.replace("'", "''")
    alter_argv = _postgresql_admin_psql_argv(
        psql, active_preset,
        "-X", "-v", "ON_ERROR_STOP=1", "--dbname", "postgres",
        "--command",
        "ALTER SYSTEM SET shared_preload_libraries = "
        f"'{sql_literal}';",
    )
    log_handle.write(
        "\n# Enabling bioseq in PostgreSQL shared_preload_libraries ...\n")
    log_handle.flush()
    rc = _stream_subprocess(
        alter_argv, log_handle, deadline, env=environment)
    if rc != 0:
        raise RuntimeError(
            "PostgreSQL shared_preload_libraries setup exited with code "
            f"{rc}")

def _verify_postgresql_shared_preload(
        psql: str, active_preset: str) -> None:
    """Fail closed when a higher-priority setting defeated ALTER SYSTEM."""
    environment = _postgresql_admin_environment()
    show_argv = _postgresql_admin_psql_argv(
        psql, active_preset,
        "-X", "-Atq", "--dbname", "postgres",
        "--command", "SHOW shared_preload_libraries;",
    )
    try:
        shown = subprocess.run(
            show_argv,
            capture_output=True,
            text=True,
            timeout=10,
            env=environment,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(
            "could not verify PostgreSQL shared_preload_libraries after "
            f"restart: {exc}") from exc
    if shown.returncode != 0:
        detail = (shown.stderr or shown.stdout).strip()
        raise RuntimeError(
            "could not verify PostgreSQL shared_preload_libraries after "
            f"restart: {detail or f'exit {shown.returncode}'}")
    if not _shared_preload_contains_bioseq(shown.stdout.strip()):
        raise RuntimeError(
            "PostgreSQL restarted without bioseq in "
            "shared_preload_libraries")

def _max_prepared_transactions_value(setting: str) -> int:
    try:
        value = int(setting.strip(), 10)
    except ValueError as exc:
        raise RuntimeError(
            "PostgreSQL returned an invalid max_prepared_transactions "
            f"value: {setting!r}") from exc
    if value < 0:
        raise RuntimeError(
            "PostgreSQL returned a negative max_prepared_transactions "
            f"value: {value}")
    return value

def _ensure_postgresql_max_prepared_transactions(
        psql: str, active_preset: str, log_handle,
        deadline: float) -> None:
    """Persist the project minimum before restarting the shared cluster."""
    environment = _postgresql_admin_environment()
    show_argv = _postgresql_admin_psql_argv(
        psql, active_preset,
        "-X", "-Atq", "--dbname", "postgres",
        "--command", "SHOW max_prepared_transactions;",
    )
    try:
        shown = subprocess.run(
            show_argv,
            capture_output=True,
            text=True,
            timeout=10,
            env=environment,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(
            "could not inspect PostgreSQL max_prepared_transactions: "
            f"{exc}") from exc
    if shown.returncode != 0:
        detail = (shown.stderr or shown.stdout).strip()
        raise RuntimeError(
            "could not inspect PostgreSQL max_prepared_transactions: "
            f"{detail or f'exit {shown.returncode}'}")

    current = _max_prepared_transactions_value(shown.stdout)
    if current >= _REQUIRED_MAX_PREPARED_TRANSACTIONS:
        return

    alter_argv = _postgresql_admin_psql_argv(
        psql, active_preset,
        "-X", "-v", "ON_ERROR_STOP=1", "--dbname", "postgres",
        "--command",
        "ALTER SYSTEM SET max_prepared_transactions = "
        f"'{_REQUIRED_MAX_PREPARED_TRANSACTIONS}';",
    )
    log_handle.write(
        "\n# Enabling PostgreSQL prepared transactions for bioseq tests ...\n")
    log_handle.flush()
    rc = _stream_subprocess(
        alter_argv, log_handle, deadline, env=environment)
    if rc != 0:
        raise RuntimeError(
            "PostgreSQL max_prepared_transactions setup exited with code "
            f"{rc}")

def _verify_postgresql_max_prepared_transactions(
        psql: str, active_preset: str) -> None:
    """Fail closed if restart did not activate the project minimum."""
    environment = _postgresql_admin_environment()
    show_argv = _postgresql_admin_psql_argv(
        psql, active_preset,
        "-X", "-Atq", "--dbname", "postgres",
        "--command", "SHOW max_prepared_transactions;",
    )
    try:
        shown = subprocess.run(
            show_argv,
            capture_output=True,
            text=True,
            timeout=10,
            env=environment,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(
            "could not verify PostgreSQL max_prepared_transactions after "
            f"restart: {exc}") from exc
    if shown.returncode != 0:
        detail = (shown.stderr or shown.stdout).strip()
        raise RuntimeError(
            "could not verify PostgreSQL max_prepared_transactions after "
            f"restart: {detail or f'exit {shown.returncode}'}")
    actual = _max_prepared_transactions_value(shown.stdout)
    if actual < _REQUIRED_MAX_PREPARED_TRANSACTIONS:
        raise RuntimeError(
            "PostgreSQL restarted with max_prepared_transactions="
            f"{actual}; bioseq tests require at least "
            f"{_REQUIRED_MAX_PREPARED_TRANSACTIONS}")

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
    environment = _postgresql_admin_environment()
    direct_detail = ""
    try:
        direct = subprocess.run(
            argv, capture_output=True, text=True, timeout=10,
            env=environment)
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
            env=environment,
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

    _ensure_postgresql_shared_preload(
        psql, active_preset, log_handle, deadline)
    _ensure_postgresql_max_prepared_transactions(
        psql, active_preset, log_handle, deadline)

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
    _verify_postgresql_shared_preload(psql, active_preset)
    _verify_postgresql_max_prepared_transactions(psql, active_preset)

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
