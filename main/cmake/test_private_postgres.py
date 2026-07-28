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
import os
from pathlib import Path
import subprocess
import sys
from unittest import mock

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))

from private_postgres import (  # noqa: E402
    ClusterCommandError,
    ClusterInputs,
    UniquePrivateCluster,
)
import private_postgres as private_postgres_module  # noqa: E402

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
    injected = {
        "BIOSEQ_TEST_BUILD_PRESET": "test-opt-asan",
        "BIOSEQ_TEST_BUILD_PRESET_DEBUG": "test-opt-asan",
        # Raw loader injection is hostile ambient state. A sanitizer run uses
        # the separately validated BIOSEQ_SANITIZER_RUNTIME selector instead.
        "DYLD_INSERT_LIBRARIES": "/tmp/untrusted-interposer.dylib",
        "ASAN_OPTIONS": "halt_on_error=1:abort_on_error=1",
        "ASAN_SYMBOLIZER_PATH": "/tmp/llvm-symbolizer",
        # Ambient connection aliases must be replaced by the owned cluster.
        "BIOSEQ_TEST_PGHOST": "/tmp/not-the-private-cluster",
        "BIOSEQ_TEST_PGPORT": "1",
        "BIOSEQ_TEST_PGDATABASE": "wrong_database",
        "BIOSEQ_TEST_PGUSER": "wrong_user",
    }
    previous = {name: os.environ.get(name) for name in injected}
    try:
        os.environ.update(injected)
        env = cluster.child_environment()
    finally:
        for name, value in previous.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value
        cluster.stop()
    assert env["PGHOST"] == str(cluster.socket_dir)
    assert env["PGPORT"] == str(cluster.port)
    assert env["PGDATABASE"] == "lifecycle_probe"
    assert env["PGUSER"] == cluster.database_user
    for banned in ("PGPASSWORD", "PGSERVICE", "PGHOSTADDR", "PGOPTIONS"):
        assert banned not in env, f"{banned} leaked into child environment"
    assert "DYLD_INSERT_LIBRARIES" not in env
    assert env["BIOSEQ_TEST_PGHOST"] == str(cluster.socket_dir)
    assert env["BIOSEQ_TEST_PGPORT"] == str(cluster.port)
    assert env["BIOSEQ_TEST_PGDATABASE"] == "lifecycle_probe"
    assert env["BIOSEQ_TEST_PGUSER"] == cluster.database_user
    assert env["BIOSEQ_TEST_PG_HOST"] == str(cluster.socket_dir)
    assert env["BIOSEQ_TEST_PG_DATA_DIR"] == str(cluster.data_dir)
    for name in (
            "BIOSEQ_TEST_BUILD_PRESET", "BIOSEQ_TEST_BUILD_PRESET_DEBUG",
            "ASAN_OPTIONS", "ASAN_SYMBOLIZER_PATH"):
        assert env[name] == injected[name], f"{name} was not preserved"


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


def _prepare_possible_live_cluster(
    bindir: Path,
    scratch: Path,
    label: str,
) -> UniquePrivateCluster:
    cluster = UniquePrivateCluster(_inputs(bindir, scratch, label))
    cluster.run_dir.mkdir(parents=True)
    cluster.data_dir.mkdir(parents=True)
    (cluster.data_dir / "postgresql.conf").write_text("")
    cluster._shutdown_proven = False
    return cluster


def test_start_error_after_launch_proves_shutdown_before_cleanup(
    bindir: Path,
    scratch: Path,
) -> None:
    cluster = UniquePrivateCluster(
        _inputs(bindir, scratch, "start_error_cleanup"))
    probes = [
        private_postgres_module._ServerProbe(
            private_postgres_module._SERVER_RUNNING,
            "detached postmaster is running",
            True,
        ),
        private_postgres_module._ServerProbe(
            private_postgres_module._SERVER_STOPPED,
            "postmaster is absent",
        ),
    ]

    def fake_run(command, **_kwargs):
        program = Path(command[0]).name
        if program == "initdb":
            cluster.data_dir.mkdir(parents=True)
            return ""
        if program == "pg_ctl" and "start" in command:
            assert not cluster._shutdown_proven
            raise ClusterCommandError(
                "pg_ctl start timed out after launching postmaster")
        raise AssertionError(f"unexpected command: {command}")

    with (
        mock.patch.object(
            private_postgres_module,
            "_run_checked",
            side_effect=fake_run,
        ),
        mock.patch.object(
            cluster,
            "_probe_server_state",
            side_effect=probes,
        ),
        mock.patch.object(cluster, "_attempt_stop") as attempt_stop,
    ):
        try:
            cluster.start()
        except ClusterCommandError as error:
            assert "timed out after launching" in str(error)
        else:
            raise AssertionError("pg_ctl start failure must propagate")

    attempt_stop.assert_called_once()
    assert attempt_stop.call_args.args == ("fast",)
    assert cluster._shutdown_proven
    assert not cluster.data_dir.exists()
    assert not cluster.socket_dir.exists()


def test_fast_failure_uses_immediate_and_requires_final_probe(
    bindir: Path,
    scratch: Path,
) -> None:
    cluster = _prepare_possible_live_cluster(
        bindir, scratch, "fast_then_immediate")
    probes = [
        private_postgres_module._ServerProbe(
            private_postgres_module._SERVER_RUNNING,
            "running before stop",
            True,
        ),
        private_postgres_module._ServerProbe(
            private_postgres_module._SERVER_RUNNING,
            "still running after fast stop",
            True,
        ),
        private_postgres_module._ServerProbe(
            private_postgres_module._SERVER_STOPPED,
            "absent after immediate stop",
        ),
    ]
    modes: list[str] = []

    def attempt(mode: str, **_kwargs) -> None:
        modes.append(mode)
        if mode == "fast":
            raise ClusterCommandError("fast stop timed out")

    with (
        mock.patch.object(
            cluster,
            "_probe_server_state",
            side_effect=probes,
        ),
        mock.patch.object(cluster, "_attempt_stop", side_effect=attempt),
    ):
        cluster.stop()

    assert modes == ["fast", "immediate"]
    assert cluster._shutdown_proven
    assert not cluster.data_dir.exists()
    assert not cluster.socket_dir.exists()


def test_unproven_shutdown_retains_owned_directories(
    bindir: Path,
    scratch: Path,
) -> None:
    cluster = _prepare_possible_live_cluster(
        bindir, scratch, "unproven_shutdown")
    unknown = private_postgres_module._ServerProbe(
        private_postgres_module._SERVER_UNKNOWN,
        "status probe cannot distinguish the postmaster",
        True,
    )
    modes: list[str] = []

    def failed_attempt(mode: str, **_kwargs) -> None:
        modes.append(mode)
        raise ClusterCommandError(f"{mode} stop failed")

    with (
        mock.patch.object(
            cluster,
            "_probe_server_state",
            side_effect=[unknown, unknown, unknown],
        ),
        mock.patch.object(
            cluster,
            "_attempt_stop",
            side_effect=failed_attempt,
        ),
    ):
        try:
            cluster.stop()
        except ClusterCommandError as error:
            assert "shutdown is unproven" in str(error)
        else:
            raise AssertionError("unproven shutdown must fail")

    assert modes == ["fast", "immediate"]
    assert not cluster._shutdown_proven
    assert cluster.data_dir.is_dir()
    assert cluster.socket_dir.is_dir()


def test_status_probe_cross_checks_live_pid(
    bindir: Path,
    scratch: Path,
) -> None:
    cluster = _prepare_possible_live_cluster(
        bindir, scratch, "status_pid_cross_check")
    stopped_result = private_postgres_module._CommandResult(3, "not running")
    with (
        mock.patch.object(
            private_postgres_module,
            "_run_bounded",
            return_value=stopped_result,
        ),
        mock.patch.object(
            cluster,
            "_postmaster_process_state",
            return_value=private_postgres_module._PostmasterProcess(
                pid=12345,
                is_live=True,
                ownership_proven=False,
                detail="live PID belongs to an unrelated process",
            ),
        ),
    ):
        probe = cluster._probe_server_state(
            timeout=1,
            env={},
            output_path=cluster.run_dir / "status.log",
        )
    assert probe.state == private_postgres_module._SERVER_UNKNOWN
    assert cluster.data_dir.is_dir()
    cluster._shutdown_proven = True
    cluster.stop()


def test_stale_live_pid_never_signals_unrelated_process(
    bindir: Path,
    scratch: Path,
) -> None:
    cluster = _prepare_possible_live_cluster(
        bindir, scratch, "stale_live_pid")
    sleeper = subprocess.Popen(
        [
            sys.executable,
            "-c",
            "import time; time.sleep(30)",
        ],
        start_new_session=True,
    )
    original_run_bounded = private_postgres_module._run_bounded

    def fake_status(command, **kwargs):
        if Path(command[0]).name == "pg_ctl":
            return private_postgres_module._CommandResult(
                0,
                "server is running",
            )
        return original_run_bounded(command, **kwargs)

    try:
        (cluster.data_dir / "postmaster.pid").write_text(
            f"{sleeper.pid}\n{cluster.data_dir.resolve()}\n",
            encoding="utf-8",
        )
        with (
            mock.patch.object(
                private_postgres_module,
                "_run_bounded",
                side_effect=fake_status,
            ),
            mock.patch.object(cluster, "_attempt_stop") as attempt_stop,
        ):
            try:
                cluster.stop()
            except ClusterCommandError as error:
                assert "ownership is unproven" in str(error)
            else:
                raise AssertionError(
                    "stale live PID must make shutdown unproven")
        attempt_stop.assert_not_called()
        assert sleeper.poll() is None, "unrelated process was signalled"
        assert cluster.data_dir.is_dir()
        assert cluster.socket_dir.is_dir()
    finally:
        sleeper.terminate()
        sleeper.wait(timeout=5)
        cluster._shutdown_proven = True
        cluster.stop()


TESTS = [
    test_basic_lifecycle_and_bootstrap,
    test_environment_allowlist,
    test_unique_allocation,
    test_bootstrap_timeout_is_bounded,
    test_bootstrap_failure_retains_logs,
    test_start_error_after_launch_proves_shutdown_before_cleanup,
    test_fast_failure_uses_immediate_and_requires_final_probe,
    test_unproven_shutdown_retains_owned_directories,
    test_status_probe_cross_checks_live_pid,
    test_stale_live_pid_never_signals_unrelated_process,
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
