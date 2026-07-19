#!/usr/bin/env python3
"""Generic disposable private PostgreSQL cluster lifecycle (spec section 13.1).

This importable module owns cluster state: unique data/socket/port allocation,
init/start/createdb/bootstrap/stop, signal-safe primitives, configuration
rendering, an explicit child-environment allowlist, and optional address-space
containment. It is source-neutral: it knows nothing about the caching provider
or the website generator. Consumers (main/cmake/run_private_postgres.py and the
cache adapter main/bioseq_caching/test/run_private_provider_tests.py) supply
their bootstrap/library/preload arguments through ClusterInputs.

No lifecycle subprocess is unbounded: every invocation runs in its own process
group with a validated deadline, and on expiry the whole group is SIGTERM'd,
given two seconds, then SIGKILL'd before the owned server is stopped.
"""

from __future__ import annotations

import contextlib
import dataclasses
import datetime as dt
import getpass
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import uuid
from typing import Sequence


class ClusterCommandError(RuntimeError):
    """A cluster lifecycle command failed, timed out, or was misconfigured."""


# Connection environment that must never leak into a child: any of these would
# silently redirect libpq away from the private socket (spec section 13.1).
_STRIPPED_ENV_EXACT = frozenset({
    "PGHOSTADDR", "PGSERVICE", "PGSERVICEFILE", "PGPASSFILE", "PGPASSWORD",
    "PGOPTIONS", "PGSSLMODE", "PGCHANNELBINDING", "PGTARGETSESSIONATTRS",
    "PGHOST", "PGPORT", "PGDATABASE", "PGUSER", "PGCLIENTENCODING",
})
# Non-connection environment that is safe to forward verbatim.
_ALLOWED_ENV_EXACT = frozenset({
    "PATH", "HOME", "LANG", "LANGUAGE", "TZ", "TERM", "TMPDIR", "USER",
    "LOGNAME", "LD_LIBRARY_PATH", "LD_PRELOAD", "DYLD_LIBRARY_PATH",
    "ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS", "MSAN_OPTIONS",
    "ASAN_SYMBOLIZER_PATH", "PYTHONUNBUFFERED",
})
_ALLOWED_ENV_PREFIXES = ("LC_",)


def _resolved_file(value: str, label: str) -> Path:
    path = Path(value).expanduser().resolve()
    if not path.is_file():
        raise ClusterCommandError(f"{label} is not a file: {path}")
    return path


def _resolved_dir(value: str, label: str) -> Path:
    path = Path(value).expanduser().resolve()
    if not path.is_dir():
        raise ClusterCommandError(f"{label} is not a directory: {path}")
    return path


def _validated_timeout(value: int, label: str) -> int:
    number = int(value)
    if number < 1 or number > 600:
        raise ClusterCommandError(
            f"{label} must be an integer 1-600 seconds, got {value}")
    return number


def _validated_identifier(value: str, label: str) -> str:
    if not value or len(value) > 63:
        raise ClusterCommandError(f"{label} must be 1-63 characters: {value!r}")
    if value[0].isdigit():
        raise ClusterCommandError(f"{label} must not start with a digit: {value!r}")
    for character in value:
        if not (character.islower() or character.isdigit() or character == "_"):
            raise ClusterCommandError(
                f"{label} must be lower_snake_case identifier: {value!r}")
    return value


def _postgresql_conf_literal(value: str) -> str:
    if "\n" in value or "\r" in value:
        raise ClusterCommandError("PostgreSQL configuration value contains a newline")
    return "'" + value.replace("'", "''") + "'"


def _available_local_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def _terminate_group(process: subprocess.Popen) -> None:
    """SIGTERM the process group, wait two seconds, then SIGKILL survivors."""
    with contextlib.suppress(ProcessLookupError, PermissionError):
        os.killpg(process.pid, signal.SIGTERM)
    with contextlib.suppress(subprocess.TimeoutExpired):
        process.wait(timeout=2)
        return
    with contextlib.suppress(ProcessLookupError, PermissionError):
        os.killpg(process.pid, signal.SIGKILL)
    with contextlib.suppress(Exception):
        process.wait(timeout=2)


def _run_checked(
    command: Sequence[str],
    *,
    timeout: int,
    env: dict[str, str] | None = None,
    output_path: Path | None = None,
    preexec_fn=None,
) -> str:
    """Run a lifecycle command in its own process group with a hard deadline."""
    process = subprocess.Popen(
        list(command),
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
        preexec_fn=preexec_fn,
    )
    try:
        stdout, _ = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        _terminate_group(process)
        stdout, _ = process.communicate()
        if output_path is not None:
            output_path.parent.mkdir(parents=True, exist_ok=True)
            output_path.write_text(stdout or "", encoding="utf-8")
        raise ClusterCommandError(
            f"command timed out after {timeout}s: {' '.join(command)}")
    if output_path is not None:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(stdout or "", encoding="utf-8")
    if process.returncode != 0:
        raise ClusterCommandError(
            f"command failed with exit code {process.returncode}: "
            f"{' '.join(command)}\n{stdout}")
    return stdout or ""


def _address_space_preexec(limit_mib: int | None):
    """Return a preexec hook that caps RLIMIT_AS, or None when unbounded."""
    if limit_mib is None:
        return None
    import resource

    limit_bytes = int(limit_mib) * 1024 * 1024

    def _apply() -> None:  # runs in the forked child before exec
        resource.setrlimit(resource.RLIMIT_AS, (limit_bytes, limit_bytes))

    return _apply


@dataclasses.dataclass(frozen=True)
class ClusterInputs:
    """Everything the generic lifecycle needs, supplied by each consumer."""

    bindir: Path
    database: str
    config_template: Path
    output_root: Path
    run_label: str = "private_pg"
    bootstraps: tuple[Path, ...] = ()
    # Optional per-bootstrap log-file stems, parallel to `bootstraps`. When a
    # consumer supplies fewer labels than bootstraps, the remainder fall back to
    # `bootstrap_{index}`. Consumers use this for stable, readable log names.
    bootstrap_labels: tuple[str, ...] = ()
    library_dirs: tuple[Path, ...] = ()
    preload_libraries: tuple[str, ...] = ()
    disable_prepared_transactions: bool = False
    data_dir_parent: Path | None = None
    socket_dir_parent: Path | None = None
    log_dir: Path | None = None
    init_command_timeout: int = 60
    bootstrap_timeout: int = 60
    startup_timeout: int = 60
    shutdown_timeout: int = 60
    address_space_limit_mib: int | None = None


class UniquePrivateCluster:
    """Lifecycle owner for one private PostgreSQL node and its database."""

    def __init__(self, inputs: ClusterInputs) -> None:
        self.inputs = inputs
        stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        suffix = uuid.uuid4().hex[:10]
        run_name = f"{inputs.run_label}_{stamp}_{os.getpid()}_{suffix}"
        self.run_dir = inputs.output_root / run_name
        data_parent = inputs.data_dir_parent or self.run_dir
        self.data_dir = data_parent / "data" if inputs.data_dir_parent else self.run_dir / "data"
        # AF_UNIX socket paths cap at 107 bytes; keep the socket dir short even
        # when the retained run/log path is deep.
        socket_parent = inputs.socket_dir_parent or Path(tempfile.gettempdir())
        socket_parent.mkdir(parents=True, exist_ok=True)
        self.socket_dir = Path(
            tempfile.mkdtemp(prefix=f"bqpg_{os.getpid()}_", dir=str(socket_parent)))
        self.log_dir = inputs.log_dir or self.run_dir
        self.server_log = self.log_dir / "server_startup.log"
        self.lifecycle_log = self.log_dir / "lifecycle.log"
        self.port = _available_local_port()
        self.database = _validated_identifier(inputs.database, "database")
        self.database_user = getpass.getuser()
        self._started = False
        self._database_created = False

    def _program(self, name: str) -> Path:
        program = self.inputs.bindir / name
        if not program.is_file():
            raise ClusterCommandError(f"PostgreSQL program not found: {program}")
        return program

    def _append_lifecycle(self, message: str) -> None:
        timestamp = dt.datetime.now(dt.timezone.utc).isoformat()
        self.lifecycle_log.parent.mkdir(parents=True, exist_ok=True)
        with self.lifecycle_log.open("a", encoding="utf-8") as stream:
            stream.write(f"{timestamp} {message}\n")

    def _write_configuration(self) -> None:
        base = self.inputs.config_template.read_text(encoding="utf-8")
        unique_dirs = list(dict.fromkeys(str(path) for path in self.inputs.library_dirs))
        dynamic_path = os.pathsep.join([*unique_dirs, "$libdir"])
        additions = [
            "",
            "# Unique values appended by private_postgres.py",
            f"port = {self.port}",
            f"unix_socket_directories = {_postgresql_conf_literal(str(self.socket_dir))}",
            f"dynamic_library_path = {_postgresql_conf_literal(dynamic_path)}",
        ]
        if self.inputs.preload_libraries:
            preload = ",".join(self.inputs.preload_libraries)
            additions.append(
                f"shared_preload_libraries = {_postgresql_conf_literal(preload)}")
        if self.inputs.disable_prepared_transactions:
            additions.append("max_prepared_transactions = 0")
        additions.append("")
        with (self.data_dir / "postgresql.conf").open("a", encoding="utf-8") as stream:
            if base and not base.endswith("\n"):
                base += "\n"
            stream.write(base)
            stream.write("\n".join(additions))

    def start(self) -> None:
        self.run_dir.mkdir(parents=True, exist_ok=False)
        self.log_dir.mkdir(parents=True, exist_ok=True)
        init_timeout = _validated_timeout(
            self.inputs.init_command_timeout, "init-command-timeout")
        self._append_lifecycle(f"initializing data directory {self.data_dir}")
        _run_checked(
            [
                str(self._program("initdb")),
                "--pgdata", str(self.data_dir),
                "--username", self.database_user,
                "--auth-local=trust",
                "--auth-host=reject",
                "--encoding=UTF8",
                "--no-locale",
                "--no-sync",
            ],
            timeout=init_timeout,
            output_path=self.run_dir / "initdb.log",
        )
        self._write_configuration()
        self._append_lifecycle(f"starting private node on port {self.port}")
        _run_checked(
            [
                str(self._program("pg_ctl")),
                "--pgdata", str(self.data_dir),
                "--log", str(self.server_log),
                "--wait", "start",
            ],
            timeout=_validated_timeout(self.inputs.startup_timeout, "startup-timeout"),
            output_path=self.run_dir / "pg_ctl_start.log",
            preexec_fn=_address_space_preexec(self.inputs.address_space_limit_mib),
        )
        self._started = True
        _run_checked(
            [
                str(self._program("createdb")),
                "--host", str(self.socket_dir),
                "--port", str(self.port),
                "--username", self.database_user,
                self.database,
            ],
            timeout=init_timeout,
            output_path=self.run_dir / "createdb.log",
        )
        self._database_created = True
        for index, bootstrap in enumerate(self.inputs.bootstraps):
            label = (
                self.inputs.bootstrap_labels[index]
                if index < len(self.inputs.bootstrap_labels)
                else f"bootstrap_{index}"
            )
            self._bootstrap(bootstrap, label)

    def _bootstrap(self, bootstrap: Path, label: str) -> None:
        self._append_lifecycle(f"executing {label} {bootstrap}")
        _run_checked(
            [
                str(self._program("psql")),
                "-X", "-v", "ON_ERROR_STOP=1",
                "--host", str(self.socket_dir),
                "--port", str(self.port),
                "--username", self.database_user,
                "--dbname", self.database,
                "--file", str(bootstrap),
            ],
            timeout=_validated_timeout(self.inputs.bootstrap_timeout, "bootstrap-timeout"),
            output_path=self.run_dir / f"{label}.log",
        )

    def child_environment(self) -> dict[str, str]:
        """Explicit allowlist environment for the launched child (§13.1)."""
        env: dict[str, str] = {}
        for name, value in os.environ.items():
            if name in _STRIPPED_ENV_EXACT:
                continue
            if name.startswith("PG") or name.startswith("BIOSEQ_TEST_PG"):
                continue
            if name in _ALLOWED_ENV_EXACT or name.startswith(_ALLOWED_ENV_PREFIXES):
                env[name] = value
        env.update({
            "PGHOST": str(self.socket_dir),
            "PGPORT": str(self.port),
            "PGDATABASE": self.database,
            "PGUSER": self.database_user,
            "BIOSEQ_TEST_PG_HOST": str(self.socket_dir),
            "BIOSEQ_TEST_PG_PORT": str(self.port),
            "BIOSEQ_TEST_PG_DATABASE": self.database,
            "BIOSEQ_TEST_PG_USER": self.database_user,
            "BIOSEQ_TEST_PG_RUN_DIR": str(self.run_dir),
            "BIOSEQ_TEST_PG_DATA_DIR": str(self.data_dir),
        })
        return env

    def run_child(self, command: Sequence[str]) -> int:
        """Launch exactly one child in a new process group, forwarding signals."""
        if not command:
            raise ClusterCommandError("no child command supplied")
        process = subprocess.Popen(
            list(command),
            env=self.child_environment(),
            start_new_session=True,
            preexec_fn=_address_space_preexec(self.inputs.address_space_limit_mib),
        )

        def _forward(signum, _frame):
            with contextlib.suppress(ProcessLookupError, PermissionError):
                os.killpg(process.pid, signum)

        previous = {
            number: signal.signal(number, _forward)
            for number in (signal.SIGINT, signal.SIGTERM)
        }
        try:
            return process.wait()
        finally:
            for number, handler in previous.items():
                signal.signal(number, handler)

    def _copy_collector_logs(self) -> None:
        source = self.data_dir / "pg_log"
        destination = self.run_dir / "postgresql_logs"
        if source.is_dir():
            destination.mkdir(exist_ok=True)
            for log_file in source.iterdir():
                if log_file.is_file():
                    shutil.copy2(log_file, destination / log_file.name)

    def stop(self) -> None:
        shutdown_timeout = _validated_timeout(
            self.inputs.shutdown_timeout, "shutdown-timeout")
        if self._started and self._database_created:
            with contextlib.suppress(Exception):
                _run_checked(
                    [
                        str(self._program("dropdb")),
                        "--if-exists",
                        "--host", str(self.socket_dir),
                        "--port", str(self.port),
                        "--username", self.database_user,
                        self.database,
                    ],
                    timeout=shutdown_timeout,
                    output_path=self.run_dir / "dropdb.log",
                )
        if self._started:
            try:
                _run_checked(
                    [
                        str(self._program("pg_ctl")),
                        "--pgdata", str(self.data_dir),
                        "--wait", "--mode=fast", "stop",
                    ],
                    timeout=shutdown_timeout,
                    output_path=self.run_dir / "pg_ctl_stop.log",
                )
            except Exception:
                with contextlib.suppress(Exception):
                    _run_checked(
                        [
                            str(self._program("pg_ctl")),
                            "--pgdata", str(self.data_dir),
                            "--wait", "--mode=immediate", "stop",
                        ],
                        timeout=shutdown_timeout,
                        output_path=self.run_dir / "pg_ctl_stop_immediate.log",
                    )
        self._copy_collector_logs()
        shutil.rmtree(self.data_dir, ignore_errors=True)
        shutil.rmtree(self.socket_dir, ignore_errors=True)
        if self.run_dir.exists():
            self._append_lifecycle("private data and socket directories removed")
        self._started = False
        self._database_created = False

    def __enter__(self) -> "UniquePrivateCluster":
        self.start()
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.stop()
