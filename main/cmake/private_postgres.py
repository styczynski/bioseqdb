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
import importlib.util
import os
from pathlib import Path
import shutil
import shlex
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import uuid
from typing import Sequence


def _load_sibling_sanitizer_runtime():
    """Load the exact sibling helper under direct and file-spec imports."""
    helper_path = Path(__file__).resolve().with_name("sanitizer_runtime.py")
    try:
        helper_path = helper_path.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise ImportError(
            f"sanitizer runtime helper is missing: {helper_path}") from exc

    module_name = "sanitizer_runtime"
    existing = sys.modules.get(module_name)
    if existing is not None:
        existing_file = getattr(existing, "__file__", None)
        try:
            existing_path = Path(existing_file).resolve(strict=True)
        except (OSError, RuntimeError, TypeError) as exc:
            raise ImportError(
                "loaded sanitizer_runtime has no valid source path") from exc
        if existing_path != helper_path:
            raise ImportError(
                "refusing non-sibling sanitizer_runtime module: "
                f"{existing_path}")
        return existing

    spec = importlib.util.spec_from_file_location(module_name, helper_path)
    if spec is None or spec.loader is None:
        raise ImportError(
            f"cannot load sanitizer runtime helper: {helper_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        if sys.modules.get(module_name) is module:
            sys.modules.pop(module_name, None)
        raise
    return module


_sanitizer_runtime = _load_sibling_sanitizer_runtime()
SANITIZER_RUNTIME_ENV = _sanitizer_runtime.SANITIZER_RUNTIME_ENV
postgres_sanitizer_environment = (
    _sanitizer_runtime.postgres_sanitizer_environment)
sanitizer_descendant_environment = (
    _sanitizer_runtime.sanitizer_descendant_environment)


class ClusterCommandError(RuntimeError):
    """A cluster lifecycle command failed, timed out, or was misconfigured."""


@dataclasses.dataclass(frozen=True)
class _CommandResult:
    returncode: int
    stdout: str


@dataclasses.dataclass(frozen=True)
class _ServerProbe:
    state: str
    detail: str
    stop_is_safe: bool = False


@dataclasses.dataclass(frozen=True)
class _PostmasterProcess:
    pid: int | None
    is_live: bool
    ownership_proven: bool
    detail: str


_SERVER_RUNNING = "running"
_SERVER_STOPPED = "stopped"
_SERVER_UNKNOWN = "unknown"


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
    # A private-cluster runner may own the long-lived bioseq-build MCP server.
    # Preserve only its bounded preset selectors; the child still receives no
    # ambient connection state and cannot redirect libpq away from this node.
    "BIOSEQ_TEST_BUILD_PRESET", "BIOSEQ_TEST_BUILD_PRESET_DEBUG",
    SANITIZER_RUNTIME_ENV,
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


def _run_bounded(
    command: Sequence[str],
    *,
    timeout: int,
    env: dict[str, str] | None = None,
    output_path: Path | None = None,
    preexec_fn=None,
) -> _CommandResult:
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
    return _CommandResult(
        returncode=int(process.returncode),
        stdout=stdout or "",
    )


def _run_checked(
    command: Sequence[str],
    *,
    timeout: int,
    env: dict[str, str] | None = None,
    output_path: Path | None = None,
    preexec_fn=None,
) -> str:
    """Run a bounded lifecycle command and require a zero exit status."""
    result = _run_bounded(
        command,
        timeout=timeout,
        env=env,
        output_path=output_path,
        preexec_fn=preexec_fn,
    )
    if result.returncode != 0:
        raise ClusterCommandError(
            f"command failed with exit code {result.returncode}: "
            f"{' '.join(command)}\n{result.stdout}")
    return result.stdout


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
        # This is the teardown authority. It becomes false before pg_ctl start
        # is launched, because a timeout or nonzero exit can occur after the
        # postmaster detached from pg_ctl. Only a bounded process-state probe
        # may make it true again.
        self._shutdown_proven = True

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
        try:
            self._start()
        except BaseException as start_error:
            try:
                self.stop()
            except BaseException as stop_error:
                raise ClusterCommandError(
                    "private PostgreSQL startup failed and cleanup could not "
                    f"prove shutdown: startup={start_error}; "
                    f"shutdown={stop_error}"
                ) from start_error
            raise

    def _start(self) -> None:
        self.log_dir.mkdir(parents=True, exist_ok=True)
        init_timeout = _validated_timeout(
            self.inputs.init_command_timeout, "init-command-timeout")
        self._append_lifecycle(f"initializing data directory {self.data_dir}")
        lifecycle_env = sanitizer_descendant_environment(os.environ)
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
            env=lifecycle_env,
            output_path=self.run_dir / "initdb.log",
        )
        self._write_configuration()
        self._append_lifecycle(f"starting private node on port {self.port}")
        # pg_ctl may return or time out after the postmaster has detached.
        # Conservatively require stop() to prove the server absent from this
        # point onward, regardless of the pg_ctl start result.
        self._shutdown_proven = False
        _run_checked(
            [
                str(self._program("pg_ctl")),
                "--pgdata", str(self.data_dir),
                "--log", str(self.server_log),
                "--wait", "start",
            ],
            timeout=_validated_timeout(self.inputs.startup_timeout, "startup-timeout"),
            env=postgres_sanitizer_environment(lifecycle_env),
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
            env=lifecycle_env,
            output_path=self.run_dir / "createdb.log",
        )
        self._database_created = True
        for index, bootstrap in enumerate(self.inputs.bootstraps):
            label = (
                self.inputs.bootstrap_labels[index]
                if index < len(self.inputs.bootstrap_labels)
                else f"bootstrap_{index}"
            )
            self._bootstrap(bootstrap, label, lifecycle_env)

    def _bootstrap(
        self, bootstrap: Path, label: str, lifecycle_env: dict[str, str]
    ) -> None:
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
            env=lifecycle_env,
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
            # The core integration executable predates the generic private-PG
            # spelling below. Publish both exact ABIs from the same owned
            # values so an MCP server can run unit+integration inside this
            # cluster without a shell adapter or ambient connection fallback.
            "BIOSEQ_TEST_PGHOST": str(self.socket_dir),
            "BIOSEQ_TEST_PGPORT": str(self.port),
            "BIOSEQ_TEST_PGDATABASE": self.database,
            "BIOSEQ_TEST_PGUSER": self.database_user,
            "BIOSEQ_TEST_PG_HOST": str(self.socket_dir),
            "BIOSEQ_TEST_PG_PORT": str(self.port),
            "BIOSEQ_TEST_PG_DATABASE": self.database,
            "BIOSEQ_TEST_PG_USER": self.database_user,
            "BIOSEQ_TEST_PG_RUN_DIR": str(self.run_dir),
            "BIOSEQ_TEST_PG_DATA_DIR": str(self.data_dir),
        })
        return sanitizer_descendant_environment(env)

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

    def _postmaster_process_state(
        self,
        *,
        timeout: int,
        env: dict[str, str],
        output_path: Path,
    ) -> _PostmasterProcess:
        """Prove a live PID is this cluster's canonical staged postmaster."""
        pid_file = self.data_dir / "postmaster.pid"
        try:
            metadata = pid_file.lstat()
        except FileNotFoundError:
            return _PostmasterProcess(
                pid=None,
                is_live=False,
                ownership_proven=False,
                detail="no postmaster PID file",
            )
        except OSError as error:
            raise ClusterCommandError(
                f"cannot inspect postmaster PID file {pid_file}: {error}"
            ) from error
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
            raise ClusterCommandError(
                f"postmaster PID file is not a regular non-symlink file: "
                f"{pid_file}"
            )
        try:
            lines = pid_file.read_text(
                encoding="utf-8",
                errors="strict",
            ).splitlines()
            pid = int(lines[0])
        except (OSError, UnicodeError, IndexError, ValueError) as error:
            raise ClusterCommandError(
                f"postmaster PID file is malformed: {pid_file}"
            ) from error
        if pid <= 1:
            raise ClusterCommandError(
                f"postmaster PID file has an invalid PID {pid}: {pid_file}"
            )
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return _PostmasterProcess(
                pid=pid,
                is_live=False,
                ownership_proven=False,
                detail=f"postmaster PID {pid} is absent",
            )
        except PermissionError:
            pass
        except OSError as error:
            raise ClusterCommandError(
                f"cannot probe postmaster PID {pid}: {error}"
            ) from error

        try:
            declared_data_dir = Path(lines[1])
            expected_data_dir = self.data_dir.resolve(strict=True)
            if (
                not declared_data_dir.is_absolute()
                or declared_data_dir.resolve(strict=True)
                != expected_data_dir
            ):
                return _PostmasterProcess(
                    pid=pid,
                    is_live=True,
                    ownership_proven=False,
                    detail=(
                        f"live PID {pid} declares foreign data directory "
                        f"{declared_data_dir}"
                    ),
                )
        except (OSError, IndexError, RuntimeError) as error:
            return _PostmasterProcess(
                pid=pid,
                is_live=True,
                ownership_proven=False,
                detail=(
                    f"cannot validate data directory for live PID {pid}: "
                    f"{error}"
                ),
            )

        ps = shutil.which("ps", path=env.get("PATH"))
        if ps is None:
            return _PostmasterProcess(
                pid=pid,
                is_live=True,
                ownership_proven=False,
                detail="cannot locate ps for postmaster ownership proof",
            )
        try:
            result = _run_bounded(
                [ps, "-p", str(pid), "-o", "command="],
                timeout=min(timeout, 5),
                env=env,
                output_path=output_path,
            )
        except Exception as error:
            return _PostmasterProcess(
                pid=pid,
                is_live=True,
                ownership_proven=False,
                detail=f"postmaster command probe failed: {error}",
            )
        if result.returncode != 0:
            return _PostmasterProcess(
                pid=pid,
                is_live=True,
                ownership_proven=False,
                detail=(
                    "postmaster command probe returned "
                    f"{result.returncode}"
                ),
            )
        try:
            command = shlex.split(result.stdout.strip())
        except ValueError as error:
            return _PostmasterProcess(
                pid=pid,
                is_live=True,
                ownership_proven=False,
                detail=f"cannot parse postmaster command: {error}",
            )
        try:
            expected_postgres = self._program("postgres").resolve(
                strict=True)
            actual_postgres = Path(command[0])
            if (
                not actual_postgres.is_absolute()
                or actual_postgres.resolve(strict=True)
                != expected_postgres
            ):
                return _PostmasterProcess(
                    pid=pid,
                    is_live=True,
                    ownership_proven=False,
                    detail=(
                        f"live PID {pid} executable is not the staged "
                        f"postgres: {actual_postgres}"
                    ),
                )
        except (ClusterCommandError, IndexError, OSError) as error:
            return _PostmasterProcess(
                pid=pid,
                is_live=True,
                ownership_proven=False,
                detail=f"cannot validate postmaster executable: {error}",
            )

        command_data_dirs: list[Path] = []
        for index, argument in enumerate(command):
            if argument == "-D" and index + 1 < len(command):
                command_data_dirs.append(Path(command[index + 1]))
            elif argument.startswith("-D") and len(argument) > 2:
                command_data_dirs.append(Path(argument[2:]))
        try:
            owns_data_dir = any(
                candidate.is_absolute()
                and candidate.resolve(strict=True) == expected_data_dir
                for candidate in command_data_dirs
            )
        except OSError:
            owns_data_dir = False
        if not owns_data_dir:
            return _PostmasterProcess(
                pid=pid,
                is_live=True,
                ownership_proven=False,
                detail=(
                    f"live PID {pid} command does not contain -D "
                    f"{expected_data_dir}"
                ),
            )
        return _PostmasterProcess(
            pid=pid,
            is_live=True,
            ownership_proven=True,
            detail=(
                f"live PID {pid} is staged postgres with data directory "
                f"{expected_data_dir}"
            ),
        )

    def _probe_server_state(
        self,
        *,
        timeout: int,
        env: dict[str, str],
        output_path: Path,
    ) -> _ServerProbe:
        """Cross-check pg_ctl status with the postmaster PID's live state."""
        status_error: Exception | None = None
        result: _CommandResult | None = None
        try:
            result = _run_bounded(
                [
                    str(self._program("pg_ctl")),
                    "--pgdata", str(self.data_dir),
                    "status",
                ],
                timeout=timeout,
                env=env,
                output_path=output_path,
            )
        except Exception as error:
            status_error = error
        try:
            process = self._postmaster_process_state(
                timeout=timeout,
                env=env,
                output_path=output_path.with_name(
                    output_path.stem + "_process.log"),
            )
        except ClusterCommandError as error:
            return _ServerProbe(_SERVER_UNKNOWN, str(error))

        if status_error is not None:
            return _ServerProbe(
                _SERVER_UNKNOWN,
                f"pg_ctl status probe failed: {status_error}; "
                f"{process.detail}",
                not process.is_live or process.ownership_proven,
            )
        assert result is not None
        if (
            result.returncode == 0
            and process.is_live
            and process.ownership_proven
        ):
            return _ServerProbe(
                _SERVER_RUNNING,
                f"pg_ctl reports running; {process.detail}",
                True,
            )
        if result.returncode == 3 and not process.is_live:
            return _ServerProbe(
                _SERVER_STOPPED,
                f"pg_ctl reports stopped; {process.detail}",
                True,
            )
        return _ServerProbe(
            _SERVER_UNKNOWN,
            "inconsistent server probe: "
            f"pg_ctl exit={result.returncode}; {process.detail}",
            not process.is_live or process.ownership_proven,
        )

    def _attempt_stop(
        self,
        mode: str,
        *,
        timeout: int,
        env: dict[str, str],
        output_path: Path,
    ) -> None:
        _run_checked(
            [
                str(self._program("pg_ctl")),
                "--pgdata", str(self.data_dir),
                "--wait", f"--mode={mode}", "stop",
            ],
            timeout=timeout,
            env=env,
            output_path=output_path,
        )

    @staticmethod
    def _remove_owned_directory(path: Path) -> None:
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            return
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
            raise ClusterCommandError(
                f"refusing to remove non-directory lifecycle path: {path}"
            )
        shutil.rmtree(path)

    def stop(self) -> None:
        shutdown_timeout = _validated_timeout(
            self.inputs.shutdown_timeout, "shutdown-timeout")
        lifecycle_env = sanitizer_descendant_environment(os.environ)
        shutdown_errors: list[str] = []
        if not self._shutdown_proven or self._started:
            before = self._probe_server_state(
                timeout=shutdown_timeout,
                env=lifecycle_env,
                output_path=self.run_dir / "pg_ctl_status_before_stop.log",
            )
            self._append_lifecycle(
                f"server probe before stop: {before.state}: {before.detail}")
            if (
                before.state == _SERVER_RUNNING
                and self._database_created
            ):
                try:
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
                        env=lifecycle_env,
                        output_path=self.run_dir / "dropdb.log",
                    )
                except Exception as error:
                    shutdown_errors.append(f"dropdb failed: {error}")

            after_fast = before
            if before.state != _SERVER_STOPPED:
                if before.stop_is_safe:
                    try:
                        self._attempt_stop(
                            "fast",
                            timeout=shutdown_timeout,
                            env=lifecycle_env,
                            output_path=self.run_dir / "pg_ctl_stop.log",
                        )
                    except Exception as error:
                        shutdown_errors.append(
                            f"fast stop failed: {error}")
                else:
                    shutdown_errors.append(
                        "fast stop refused because postmaster ownership is "
                        f"unproven: {before.detail}")
                after_fast = self._probe_server_state(
                    timeout=shutdown_timeout,
                    env=lifecycle_env,
                    output_path=self.run_dir
                    / "pg_ctl_status_after_fast_stop.log",
                )
                self._append_lifecycle(
                    "server probe after fast stop: "
                    f"{after_fast.state}: {after_fast.detail}")

            final_probe = after_fast
            if after_fast.state != _SERVER_STOPPED:
                if after_fast.stop_is_safe:
                    try:
                        self._attempt_stop(
                            "immediate",
                            timeout=shutdown_timeout,
                            env=lifecycle_env,
                            output_path=self.run_dir
                            / "pg_ctl_stop_immediate.log",
                        )
                    except Exception as error:
                        shutdown_errors.append(
                            f"immediate stop failed: {error}")
                else:
                    shutdown_errors.append(
                        "immediate stop refused because postmaster "
                        "ownership is unproven: "
                        f"{after_fast.detail}")
                final_probe = self._probe_server_state(
                    timeout=shutdown_timeout,
                    env=lifecycle_env,
                    output_path=self.run_dir
                    / "pg_ctl_status_after_immediate_stop.log",
                )
                self._append_lifecycle(
                    "server probe after immediate stop: "
                    f"{final_probe.state}: {final_probe.detail}")

            if final_probe.state != _SERVER_STOPPED:
                details = "; ".join(shutdown_errors)
                if details:
                    details = "; stop errors: " + details
                self._append_lifecycle(
                    "shutdown is unproven; retaining private data and socket "
                    f"directories: {final_probe.detail}{details}")
                raise ClusterCommandError(
                    "private PostgreSQL shutdown is unproven; refusing "
                    "directory cleanup: "
                    f"{final_probe.detail}{details}"
                )
            self._shutdown_proven = True

        self._started = False
        self._database_created = False
        if shutdown_errors:
            self._append_lifecycle(
                "shutdown was proven after recoverable errors: "
                + "; ".join(shutdown_errors))
        self._copy_collector_logs()
        self._remove_owned_directory(self.data_dir)
        self._remove_owned_directory(self.socket_dir)
        if self.run_dir.exists():
            self._append_lifecycle(
                "private data and socket directories removed after "
                "shutdown proof")

    def __enter__(self) -> "UniquePrivateCluster":
        self.start()
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.stop()
