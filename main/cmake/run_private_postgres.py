#!/usr/bin/env python3
"""Generic one-child private PostgreSQL runner (spec section 13.1).

Starts a unique disposable cluster, creates the requested database, applies each
bootstrap in order, then launches exactly one child (everything after `--`) with
a strict connection-environment allowlist pointing at the private Unix socket.
Signals are forwarded to the child's process group and the owned server is always
stopped. Website generation invokes this runner; the caching provider uses its
own adapter over the same lifecycle module.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import signal
import sys
from typing import Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))

from private_postgres import (  # noqa: E402
    ClusterCommandError,
    ClusterInputs,
    UniquePrivateCluster,
    _resolved_dir,
    _resolved_file,
    _validated_identifier,
    _validated_timeout,
)


def _split_child(argv: Sequence[str]) -> tuple[list[str], list[str]]:
    argv = list(argv)
    if "--" not in argv:
        raise ClusterCommandError("missing '--' separator before the child command")
    index = argv.index("--")
    return argv[:index], argv[index + 1:]


def _parse_args(runner_argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pg-bin-dir", required=True)
    parser.add_argument("--data-dir-parent")
    parser.add_argument("--socket-dir-parent")
    parser.add_argument("--log-dir")
    parser.add_argument("--database", required=True)
    parser.add_argument("--bootstrap", action="append", default=[])
    parser.add_argument("--library-dir", action="append", default=[])
    parser.add_argument("--preload-library", action="append", default=[])
    parser.add_argument("--disable-prepared-transactions", action="store_true")
    parser.add_argument(
        "--postgresql-setting", action="append", default=[],
        help=argparse.SUPPRESS)  # compatibility: one fail-closed value only
    parser.add_argument("--init-command-timeout", type=int, default=60)
    parser.add_argument("--bootstrap-timeout", type=int, default=60)
    parser.add_argument("--startup-timeout", type=int, default=60)
    parser.add_argument("--shutdown-timeout", type=int, default=60)
    parser.add_argument("--address-space-limit-mib", type=int, default=None)
    parser.add_argument("--run-label", default="private_pg")
    return parser.parse_args(list(runner_argv))


def _inputs_from_args(args: argparse.Namespace) -> ClusterInputs:
    bindir = _resolved_dir(args.pg_bin_dir, "pg-bin-dir")
    if any(value != "max_prepared_transactions=0"
           for value in args.postgresql_setting):
        raise ClusterCommandError(
            "--postgresql-setting only accepts max_prepared_transactions=0; "
            "use a dedicated lifecycle option for any future override")
    log_dir: Path | None = None
    if args.log_dir:
        log_dir = Path(args.log_dir).expanduser().resolve()
        log_dir.mkdir(parents=True, exist_ok=True)
    return ClusterInputs(
        bindir=bindir,
        database=_validated_identifier(args.database, "database"),
        config_template=_resolved_file(
            str(Path(__file__).resolve().parent / "private_postgresql.conf.in"),
            "config template"),
        output_root=(log_dir or bindir.parent),
        run_label=args.run_label,
        bootstraps=tuple(
            _resolved_file(path, "bootstrap") for path in args.bootstrap),
        library_dirs=tuple(
            _resolved_dir(path, "library-dir") for path in args.library_dir),
        preload_libraries=tuple(args.preload_library),
        disable_prepared_transactions=(
            args.disable_prepared_transactions or bool(args.postgresql_setting)),
        data_dir_parent=(
            Path(args.data_dir_parent).expanduser().resolve()
            if args.data_dir_parent else None),
        socket_dir_parent=(
            Path(args.socket_dir_parent).expanduser().resolve()
            if args.socket_dir_parent else None),
        log_dir=log_dir,
        init_command_timeout=_validated_timeout(
            args.init_command_timeout, "init-command-timeout"),
        bootstrap_timeout=_validated_timeout(
            args.bootstrap_timeout, "bootstrap-timeout"),
        startup_timeout=_validated_timeout(args.startup_timeout, "startup-timeout"),
        shutdown_timeout=_validated_timeout(args.shutdown_timeout, "shutdown-timeout"),
        address_space_limit_mib=(
            None if args.address_space_limit_mib is None
            else int(args.address_space_limit_mib)),
    )


def run(argv: Sequence[str]) -> int:
    runner_argv, child = _split_child(argv)
    if not child:
        raise ClusterCommandError("no child command supplied after '--'")
    inputs = _inputs_from_args(_parse_args(runner_argv))
    cluster = UniquePrivateCluster(inputs)
    cluster.start()
    try:
        return cluster.run_child(child)
    finally:
        cluster.stop()


def main() -> None:
    def _interrupt(signum: int, frame: object) -> None:  # noqa: ANN001
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, _interrupt)
    try:
        sys.exit(run(sys.argv[1:]))
    except ClusterCommandError as error:
        print(f"run_private_postgres: {error}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
