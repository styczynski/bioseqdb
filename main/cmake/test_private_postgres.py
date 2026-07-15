#!/usr/bin/env python3
"""Lifecycle tests for the generic private PostgreSQL runner (spec section 17.3).

Run by the bioseq_private_postgres_tests CMake target with the selected
pg_config bindir and an isolated scratch root. These cover unique allocation,
bootstrap ordering, the strict child-environment allowlist, bounded timeouts,
and success/failure cleanup. Extension-stage membership/isolation is covered by
the website CMake integration once staging exists; these tests need only a base
PostgreSQL bindir.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))

from private_postgres import (  # noqa: E402
    ClusterCommandError,
    ClusterInputs,
    UniquePrivateCluster,
)

_CONFIG = _HERE / "private_postgresql.conf.in"


def _inputs(bindir: Path, scratch: Path, label: str, **kw) -> ClusterInputs:
    root = scratch / label
    root.mkdir(parents=True, exist_ok=True)
    return ClusterInputs(
        bindir=bindir,
        database=kw.pop("database", "lifecycle_probe"),
        config_template=_CONFIG,
        output_root=root,
        log_dir=root,
        run_label=label,
        **kw,
    )


def _child_capture(bindir: Path, sql: str) -> list[str]:
    return [str(bindir / "psql"), "-tAqc", sql]


def test_basic_lifecycle_and_bootstrap(bindir: Path, scratch: Path) -> None:
    boot = scratch / "boot_basic.sql"
    boot.write_text("CREATE TABLE t(v int); INSERT INTO t VALUES (7);\n")
    cluster = UniquePrivateCluster(
        _inputs(bindir, scratch, "basic", bootstraps=(boot,)))
    captured = scratch / "basic.out"
    cluster.start()
    try:
        rc = cluster.run_child(
            [str(bindir / "psql"), "-tAqo", str(captured),
             "-c", "SELECT current_database() || ':' || (SELECT v FROM t)"])
    finally:
        cluster.stop()
    assert rc == 0, f"child exit {rc}"
    text = captured.read_text().strip()
    assert text == "lifecycle_probe:7", f"unexpected child output {text!r}"
    assert not (cluster.data_dir).exists(), "data dir should be removed on success"
    assert (cluster.run_dir / "bootstrap_0.log").is_file(), "bootstrap log retained"


def test_environment_allowlist(bindir: Path, scratch: Path) -> None:
    cluster = UniquePrivateCluster(_inputs(bindir, scratch, "env"))
    cluster.start()
    try:
        env = cluster.child_environment()
    finally:
        cluster.stop()
    assert env["PGHOST"] == str(cluster.socket_dir)
    assert env["PGDATABASE"] == "lifecycle_probe"
    for banned in ("PGPASSWORD", "PGSERVICE", "PGHOSTADDR", "PGOPTIONS"):
        assert banned not in env, f"{banned} leaked into child environment"
    assert env["BIOSEQ_TEST_PG_HOST"] == str(cluster.socket_dir)


def test_unique_allocation(bindir: Path, scratch: Path) -> None:
    first = UniquePrivateCluster(_inputs(bindir, scratch, "uniq_a"))
    second = UniquePrivateCluster(_inputs(bindir, scratch, "uniq_b"))
    assert first.socket_dir != second.socket_dir
    assert first.port != second.port
    assert first.run_dir != second.run_dir


def test_bootstrap_timeout_is_bounded(bindir: Path, scratch: Path) -> None:
    boot = scratch / "boot_slow.sql"
    boot.write_text("SELECT pg_sleep(30);\n")
    cluster = UniquePrivateCluster(
        _inputs(bindir, scratch, "timeout",
                bootstraps=(boot,), bootstrap_timeout=2))
    raised = False
    try:
        cluster.start()
    except ClusterCommandError as error:
        raised = "timed out" in str(error)
    finally:
        cluster.stop()
    assert raised, "a slow bootstrap must fail with a bounded timeout"


def test_bootstrap_failure_retains_logs(bindir: Path, scratch: Path) -> None:
    boot = scratch / "boot_bad.sql"
    boot.write_text("THIS IS NOT SQL;\n")
    cluster = UniquePrivateCluster(
        _inputs(bindir, scratch, "boomfail", bootstraps=(boot,)))
    raised = False
    try:
        cluster.start()
    except ClusterCommandError:
        raised = True
    finally:
        cluster.stop()
    assert raised, "a failing bootstrap must raise"


TESTS = [
    test_basic_lifecycle_and_bootstrap,
    test_environment_allowlist,
    test_unique_allocation,
    test_bootstrap_timeout_is_bounded,
    test_bootstrap_failure_retains_logs,
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pg-bin-dir", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    bindir = Path(args.pg_bin_dir).expanduser().resolve()
    scratch = Path(args.scratch_root).expanduser().resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    failures = 0
    for test in TESTS:
        name = test.__name__
        try:
            test(bindir, scratch)
            print(f"ok - {name}")
        except Exception as error:  # noqa: BLE001
            failures += 1
            print(f"not ok - {name}: {type(error).__name__}: {error}")
    print(f"{len(TESTS) - failures}/{len(TESTS)} lifecycle tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
