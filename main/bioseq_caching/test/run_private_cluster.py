#!/usr/bin/env python3
"""Run bioseq_cache SQL tests in a disposable build-tree PostgreSQL cluster.

The runner never installs either extension and never touches the shared
development server. Every invocation owns a unique data directory, socket,
port, and database. Runner output and PostgreSQL logs remain under the selected
output root after data/socket cleanup.
"""

from __future__ import annotations

import argparse
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
from typing import Iterable, Mapping, Sequence


class ClusterCommandError(RuntimeError):
    """A cluster lifecycle or test command failed."""


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


def _postgresql_conf_literal(value: str) -> str:
    if "\n" in value or "\r" in value:
        raise ClusterCommandError("PostgreSQL configuration value contains a newline")
    return "'" + value.replace("'", "''") + "'"


def _available_local_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def _run_checked(
    command: Sequence[str],
    *,
    env: Mapping[str, str] | None = None,
    output_path: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        list(command),
        env=dict(env) if env is not None else None,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if output_path is not None:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        rendered = " ".join(command)
        raise ClusterCommandError(
            f"command failed with exit code {completed.returncode}: {rendered}\n"
            f"{completed.stdout}"
        )
    return completed


@dataclasses.dataclass(frozen=True)
class ClusterInputs:
    bindir: Path
    cache_dso: Path
    cache_bootstrap: Path
    config_template: Path
    output_root: Path
    kind: str
    bioseq_dso: Path | None = None
    bioseq_bootstrap: Path | None = None


class UniquePrivateCluster:
    """Lifecycle owner for one private PostgreSQL node and test database."""

    def __init__(self, inputs: ClusterInputs) -> None:
        self.inputs = inputs
        stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        suffix = uuid.uuid4().hex[:10]
        self.run_dir = inputs.output_root / f"{inputs.kind}_{stamp}_{os.getpid()}_{suffix}"
        self.data_dir = self.run_dir / "data"
        # AF_UNIX socket paths are capped at 107 bytes on Linux. Keep the socket
        # directory short even when the retained build-tree log path is deep.
        self.socket_dir = Path(
            tempfile.mkdtemp(prefix=f"bqc_{os.getpid()}_", dir=tempfile.gettempdir())
        )
        self.runner_output_dir = self.run_dir / "runner"
        self.server_log = self.run_dir / "server_startup.log"
        self.lifecycle_log = self.run_dir / "lifecycle.log"
        self.port = _available_local_port()
        self.database = f"bioseq_cache_{os.getpid()}_{suffix}"
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
        with self.lifecycle_log.open("a", encoding="utf-8") as stream:
            stream.write(f"{timestamp} {message}\n")

    def _write_configuration(self) -> None:
        base = self.inputs.config_template.read_text(encoding="utf-8")
        library_dirs = [self.inputs.cache_dso.parent]
        if self.inputs.bioseq_dso is not None:
            library_dirs.append(self.inputs.bioseq_dso.parent)
        unique_dirs = list(dict.fromkeys(str(path) for path in library_dirs))
        dynamic_path = os.pathsep.join([*unique_dirs, "$libdir"])
        additions = [
            "",
            "# Unique values appended by run_private_cluster.py",
            f"port = {self.port}",
            f"unix_socket_directories = {_postgresql_conf_literal(str(self.socket_dir))}",
            f"dynamic_library_path = {_postgresql_conf_literal(dynamic_path)}",
            "shared_preload_libraries = 'bioseq_cache'",
            "",
        ]
        with (self.data_dir / "postgresql.conf").open("a", encoding="utf-8") as stream:
            if base and not base.endswith("\n"):
                base += "\n"
            stream.write(base)
            stream.write("\n".join(additions))

    def start(self) -> None:
        self.run_dir.mkdir(parents=True, exist_ok=False)
        self.runner_output_dir.mkdir()
        self._append_lifecycle(f"initializing data directory {self.data_dir}")
        _run_checked(
            [
                str(self._program("initdb")),
                "--pgdata",
                str(self.data_dir),
                "--username",
                self.database_user,
                "--auth-local=trust",
                "--auth-host=reject",
                "--encoding=UTF8",
                "--no-locale",
                "--no-sync",
            ],
            output_path=self.run_dir / "initdb.log",
        )
        self._write_configuration()
        self._append_lifecycle(f"starting private node on port {self.port}")
        _run_checked(
            [
                str(self._program("pg_ctl")),
                "--pgdata",
                str(self.data_dir),
                "--log",
                str(self.server_log),
                "--wait",
                "start",
            ],
            output_path=self.run_dir / "pg_ctl_start.log",
        )
        self._started = True
        _run_checked(
            [
                str(self._program("createdb")),
                "--host",
                str(self.socket_dir),
                "--port",
                str(self.port),
                "--username",
                self.database_user,
                self.database,
            ],
            output_path=self.run_dir / "createdb.log",
        )
        self._database_created = True
        self._bootstrap(self.inputs.cache_bootstrap, "cache")
        if self.inputs.bioseq_bootstrap is not None:
            self._bootstrap(self.inputs.bioseq_bootstrap, "provider")

    def _bootstrap(self, bootstrap: Path, label: str) -> None:
        self._append_lifecycle(f"executing {label} bootstrap {bootstrap}")
        _run_checked(
            [
                str(self._program("psql")),
                "-X",
                "-v",
                "ON_ERROR_STOP=1",
                "--host",
                str(self.socket_dir),
                "--port",
                str(self.port),
                "--username",
                self.database_user,
                "--dbname",
                self.database,
                "--file",
                str(bootstrap),
            ],
            output_path=self.run_dir / f"bootstrap_{label}.log",
        )

    def environment(self) -> dict[str, str]:
        env = os.environ.copy()
        env.update(
            {
                "PGHOST": str(self.socket_dir),
                "PGPORT": str(self.port),
                "PGUSER": self.database_user,
                "PGDATABASE": self.database,
                "BIOSEQ_CACHE_DSO": str(self.inputs.cache_dso),
                "BIOSEQ_CACHE_LIBDIR": str(self.inputs.cache_dso.parent),
                "BIOSEQ_CACHE_BOOTSTRAP_SQL": str(self.inputs.cache_bootstrap),
                "BIOSEQ_CACHE_PRIVATE_RUN_DIR": str(self.run_dir),
            }
        )
        if self.inputs.bioseq_dso is not None:
            env["BIOSEQ_DSO"] = str(self.inputs.bioseq_dso)
            env["BIOSEQ_LIBDIR"] = str(self.inputs.bioseq_dso.parent)
        if self.inputs.bioseq_bootstrap is not None:
            env["BIOSEQ_BOOTSTRAP_SQL"] = str(self.inputs.bioseq_bootstrap)
        return env

    def psql(self, sql: str, *, output_name: str) -> str:
        completed = _run_checked(
            [
                str(self._program("psql")),
                "-XAt",
                "-v",
                "ON_ERROR_STOP=1",
                "--host",
                str(self.socket_dir),
                "--port",
                str(self.port),
                "--username",
                self.database_user,
                "--dbname",
                self.database,
                "--command",
                sql,
            ],
            output_path=self.run_dir / output_name,
        )
        return completed.stdout

    def _copy_collector_logs(self) -> None:
        source = self.data_dir / "pg_log"
        destination = self.run_dir / "postgresql_logs"
        if source.is_dir():
            destination.mkdir(exist_ok=True)
            for log_file in source.iterdir():
                if log_file.is_file():
                    shutil.copy2(log_file, destination / log_file.name)

    def stop(self) -> None:
        if self._started and self._database_created:
            with contextlib.suppress(Exception):
                _run_checked(
                    [
                        str(self._program("dropdb")),
                        "--if-exists",
                        "--host",
                        str(self.socket_dir),
                        "--port",
                        str(self.port),
                        "--username",
                        self.database_user,
                        self.database,
                    ],
                    output_path=self.run_dir / "dropdb.log",
                )
        if self._started:
            try:
                _run_checked(
                    [
                        str(self._program("pg_ctl")),
                        "--pgdata",
                        str(self.data_dir),
                        "--wait",
                        "--mode=fast",
                        "stop",
                    ],
                    output_path=self.run_dir / "pg_ctl_stop.log",
                )
            except Exception:
                with contextlib.suppress(Exception):
                    _run_checked(
                        [
                            str(self._program("pg_ctl")),
                            "--pgdata",
                            str(self.data_dir),
                            "--wait",
                            "--mode=immediate",
                            "stop",
                        ],
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


def _run_regression_tool(
    cluster: UniquePrivateCluster,
    *,
    runner: Path,
    input_dir: Path,
    schedule: Path,
    label: str,
) -> None:
    output_dir = cluster.runner_output_dir / label
    output_dir.mkdir(parents=True, exist_ok=True)
    command = [
        str(runner),
        "--use-existing",
        f"--dbname={cluster.database}",
        f"--bindir={cluster.inputs.bindir}",
        f"--inputdir={input_dir}",
        f"--outputdir={output_dir}",
        f"--schedule={schedule}",
    ]
    _run_checked(
        command,
        env=cluster.environment(),
        output_path=cluster.run_dir / f"{label}.log",
    )


def _run_prove(
    cluster: UniquePrivateCluster,
    *,
    prove: Path,
    tap_dir: Path,
) -> None:
    pg_perl = (
        cluster.inputs.bindir.parent
        / "lib"
        / "pgxs"
        / "src"
        / "test"
        / "perl"
    )
    env = cluster.environment()
    existing_perl5lib = env.get("PERL5LIB", "")
    if pg_perl.is_dir():
        env["PERL5LIB"] = os.pathsep.join(
            part for part in (str(pg_perl), existing_perl5lib) if part
        )
    command = [str(prove)]
    if pg_perl.is_dir():
        command.extend(["-I", str(pg_perl)])
    command.extend(["-r", str(tap_dir)])
    _run_checked(
        command,
        env=env,
        output_path=cluster.run_dir / "tap.log",
    )


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--kind",
        required=True,
        choices=("regress", "isolation", "tap", "bioseq-provider"),
    )
    parser.add_argument("--runner")
    parser.add_argument("--bindir", required=True)
    parser.add_argument("--dso", required=True)
    parser.add_argument("--bioseq-dso")
    parser.add_argument("--bootstrap", required=True)
    parser.add_argument("--bioseq-bootstrap")
    parser.add_argument(
        "--config",
        default=str(Path(__file__).with_name("postgresql.conf.in")),
    )
    parser.add_argument("--inputdir")
    parser.add_argument("--schedule")
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--pg-regress")
    parser.add_argument("--pg-isolation-regress")
    parser.add_argument("--prove")
    return parser.parse_args(argv)


def _inputs_from_args(args: argparse.Namespace) -> ClusterInputs:
    bioseq_dso = (
        _resolved_file(args.bioseq_dso, "Bioseq DSO") if args.bioseq_dso else None
    )
    bioseq_bootstrap = (
        _resolved_file(args.bioseq_bootstrap, "Bioseq bootstrap")
        if args.bioseq_bootstrap
        else None
    )
    if (bioseq_dso is None) != (bioseq_bootstrap is None):
        raise ClusterCommandError(
            "--bioseq-dso and --bioseq-bootstrap must be supplied together"
        )
    output_root = Path(args.output_root).expanduser().resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    return ClusterInputs(
        bindir=_resolved_dir(args.bindir, "PostgreSQL bindir"),
        cache_dso=_resolved_file(args.dso, "cache DSO"),
        cache_bootstrap=_resolved_file(args.bootstrap, "cache bootstrap"),
        config_template=_resolved_file(args.config, "PostgreSQL config template"),
        output_root=output_root,
        kind=args.kind,
        bioseq_dso=bioseq_dso,
        bioseq_bootstrap=bioseq_bootstrap,
    )


def _require_argument(args: argparse.Namespace, name: str) -> str:
    value = getattr(args, name)
    if not value:
        raise ClusterCommandError(f"--{name.replace('_', '-')} is required for {args.kind}")
    return str(value)


def run(argv: Sequence[str]) -> int:
    args = _parse_args(argv)
    inputs = _inputs_from_args(args)
    cluster = UniquePrivateCluster(inputs)
    try:
        with cluster:
            if args.kind in ("regress", "isolation"):
                runner = _resolved_file(_require_argument(args, "runner"), "test runner")
                input_dir = _resolved_dir(
                    _require_argument(args, "inputdir"), "test input directory"
                )
                schedule = _resolved_file(
                    _require_argument(args, "schedule"), "test schedule"
                )
                _run_regression_tool(
                    cluster,
                    runner=runner,
                    input_dir=input_dir,
                    schedule=schedule,
                    label=args.kind,
                )
            elif args.kind == "tap":
                prove = _resolved_file(_require_argument(args, "prove"), "prove")
                tap_dir = _resolved_dir(
                    _require_argument(args, "inputdir"), "TAP directory"
                )
                _run_prove(cluster, prove=prove, tap_dir=tap_dir)
            else:
                if inputs.bioseq_dso is None:
                    raise ClusterCommandError(
                        "bioseq-provider requires both Bioseq DSO/bootstrap inputs"
                    )
                input_dir = _resolved_dir(
                    _require_argument(args, "inputdir"), "provider test directory"
                )
                pg_regress = _resolved_file(
                    _require_argument(args, "pg_regress"), "pg_regress"
                )
                pg_isolation = _resolved_file(
                    _require_argument(args, "pg_isolation_regress"),
                    "pg_isolation_regress",
                )
                prove = _resolved_file(_require_argument(args, "prove"), "prove")
                _run_regression_tool(
                    cluster,
                    runner=pg_regress,
                    input_dir=input_dir,
                    schedule=input_dir / "provider_regress_schedule",
                    label="provider_regress",
                )
                _run_regression_tool(
                    cluster,
                    runner=pg_isolation,
                    input_dir=input_dir,
                    schedule=input_dir / "provider_isolation_schedule",
                    label="provider_isolation",
                )
                _run_prove(cluster, prove=prove, tap_dir=input_dir / "tap")
        print(f"bioseq_cache private run passed; logs: {cluster.run_dir}")
        return 0
    except (ClusterCommandError, OSError) as error:
        with contextlib.suppress(Exception):
            cluster.stop()
        print(f"bioseq_cache private run failed: {error}", file=sys.stderr)
        print(f"retained logs: {cluster.run_dir}", file=sys.stderr)
        return 1


def main() -> None:
    previous_handlers: dict[int, object] = {}

    def _interrupt(signum: int, frame: object) -> None:
        raise KeyboardInterrupt(f"received signal {signum}")

    for signum in (signal.SIGINT, signal.SIGTERM):
        previous_handlers[signum] = signal.signal(signum, _interrupt)
    try:
        raise SystemExit(run(sys.argv[1:]))
    except KeyboardInterrupt as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(130) from error
    finally:
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)


if __name__ == "__main__":
    main()
