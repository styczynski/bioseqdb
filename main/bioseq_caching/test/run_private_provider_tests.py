#!/usr/bin/env python3
"""Run bioseq_cache SQL tests in a disposable build-tree PostgreSQL cluster.

The runner never installs either extension and never touches the shared
development server. Every invocation owns a unique data directory, socket,
port, and database. Runner output and PostgreSQL logs remain under the selected
output root after data/socket cleanup.

The disposable-cluster lifecycle (unique data/socket/log directories, Unix-socket
startup, bootstrap, cleanup, signal forwarding) lives in the shared generic runner
`main/cmake/private_postgres.py`; this module is the cache-specific consumer that
maps cache inputs onto it and drives the bioseq_cache regression/TAP tools (§6).
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import importlib.util
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import uuid
from typing import Mapping, Sequence


def _load_generic_runner():
    """Import the shared generic private-cluster runner from main/cmake."""
    # test -> bioseq_caching -> main; the generic runner lives in main/cmake.
    runner_path = Path(__file__).resolve().parents[2] / "cmake" / "private_postgres.py"
    spec = importlib.util.spec_from_file_location(
        "bioseq_generic_private_postgres", runner_path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load generic private-cluster runner: {runner_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


_generic = _load_generic_runner()

# One shared error type and path validators across the generic lifecycle and this
# cache consumer, so `run` catches lifecycle failures and callers see one class.
ClusterCommandError = _generic.ClusterCommandError
_resolved_file = _generic._resolved_file
_resolved_dir = _generic._resolved_dir


def _run_checked(
    command: Sequence[str],
    *,
    env: Mapping[str, str] | None = None,
    output_path: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run one cache test-driver command (pg_regress/prove/psql), capturing output.

    Distinct from the generic lifecycle's `_run_checked`: the regression and TAP
    suites run without a wall-clock deadline, so this helper imposes none and
    returns the completed process for callers that read stdout.
    """
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


class UniquePrivateCluster(_generic.UniquePrivateCluster):
    """bioseq_cache consumer of the generic disposable-cluster lifecycle.

    The lifecycle (initdb/config/startup/bootstrap/cleanup) is inherited; only the
    cache-specific input mapping, the runner output directory, and the extension
    environment handed to the test tools are cache-owned.
    """

    def __init__(self, inputs: ClusterInputs) -> None:
        self.cache_inputs = inputs
        suffix = uuid.uuid4().hex[:10]
        library_dirs: list[Path] = [inputs.cache_dso.parent]
        bootstraps: list[Path] = [inputs.cache_bootstrap]
        bootstrap_labels: list[str] = ["bootstrap_cache"]
        if inputs.bioseq_dso is not None:
            library_dirs.append(inputs.bioseq_dso.parent)
        if inputs.bioseq_bootstrap is not None:
            bootstraps.append(inputs.bioseq_bootstrap)
            bootstrap_labels.append("bootstrap_provider")
        super().__init__(
            _generic.ClusterInputs(
                bindir=inputs.bindir,
                database=f"bioseq_cache_{os.getpid()}_{suffix}",
                config_template=inputs.config_template,
                output_root=inputs.output_root,
                run_label=inputs.kind,
                bootstraps=tuple(bootstraps),
                bootstrap_labels=tuple(bootstrap_labels),
                library_dirs=tuple(library_dirs),
                preload_libraries=("bioseq_cache",),
            )
        )
        self.runner_output_dir = self.run_dir / "runner"

    def start(self) -> None:
        super().start()
        self.runner_output_dir.mkdir(parents=True, exist_ok=True)

    def environment(self) -> dict[str, str]:
        """Full inherited environment plus the bioseq_cache extension handles.

        The test tools (pg_regress/prove) expect the ambient environment, unlike
        the generic §13.1 allowlist used for launched children; this preserves the
        cache tools' pre-existing contract.
        """
        env = os.environ.copy()
        env.update(
            {
                "PGHOST": str(self.socket_dir),
                "PGPORT": str(self.port),
                "PGUSER": self.database_user,
                "PGDATABASE": self.database,
                "BIOSEQ_CACHE_DSO": str(self.cache_inputs.cache_dso),
                "BIOSEQ_CACHE_LIBDIR": str(self.cache_inputs.cache_dso.parent),
                "BIOSEQ_CACHE_BOOTSTRAP_SQL": str(self.cache_inputs.cache_bootstrap),
                "BIOSEQ_CACHE_PRIVATE_RUN_DIR": str(self.run_dir),
            }
        )
        if self.cache_inputs.bioseq_dso is not None:
            env["BIOSEQ_DSO"] = str(self.cache_inputs.bioseq_dso)
            env["BIOSEQ_LIBDIR"] = str(self.cache_inputs.bioseq_dso.parent)
        if self.cache_inputs.bioseq_bootstrap is not None:
            env["BIOSEQ_BOOTSTRAP_SQL"] = str(self.cache_inputs.bioseq_bootstrap)
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
    tap_files = sorted(tap_dir.glob("*.pl"))
    if not tap_files:
        raise ClusterCommandError(f"no TAP test files found in {tap_dir}")
    pg_perl = (
        cluster.inputs.bindir.parent
        / "lib"
        / "pgxs"
        / "src"
        / "test"
        / "perl"
    )
    pg_regress = (
        cluster.inputs.bindir.parent
        / "lib"
        / "pgxs"
        / "src"
        / "test"
        / "regress"
        / "pg_regress"
    )
    if not pg_regress.is_file():
        raise ClusterCommandError(f"pg_regress is not a file: {pg_regress}")
    env = cluster.environment()
    env["PG_REGRESS"] = str(pg_regress)
    env["PATH"] = os.pathsep.join(
        part
        for part in (str(cluster.inputs.bindir), env.get("PATH", ""))
        if part
    )
    existing_perl5lib = env.get("PERL5LIB", "")
    if pg_perl.is_dir():
        env["PERL5LIB"] = os.pathsep.join(
            part for part in (str(pg_perl), existing_perl5lib) if part
        )
    perl = shutil.which("perl", path=env.get("PATH"))
    if perl is None:
        raise ClusterCommandError("Perl is required for PostgreSQL TAP tests")
    module_check = subprocess.run(
        [perl, "-MIPC::Run", "-MIO::Pty", "-e", "exit 0"],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    if module_check.returncode != 0:
        raise ClusterCommandError(
            "PostgreSQL TAP tests require Perl IPC::Run and IO::Pty "
            "(Ubuntu: libipc-run-perl and libio-pty-perl): "
            + module_check.stderr.strip()
        )
    command = [str(prove)]
    if pg_perl.is_dir():
        command.extend(["-I", str(pg_perl)])
    command.extend(str(tap_file) for tap_file in tap_files)
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
        # The lifecycle-owned base config; the cache adapter only supplies the
        # cache preload/library additions on top of it (§13.1).
        default=str(Path(__file__).resolve().parents[2]
                    / "cmake" / "private_postgresql.conf.in"),
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
