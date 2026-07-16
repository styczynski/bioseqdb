#!/usr/bin/env python3
"""Failure-path tests for the disposable PostgreSQL cluster owner."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


RUNNER_PATH = Path(__file__).with_name("run_private_cluster.py")
SPEC = importlib.util.spec_from_file_location("bioseq_cache_private_runner", RUNNER_PATH)
assert SPEC is not None and SPEC.loader is not None
RUNNER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)


class PrivateClusterCleanupTest(unittest.TestCase):
    def _inputs(self, root: Path) -> object:
        bindir = root / "bin"
        bindir.mkdir()
        for program in ("initdb", "pg_ctl", "createdb", "dropdb", "psql"):
            (bindir / program).write_text("test executable\n", encoding="utf-8")
        dso = root / "bioseq_cache.so"
        dso.write_bytes(b"test dso")
        bootstrap = root / "bootstrap.sql"
        bootstrap.write_text("SELECT 1;\n", encoding="utf-8")
        config = root / "postgresql.conf.in"
        config.write_text("listen_addresses = ''\n", encoding="utf-8")
        output = root / "output"
        output.mkdir()
        return RUNNER.ClusterInputs(
            bindir=bindir,
            cache_dso=dso,
            cache_bootstrap=bootstrap,
            config_template=config,
            output_root=output,
            kind="regress",
        )

    def test_bootstrap_failure_removes_private_state_but_retains_logs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cluster = RUNNER.UniquePrivateCluster(self._inputs(root))

            def fake_command(command, *, output_path=None, **_kwargs):
                if output_path is not None:
                    output_path.parent.mkdir(parents=True, exist_ok=True)
                    output_path.write_text("captured diagnostic\n", encoding="utf-8")
                program = Path(command[0]).name
                if program == "initdb":
                    cluster.data_dir.mkdir(parents=True)
                    (cluster.data_dir / "postgresql.conf").write_text(
                        "# generated\n", encoding="utf-8"
                    )
                if program == "psql":
                    raise RUNNER.ClusterCommandError("injected bootstrap failure")
                return ""

            with mock.patch.object(RUNNER._generic, "_run_checked", side_effect=fake_command):
                with self.assertRaises(RUNNER.ClusterCommandError):
                    cluster.start()
                cluster.stop()

            self.assertFalse(cluster.data_dir.exists())
            self.assertFalse(cluster.socket_dir.exists())
            self.assertTrue(cluster.run_dir.exists())
            self.assertTrue((cluster.run_dir / "bootstrap_cache.log").exists())
            self.assertTrue(cluster.lifecycle_log.exists())

    def test_runner_failure_always_executes_context_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cluster = RUNNER.UniquePrivateCluster(self._inputs(root))

            def fake_command(command, *, output_path=None, **_kwargs):
                if output_path is not None:
                    output_path.parent.mkdir(parents=True, exist_ok=True)
                    output_path.write_text("captured diagnostic\n", encoding="utf-8")
                if Path(command[0]).name == "initdb":
                    cluster.data_dir.mkdir(parents=True)
                    (cluster.data_dir / "postgresql.conf").write_text(
                        "# generated\n", encoding="utf-8"
                    )
                return ""

            with mock.patch.object(RUNNER._generic, "_run_checked", side_effect=fake_command):
                with self.assertRaisesRegex(RuntimeError, "injected runner failure"):
                    with cluster:
                        raise RuntimeError("injected runner failure")

            self.assertFalse(cluster.data_dir.exists())
            self.assertFalse(cluster.socket_dir.exists())
            self.assertTrue(cluster.run_dir.exists())
            self.assertTrue(cluster.lifecycle_log.exists())


if __name__ == "__main__":
    unittest.main()
