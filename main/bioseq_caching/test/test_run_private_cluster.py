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
        for program in (
            "initdb",
            "pg_ctl",
            "createdb",
            "dropdb",
            "psql",
            "pg_config",
            "postgres",
        ):
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

            def fake_command(command, *, env=None, output_path=None):
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
                return mock.Mock(returncode=0, stdout="")

            with mock.patch.object(RUNNER, "_run_checked", side_effect=fake_command):
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

            def fake_command(command, *, env=None, output_path=None):
                if output_path is not None:
                    output_path.parent.mkdir(parents=True, exist_ok=True)
                    output_path.write_text("captured diagnostic\n", encoding="utf-8")
                if Path(command[0]).name == "initdb":
                    cluster.data_dir.mkdir(parents=True)
                    (cluster.data_dir / "postgresql.conf").write_text(
                        "# generated\n", encoding="utf-8"
                    )
                return mock.Mock(returncode=0, stdout="")

            with mock.patch.object(RUNNER, "_run_checked", side_effect=fake_command):
                with self.assertRaisesRegex(RuntimeError, "injected runner failure"):
                    with cluster:
                        raise RuntimeError("injected runner failure")

            self.assertFalse(cluster.data_dir.exists())
            self.assertFalse(cluster.socket_dir.exists())
            self.assertTrue(cluster.run_dir.exists())
            self.assertTrue(cluster.lifecycle_log.exists())

    def test_no_cache_node_skips_preload_and_bootstrap(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = RUNNER.dataclasses.replace(
                self._inputs(root), load_cache=False
            )
            cluster = RUNNER.UniquePrivateCluster(inputs)
            commands: list[list[str]] = []

            def fake_command(command, *, env=None, output_path=None):
                commands.append(list(command))
                if output_path is not None:
                    output_path.parent.mkdir(parents=True, exist_ok=True)
                    output_path.write_text("captured diagnostic\n", encoding="utf-8")
                if Path(command[0]).name == "initdb":
                    cluster.data_dir.mkdir(parents=True)
                    (cluster.data_dir / "postgresql.conf").write_text(
                        "# generated\n", encoding="utf-8"
                    )
                return mock.Mock(returncode=0, stdout="")

            with mock.patch.object(RUNNER, "_run_checked", side_effect=fake_command):
                with cluster:
                    configuration = (
                        cluster.data_dir / "postgresql.conf"
                    ).read_text(encoding="utf-8")
                    self.assertNotIn("shared_preload_libraries", configuration)

            self.assertFalse(
                any(Path(command[0]).name == "psql" for command in commands)
            )
            self.assertFalse((cluster.run_dir / "bootstrap_cache.log").exists())

    def test_extension_package_requires_fixed_schema_and_guard(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            control = root / "bioseq_cache.control"
            control.write_text(
                "default_version = '1.0'\n"
                "relocatable = false\n"
                "schema = 'bioseq_cache'\n"
                "module_pathname = '$libdir/bioseq_cache'\n",
                encoding="utf-8",
            )
            install = root / "bioseq_cache--1.0.sql"
            install.write_text(
                '\\echo Use "CREATE EXTENSION bioseq_cache" to load this file. '
                "\\quit\nSELECT 'MODULE_PATHNAME';\n",
                encoding="utf-8",
            )
            bootstrap = root / "bootstrap.sql"
            bootstrap.write_text("SELECT '/tmp/bioseq_cache.so';\n", encoding="utf-8")

            RUNNER._validate_extension_package(control, install, bootstrap)

            control.write_text(
                "default_version = '1.0'\n"
                "relocatable = true\n"
                "module_pathname = '$libdir/bioseq_cache'\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                RUNNER.ClusterCommandError, "control contract is incomplete"
            ):
                RUNNER._validate_extension_package(control, install, bootstrap)

            control.write_text(
                "default_version = '1.0'\n"
                "relocatable = false\n"
                "schema = 'bioseq_cache'\n"
                "module_pathname = '$libdir/bioseq_cache'\n",
                encoding="utf-8",
            )
            install.write_text("SELECT 'MODULE_PATHNAME';\n", encoding="utf-8")
            with self.assertRaisesRegex(
                RUNNER.ClusterCommandError, "lacks its psql guard"
            ):
                RUNNER._validate_extension_package(control, install, bootstrap)

    def test_extension_overlay_is_private_and_contains_only_test_package(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self._inputs(root)
            sharedir = root / "installation" / "share"
            pkglibdir = root / "installation" / "lib"
            (sharedir / "extension").mkdir(parents=True)
            (sharedir / "timezonesets").mkdir()
            (sharedir / "timezonesets" / "Default").write_text(
                "timezone fixture\n", encoding="utf-8"
            )
            (sharedir / "extension" / "host_only.control").write_text(
                "host fixture\n", encoding="utf-8"
            )
            pkglibdir.mkdir(parents=True)
            (pkglibdir / "plpgsql.so").write_bytes(b"host fixture")

            control = root / "bioseq_cache.control"
            control.write_text("fixed control\n", encoding="utf-8")
            install = root / "bioseq_cache--1.0.sql"
            install.write_text("fixed install\n", encoding="utf-8")
            inputs = RUNNER.dataclasses.replace(
                inputs,
                extension_control=control,
                extension_script=install,
            )
            cluster = RUNNER.UniquePrivateCluster(inputs)
            cluster.run_dir.mkdir(parents=True)

            directories = {
                "--bindir": inputs.bindir,
                "--sharedir": sharedir,
                "--pkglibdir": pkglibdir,
            }

            def fake_pg_config(command, *, env=None, output_path=None):
                return mock.Mock(
                    returncode=0,
                    stdout=str(directories[command[1]]) + "\n",
                )

            with mock.patch.object(
                RUNNER, "_run_checked", side_effect=fake_pg_config
            ):
                cluster._prepare_extension_overlay()

            self.assertIsNotNone(cluster._extension_overlay_root)
            self.assertIsNotNone(cluster._overlay_postgres)
            assert cluster._extension_overlay_root is not None
            assert cluster._overlay_postgres is not None
            overlay = cluster._extension_overlay_root
            overlay_share = overlay / sharedir.relative_to(root)
            overlay_extension = overlay_share / "extension"
            self.assertTrue(cluster._overlay_postgres.is_file())
            self.assertNotEqual(cluster._overlay_postgres, inputs.bindir / "postgres")
            self.assertTrue((overlay_share / "timezonesets").is_symlink())
            self.assertEqual(
                sorted(path.name for path in overlay_extension.iterdir()),
                ["bioseq_cache--1.0.sql", "bioseq_cache.control"],
            )
            self.assertEqual(
                (overlay_extension / "bioseq_cache.control").read_text(
                    encoding="utf-8"
                ),
                "fixed control\n",
            )
            self.assertTrue(
                (sharedir / "extension" / "host_only.control").is_file()
            )

            cluster.stop()
            self.assertFalse(overlay.exists())

    def test_fixed_schema_proof_requires_real_postgresql_oracle(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self._inputs(root)
            control = root / "bioseq_cache.control"
            install = root / "bioseq_cache--1.0.sql"
            control.write_text("fixed control\n", encoding="utf-8")
            install.write_text("fixed install\n", encoding="utf-8")
            inputs = RUNNER.dataclasses.replace(
                inputs,
                extension_control=control,
                extension_script=install,
            )
            cluster = RUNNER.UniquePrivateCluster(inputs)
            cluster.run_dir.mkdir(parents=True)

            def fake_psql(sql, *, output_name):
                if "pg_available_extension_versions" in sql:
                    return "1.0|f|bioseq_cache\n"
                if "pg_extension" in sql:
                    return "0\n"
                return ""

            rejection = RUNNER.subprocess.CompletedProcess(
                args=["psql"],
                returncode=3,
                stdout=(
                    'ERROR:  0A000: extension "bioseq_cache" must be installed '
                    'in schema "bioseq_cache"\n'
                ),
            )
            with mock.patch.object(
                cluster, "psql", side_effect=fake_psql
            ), mock.patch.object(
                RUNNER,
                "_run_captured",
                return_value=rejection,
            ) as captured:
                cluster._prove_fixed_extension_schema()

            command = captured.call_args.args[0]
            self.assertIn(
                "CREATE EXTENSION bioseq_cache SCHEMA another_schema;", command
            )
            self.assertIn("VERBOSITY=verbose", command)
            self.assertIn("SQLSTATE 0A000", cluster.lifecycle_log.read_text())

            unrelated_failure = RUNNER.subprocess.CompletedProcess(
                args=["psql"],
                returncode=3,
                stdout="ERROR:  58P01: could not open extension control file\n",
            )
            with mock.patch.object(
                cluster, "psql", side_effect=fake_psql
            ), mock.patch.object(
                RUNNER,
                "_run_captured",
                return_value=unrelated_failure,
            ), self.assertRaisesRegex(
                RUNNER.ClusterCommandError,
                "did not produce the fixed-schema rejection oracle",
            ):
                cluster._prove_fixed_extension_schema()
            cluster.stop()


if __name__ == "__main__":
    unittest.main()
