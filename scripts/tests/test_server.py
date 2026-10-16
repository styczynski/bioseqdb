"""Focused unit tests for the safe local benchmark-e2e config override."""

from pathlib import Path
import copy
import hashlib
import importlib.util
import io
import json
import os
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest import mock


BUILD_MCP_DIR = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = BUILD_MCP_DIR.parent
sys.path.insert(0, str(BUILD_MCP_DIR))

import build_presets
import build_worker
import cache_modes
import doxygen_toolchain
import integrity
import postgres_runtime
import process_state
import q1024_watchdog
import protocol
import remote
import server as build_server
from docs import qmd_manager
from docs.qmd_manager import QmdManager


class DetachedWorkerArgumentContractTests(unittest.TestCase):
    def test_detached_worker_uses_the_declared_benchmark_config_destination(self):
        arguments = [
            "server.py",
            "--run-build",
            "unit-integration",
            "--benchmark-e2e-config",
            "bench/configuration.json",
        ]
        with mock.patch.object(sys, "argv", arguments), mock.patch.object(
            build_server, "_run_build_worker", return_value=0,
        ) as run_worker:
            self.assertEqual(build_server.main(), 0)
        self.assertEqual(
            run_worker.call_args.kwargs["bench_e2e_config"],
            "bench/configuration.json",
        )

    def test_inline_worker_uses_the_declared_benchmark_config_destination(self):
        arguments = [
            "server.py",
            "--run-build-sync",
            "unit-integration",
            "--benchmark-e2e-config",
            "bench/configuration.json",
        ]
        with mock.patch.object(sys, "argv", arguments), mock.patch.object(
            build_server, "_run_build_inline", return_value=0,
        ) as run_worker:
            self.assertEqual(build_server.main(), 0)
        self.assertEqual(
            run_worker.call_args.kwargs["bench_e2e_config"],
            "bench/configuration.json",
        )


class NativeClang22PresetPortabilityTests(unittest.TestCase):
    def test_native_release_and_debug_presets_map_to_isolated_e2e_trees(self):
        expected = {
            "native-arm64-clang22-linux":
                "native-arm64-clang22-linux-e2e",
            "native-arm64-clang22-linux-debug":
                "native-arm64-clang22-linux-e2e-debug",
            "native-x64-clang22-linux":
                "native-x64-clang22-linux-e2e",
            "native-x64-clang22-linux-debug":
                "native-x64-clang22-linux-e2e-debug",
        }
        for production, e2e in expected.items():
            with self.subTest(production=production):
                self.assertEqual(
                    build_presets._default_e2e_preset(production), e2e)

    def test_native_presets_are_aws_runtime_hosts_without_x64_relabel(self):
        for preset in (
            "native-arm64-clang22-linux",
            "native-arm64-clang22-linux-e2e",
            "test-native-x64-clang22-linux",
            "native-x64-clang22-linux-e2e-debug",
        ):
            with self.subTest(preset=preset):
                self.assertTrue(build_presets._is_aws_preset(preset))
                self.assertFalse(build_presets._is_aws_clang_preset(preset))

    def test_release_e2e_host_contract_is_closed_and_architecture_exact(self):
        self.assertEqual(
            build_presets._release_clang_e2e_preset_host_contract(
                "bench-e2e-opt"),
            ("Darwin", "arm64", None, None),
        )
        self.assertEqual(
            build_presets._release_clang_e2e_preset_host_contract(
                "aws-x64-linux-e2e"),
            ("Linux", "x86_64", "x64", "x64-linux-libcxx"),
        )
        self.assertEqual(
            build_presets._release_clang_e2e_preset_host_contract(
                "native-arm64-clang22-linux-e2e"),
            ("Linux", "aarch64", "arm64", "arm64-linux-libcxx"),
        )
        self.assertEqual(
            build_presets._release_clang_e2e_preset_host_contract(
                "native-x64-clang22-linux-e2e"),
            ("Linux", "x86_64", "x64", "x64-linux-libcxx"),
        )
        for rejected in (
            "aws-x64-linux-gcc161-e2e",
            "bench-e2e-opt-debug",
            "native-arm64-clang22-linux-e2e-debug",
            "native-arm64-clang23-linux-e2e",
            "invented-e2e",
        ):
            with self.subTest(rejected=rejected):
                self.assertIsNone(
                    build_presets._release_clang_e2e_preset_host_contract(
                        rejected))


class DocumentActivityContractTests(unittest.TestCase):
    def test_qmd_collections_recreate_stale_root_lift_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            current = root / "DOCS/CURRENT"
            archive = root / "DOCS/ARCHIVE"
            collections = {
                qmd_manager.COL_CURRENT: {
                    "path": root / "main/DOCS/CURRENT",
                    "pattern": qmd_manager.COLLECTION_MASK,
                    "include": True,
                },
                qmd_manager.COL_ARCHIVE: {
                    "path": archive,
                    "pattern": qmd_manager.COLLECTION_MASK,
                    "include": False,
                },
            }
            commands: list[tuple[str, ...]] = []

            def cli(arguments, timeout=1800.0):
                del timeout
                command = tuple(arguments)
                commands.append(command)
                if command[:2] == ("collection", "show"):
                    value = collections.get(command[2])
                    if value is None:
                        return subprocess.CompletedProcess(command, 1, "", "missing")
                    included = "yes (default)" if value["include"] else "no"
                    return subprocess.CompletedProcess(
                        command,
                        0,
                        (
                            f"Collection: {command[2]}\n"
                            f"  Path:     {value['path']}\n"
                            f"  Pattern:  {value['pattern']}\n"
                            f"  Include:  {included}\n"
                        ),
                        "",
                    )
                if command[:2] == ("collection", "remove"):
                    collections.pop(command[2])
                elif command[:2] == ("collection", "add"):
                    collections[command[4]] = {
                        "path": Path(command[2]),
                        "pattern": command[6],
                        "include": True,
                    }
                elif command[:2] in {
                    ("collection", "include"),
                    ("collection", "exclude"),
                }:
                    collections[command[2]]["include"] = command[1] == "include"
                return subprocess.CompletedProcess(command, 0, "", "")

            manager = QmdManager()
            with mock.patch.object(
                qmd_manager.registry, "CURRENT_ROOT", current,
            ), mock.patch.object(
                qmd_manager.registry, "ARCHIVE_ROOT", archive,
            ), mock.patch.object(manager, "_cli", side_effect=cli):
                manager._ensure_collections()

        canonical_current = current.resolve()
        self.assertEqual(
            collections[qmd_manager.COL_CURRENT]["path"],
            canonical_current,
        )
        self.assertEqual(
            collections[qmd_manager.COL_CURRENT]["pattern"],
            qmd_manager.COLLECTION_MASK,
        )
        remove_command = (
            "collection", "remove", qmd_manager.COL_CURRENT)
        add_command = (
            "collection", "add", str(canonical_current), "--name",
            qmd_manager.COL_CURRENT, "--mask", qmd_manager.COLLECTION_MASK,
        )
        self.assertIn(remove_command, commands)
        self.assertIn(add_command, commands)
        self.assertLess(commands.index(remove_command), commands.index(add_command))
        self.assertNotIn(
            ("collection", "remove", qmd_manager.COL_ARCHIVE), commands)

    def test_qmd_activity_snapshot_is_closed_and_phase_aware(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with mock.patch.object(
                qmd_manager, "ACTIVITY_REGISTRY", root / "activity.json",
            ), mock.patch.object(
                qmd_manager,
                "ACTIVITY_REGISTRY_LOCK",
                root / "activity.lock",
            ), mock.patch.object(
                qmd_manager,
                "QMD_OPERATION_LOCK",
                root / "operation.lock",
            ):
                manager = QmdManager()
                manager.client.health = mock.Mock(return_value=True)
                self.assertEqual(manager.activity_snapshot(), {
                    "document_update": {
                        "queued": 0, "running": 0, "state": "absent"},
                    "document_embed": {
                        "queued": 0, "running": 0, "state": "absent"},
                    "qmd_server": {"persistent": True, "state": "idle"},
                })

                manager.begin_external_update()
                manager._queue_index()
                snapshot = manager.activity_snapshot()
                self.assertEqual(
                    snapshot["document_update"],
                    {"queued": 1, "running": 1, "state": "running"},
                )
                self.assertEqual(
                    snapshot["document_embed"],
                    {"queued": 1, "running": 0, "state": "queued"},
                )
                self.assertEqual(
                    snapshot["qmd_server"],
                    {"persistent": True, "state": "busy"},
                )
                with manager._activity_lock:
                    manager._dirty_pending = False
                    manager._dirty.clear()
                    manager._external_update_running = 0
                    manager._ready = True
                    manager._publish_process_activity_locked()
                idle = manager.activity_snapshot()
                self.assertEqual(
                    idle["document_update"],
                    {"queued": 0, "running": 0, "state": "idle"},
                )
                self.assertEqual(
                    idle["document_embed"],
                    {"queued": 0, "running": 0, "state": "idle"},
                )

    def test_qmd_readiness_balances_activity_when_lock_setup_fails(self):
        manager = QmdManager()
        with mock.patch.object(
            manager, "begin_external_update",
        ) as begin, mock.patch.object(
            manager, "end_external_update",
        ) as end, mock.patch.object(
            manager, "_lock_file", side_effect=OSError("lock failed"),
        ), self.assertRaisesRegex(OSError, "lock failed"):
            manager.ensure_ready()
        begin.assert_called_once_with()
        end.assert_called_once_with()

    def test_qmd_readiness_balances_activity_when_unlock_fails(self):
        manager = QmdManager()
        handle = object()
        with mock.patch.object(
            manager, "begin_external_update",
        ), mock.patch.object(
            manager, "end_external_update",
        ) as end, mock.patch.object(
            manager, "_lock_file", return_value=handle,
        ), mock.patch.object(
            manager, "_unlock_file", side_effect=OSError("unlock failed"),
        ), mock.patch.object(
            manager, "_ensure_collections",
        ), mock.patch.object(
            manager, "_ensure_server",
        ), mock.patch.object(
            manager, "_start_worker",
        ), mock.patch.object(
            manager, "_queue_index",
        ), self.assertRaisesRegex(OSError, "unlock failed"):
            manager.ensure_ready()
        end.assert_called_once_with()

    def test_qmd_dead_process_presence_is_unknown(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            activity_path = root / "activity.json"
            activity_path.write_text(json.dumps({
                "99999999": {
                    "pid": 99999999,
                    "initialized": True,
                    "update_queued": 0,
                    "update_running": 1,
                    "embed_queued": 0,
                    "embed_running": 0,
                },
            }), encoding="utf-8")
            with mock.patch.object(
                qmd_manager, "ACTIVITY_REGISTRY", activity_path,
            ), mock.patch.object(
                qmd_manager,
                "ACTIVITY_REGISTRY_LOCK",
                root / "activity.lock",
            ), mock.patch.object(
                QmdManager, "_pid_alive", return_value=False,
            ):
                manager = QmdManager()
                manager.client.health = mock.Mock(return_value=True)
                snapshot = manager.activity_snapshot()
        self.assertEqual(snapshot["document_update"]["state"], "unknown")
        self.assertEqual(snapshot["document_embed"]["state"], "unknown")
        self.assertEqual(snapshot["qmd_server"]["state"], "unknown")


class BenchE2eConfigOverrideTests(unittest.TestCase):
    def test_accepts_repository_relative_json_and_returns_canonical_path(self):
        resolved, error = build_worker._validate_bench_e2e_config_override(
            "benchmark-e2e", "bench/bench_e2e_configuration.json")

        self.assertIsNone(error)
        self.assertEqual(
            resolved,
            str((build_presets.REPOSITORY_ROOT
                 / "bench/bench_e2e_configuration.json").resolve()),
        )

    def test_rejects_override_for_every_other_mode(self):
        resolved, error = build_worker._validate_bench_e2e_config_override(
            "benchmark", "bench/bench_e2e_configuration.json")

        self.assertIsNone(resolved)
        self.assertIn("only for mode='benchmark-e2e'", error)

    def test_rejects_existing_json_outside_repository(self):
        outside_json = Path("/tmp/bioseq-build-e2e-outside.json")
        outside_json.write_text("{}", encoding="utf-8")
        self.addCleanup(outside_json.unlink, missing_ok=True)

        resolved, error = build_worker._validate_bench_e2e_config_override(
            "benchmark-e2e", str(outside_json))

        self.assertIsNone(resolved)
        self.assertIn("inside the repository", error)

    def test_rejects_missing_and_non_json_paths(self):
        missing, missing_error = build_worker._validate_bench_e2e_config_override(
            "benchmark-e2e", "bench/does-not-exist.json")
        non_json, non_json_error = build_worker._validate_bench_e2e_config_override(
            "benchmark-e2e", "build_mcp/server.py")

        self.assertIsNone(missing)
        self.assertIn("existing file", missing_error)
        self.assertIsNone(non_json)
        self.assertIn("must name a .json file", non_json_error)


class AlgorithmMatrixSingletonAuthorityTests(unittest.TestCase):
    LEGACY_EMPTY_LEDGER_FIXTURE = (
        REPOSITORY_ROOT
        / "bench/tests/contract/fixtures/q1024_revision4_empty_ledger.json"
    )
    SINGLETON0058 = (
        "bench/e2e_algorithm_matrix_shards/"
        "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
        "0058_15aceb88564a0756.json"
    )
    SINGLETON0068 = (
        "bench/e2e_algorithm_matrix_shards/"
        "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
        "0068_2496323c88e8add2.json"
    )
    PROBE_SINGLETON = (
        "bench/e2e_algorithm_matrix_shards/"
        "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
        "0664_ce4a7113f77b3cdf.json"
    )
    SUPERSEDED_SINGLETON = (
        "bench/e2e_algorithm_matrix_shards/"
        "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
        "0081_423ec685980909f5.json"
    )
    PACKED_CONFIG = (
        "bench/e2e_algorithm_matrix_q1024_packed_shards/"
        "bench_e2e_algorithm_matrix_q1024_packed_"
        "m2_d4_k20_w15_minimap2_both_modes.json"
    )
    M4_MPS_PACKED_CONFIG = (
        "bench/e2e_algorithm_matrix_shards/"
        "bench_e2e_algorithm_matrix_campaign_shard_"
        "m4_d4_k15_w10_just_index_scalar_mpsequence.json"
    )
    OBSERVED_CONFIG = (
        "bench/e2e_algorithm_matrix_q2_observed_singletons/"
        "bench_e2e_algorithm_matrix_q2_observed_singleton_"
        "0725_925f4f05e3a6c239.json"
    )

    def _legacy_empty_ledger(self):
        return json.loads(
            self.LEGACY_EMPTY_LEDGER_FIXTURE.read_text(encoding="utf-8"))

    def _ledger_document_patch(self, document):
        ledger_path = REPOSITORY_ROOT / "research_benchmarks/paper/e2e/ALL_RESULTS.yaml"
        raw = json.dumps(
            document, sort_keys=True, separators=(",", ":"),
            ensure_ascii=True, allow_nan=False,
        ).encode("utf-8")
        strict_json = build_worker._strict_json_object

        def load_with_fixture(path, label):
            if Path(path) == ledger_path:
                return copy.deepcopy(document), raw
            return strict_json(path, label)

        return mock.patch.object(
            build_worker, "_strict_json_object", side_effect=load_with_fixture)

    def _nonempty_current_ledger_patch(self):
        current = self._legacy_empty_ledger()
        current["schema_version"] = 6
        current["contract"]["version"] = 4
        current["aggregation_state"]["processed_reports"] = [
            {"sealed_test_adoption": True}]
        return self._ledger_document_patch(current)

    def test_exact_probe_singleton_is_hybrid_authorized_after_adoption(self):
        with self._nonempty_current_ledger_patch():
            authority, error = (
                build_worker._algorithm_matrix_campaign_authority_status(
                    self.PROBE_SINGLETON))

        self.assertIsNone(error)
        self.assertEqual(authority["name"], "q1024")
        self.assertEqual(authority["execution_role"], "q1024_singleton")
        profile = build_worker._ALGORITHM_MATRIX_CAMPAIGN_PROFILES[0]
        self.assertEqual(
            authority["partition_manifest_sha256"],
            profile["packed_partition_manifest_sha256"],
        )
        self.assertEqual(
            authority["singleton_partition_manifest_sha256"],
            profile["singleton_partition_manifest_sha256"],
        )
        self.assertEqual(authority["singleton"]["ordinal"], 664)
        self.assertEqual(authority["singleton"]["parent_tuple_index"], 0)
        self.assertEqual(
            authority["singleton"]["parent_config_sha256"],
            "aa8c89910b5375a3fb1161b1b695b3d6d39dd3c57ca1092a06612d53b0e2b6f2",
        )
        packed_manifest = json.loads((
            REPOSITORY_ROOT / profile["packed_partition_manifest"]
        ).read_text(encoding="utf-8"))
        retained = [
            entry for entry in packed_manifest["retained_singletons"]
            if entry["ordinal"] == 664
        ]
        self.assertEqual(len(retained), 1)
        self.assertEqual(retained[0]["role"], "first_mps_probe")
        self.assertEqual(retained[0]["path"], self.PROBE_SINGLETON)
        self.assertEqual(
            retained[0]["stable_key_sha256"],
            authority["singleton"]["stable_key_sha256"],
        )

    def test_independent_q1024_timeout_authorities_are_exact_and_closed(self):
        singleton_manifest = json.loads((
            REPOSITORY_ROOT
            / "bench/bench_e2e_algorithm_matrix_q1024_"
              "singleton_partition_manifest.json"
        ).read_text(encoding="utf-8"))

        def campaign_authority(ordinal):
            singleton = next(
                child for child in singleton_manifest["children"]
                if child["ordinal"] == ordinal)
            return {
                "name": "q1024",
                "execution_role": "q1024_singleton",
                "singleton_partition_manifest_sha256": (
                    build_worker._ALGORITHM_MATRIX_CAMPAIGN_PROFILES[0][
                        "singleton_partition_manifest_sha256"]),
                "singleton": singleton,
            }

        hard_stop = (
            build_worker._ALGORITHM_MATRIX_Q1024_SINGLETON0068_HARD_STOP_EPOCH)
        with mock.patch.object(
                build_worker.time, "time", return_value=hard_stop - 1):
            authority0058, authority0058_error = (
                build_worker._q1024_extended_timeout_authority(
                    self.SINGLETON0058, campaign_authority(58)))
            authority0068, authority0068_error = (
                build_worker._q1024_extended_timeout_authority(
                    self.SINGLETON0068, campaign_authority(68)))
        self.assertIsNone(authority0058_error)
        self.assertIsNone(authority0068_error)
        self.assertEqual(
            authority0058["name"],
            "q1024_singleton0058_extended_timeout")
        self.assertEqual(
            authority0068["name"],
            "q1024_singleton0068_extended_timeout")
        self.assertNotEqual(
            authority0058["manifest"], authority0068["manifest"])

        with mock.patch.object(
                build_worker.time, "time", return_value=hard_stop):
            expired_authority, expired_error = (
                build_worker._q1024_extended_timeout_authority(
                    self.SINGLETON0068, campaign_authority(68)))
        self.assertIsNone(expired_authority)
        self.assertIn("authority has expired", expired_error)

        with mock.patch.object(
                build_worker.time, "time", return_value=hard_stop - 1_000):
            bounded_timeout = (
                build_worker._run_only_runtime_timeout_seconds(
                    "benchmark-e2e", self.SINGLETON0068, 14_400.0))
        self.assertEqual(bounded_timeout, 999.0)
        with mock.patch.object(
                build_worker,
                "_algorithm_matrix_campaign_authority_status",
                return_value=({"name": "q1024"}, None)):
            self.assertEqual(
                build_worker._run_only_runtime_timeout_seconds(
                    "benchmark-e2e", self.PROBE_SINGLETON, 14_400.0),
                14_400.0,
            )

        deadline_q1024 = {
            "name": "deadline",
            "execution_role": "deadline_singleton",
            "runtime_timeout_seconds": 172_800,
            "selected_shard": {"identity": {"query_count": 1024}},
        }
        with mock.patch.object(
                build_worker,
                "_algorithm_matrix_campaign_authority_status",
                return_value=(deadline_q1024, None)):
            self.assertEqual(
                build_worker._run_only_runtime_timeout_seconds(
                    "benchmark-e2e", "canonical-deadline-member", 14_400.0),
                43_200.0,
            )
        q1024_kill_offset = (
            build_worker._Q1024_WHOLE_INVOCATION_TIMEOUT_SECONDS
            - build_worker._Q1024_CLEANUP_RESERVE_SECONDS
        )
        q1024_work_offset = (
            q1024_kill_offset
            - process_state.BUILD_TIMEOUT_TERM_GRACE_SEC
        )
        self.assertEqual(q1024_kill_offset, 42_900)
        self.assertEqual(q1024_work_offset, 42_890)

        observed_q1024 = {
            "name": "observed_rung",
            "execution_role": "observed_rung_singleton",
            "query_count": 1024,
        }
        with mock.patch.object(
                build_worker,
                "_algorithm_matrix_campaign_authority_status",
                return_value=(observed_q1024, None)):
            self.assertEqual(
                build_worker._run_only_runtime_timeout_seconds(
                    "benchmark-e2e", "canonical-observed-member", 14_400.0),
                14_400.0,
            )
        owner = build_worker._q1024_postgres_application_name("a" * 32)
        self.assertEqual(
            build_worker._validated_q1024_postgres_application_name(owner),
            owner,
        )

    def test_all_q1024_authority_shapes_reject_nonreceipt_execution(self):
        authorities = (
            {"name": "q1024", "execution_role": "q1024_singleton"},
            {
                "name": "observed_rung",
                "execution_role": "observed_rung_singleton",
                "query_count": 1024,
            },
            {
                "name": "deadline",
                "execution_role": "deadline_singleton",
                "selected_shard": {"identity": {"query_count": 1024}},
            },
        )
        for authority in authorities:
            with self.subTest(name=authority["name"]):
                self.assertTrue(
                    build_worker._q1024_nonreceipt_execution_forbidden(
                        authority, build_only=False, run_only=False))
                self.assertFalse(
                    build_worker._q1024_nonreceipt_execution_forbidden(
                        authority, build_only=True, run_only=False))
                self.assertFalse(
                    build_worker._q1024_nonreceipt_execution_forbidden(
                        authority, build_only=False, run_only=True))

    def test_expired_control_deadline_prevents_receipt_io_or_publication(self):
        with mock.patch.object(
                build_worker.time, "time", return_value=100.0), \
                mock.patch.object(
                    build_worker, "_get_build_receipt") as get_receipt:
            with self.assertRaises(TimeoutError):
                build_worker._validate_build_receipt(
                    "br_test", deadline=100.0)
        get_receipt.assert_not_called()

        with mock.patch.object(
                build_worker.time, "time", return_value=100.0), \
                mock.patch.object(
                    build_worker, "_store_build_receipt") as store:
            with self.assertRaises(TimeoutError):
                build_worker._seal_run_receipt(
                    mode="benchmark-e2e",
                    build_receipt={},
                    report_files=[],
                    bench_flags=[],
                    test_flags=[],
                    bench_e2e_config=None,
                    deadline=100.0,
                )
        store.assert_not_called()

    def test_exact_packed_config_is_authorized_after_adoption(self):
        with self._nonempty_current_ledger_patch():
            authority, error = (
                build_worker._algorithm_matrix_campaign_authority_status(
                    self.PACKED_CONFIG))

        self.assertIsNone(error)
        self.assertEqual(authority["execution_role"], "q1024_packed")
        self.assertEqual(authority["packed_member"]["expected_result_cells"], 2)

    def test_q1024_backend_cleanup_feeds_sql_on_stdin(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "build"
            bindir = Path(temporary) / "pg-bin"
            build_dir.mkdir()
            bindir.mkdir()
            pg_config = Path(temporary) / "pg_config"
            pg_config.write_text("stub\n", encoding="utf-8")
            psql = bindir / "psql"
            psql.write_text("stub\n", encoding="utf-8")
            psql.chmod(0o755)
            (build_dir / "CMakeCache.txt").write_text(
                f"PostgreSQL_CONFIG:FILEPATH={pg_config}\n",
                encoding="utf-8",
            )
            completed = [
                subprocess.CompletedProcess([], 0, f"{bindir}\n", ""),
                subprocess.CompletedProcess([], 0, "t\n", ""),
                subprocess.CompletedProcess([], 0, "t\n", ""),
                subprocess.CompletedProcess([], 0, "0\n", ""),
            ]
            with mock.patch.object(
                    postgres_runtime.subprocess,
                    "run",
                    side_effect=completed,
            ) as run:
                evidence = postgres_runtime._cleanup_owned_postgresql_backends(
                    build_dir,
                    "bench-e2e-opt",
                    application_name="bioseq-q1024-" + "a" * 32,
                    database_name="bioseq",
                    database_user="postgres",
                    log_handle=io.StringIO(),
                    deadline=time.time() + 60.0,
                )
        self.assertTrue(evidence["cleanup_verified"])
        for call in run.call_args_list[1:]:
            argv = call.args[0]
            statement = call.kwargs.get("input")
            self.assertNotIn("--command", argv)
            self.assertEqual(argv[-2:], ["--file", "-"])
            self.assertIn(
                "owner_application=bioseq-q1024-" + "a" * 32, argv)
            self.assertIn("owner_database=bioseq", argv)
            self.assertIn("owner_user=postgres", argv)
            self.assertIsInstance(statement, str)
            self.assertIn(":'owner_application'", statement)
            self.assertIn(":'owner_database'", statement)
            self.assertIn(":'owner_user'", statement)

    def test_q1024_backend_last_resort_rechecks_exact_backend_and_postmaster(self):
        backend = process_state._ProcessIdentity(
            444, 111, 444, 444, "backend-os-start", "postgres backend")
        postmaster = process_state._ProcessIdentity(
            111, 1, 111, 111, "postmaster-os-start", "postgres")
        backend_start = "2026-09-09 10:00:00+00"
        postmaster_start = "2026-09-09 09:00:00+00"
        snapshot = f"444\t{backend_start}\t{postmaster_start}\n"
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "build"
            bindir = Path(temporary) / "pg-bin"
            data_dir = Path(temporary) / "pg-data"
            build_dir.mkdir()
            bindir.mkdir()
            data_dir.mkdir()
            pg_config = Path(temporary) / "pg_config"
            pg_config.write_text("stub\n", encoding="utf-8")
            psql = bindir / "psql"
            psql.write_text("stub\n", encoding="utf-8")
            psql.chmod(0o755)
            (build_dir / "CMakeCache.txt").write_text(
                f"PostgreSQL_CONFIG:FILEPATH={pg_config}\n",
                encoding="utf-8",
            )
            completed = [
                subprocess.CompletedProcess([], 0, f"{bindir}\n", ""),
                subprocess.CompletedProcess([], 0, snapshot, ""),
                subprocess.CompletedProcess([], 0, "t\n", ""),
                subprocess.CompletedProcess([], 0, "t\n", ""),
                subprocess.CompletedProcess([], 0, "1\n", ""),
                subprocess.CompletedProcess([], 0, snapshot, ""),
                subprocess.CompletedProcess([], 0, "0\n", ""),
            ]

            def capture(pid, *, deadline=None):
                return postmaster if pid == 111 else backend if pid == 444 else None

            with mock.patch.object(
                    postgres_runtime.subprocess,
                    "run",
                    side_effect=completed,
            ), mock.patch.object(
                    postgres_runtime,
                    "_query_postgresql_data_directory",
                    return_value=str(data_dir),
            ), mock.patch.object(
                    postgres_runtime,
                    "_postgresql_postmaster_pid",
                    return_value=111,
            ), mock.patch.object(
                    postgres_runtime,
                    "_capture_process_identity",
                    side_effect=capture,
            ), mock.patch.object(postgres_runtime.os, "kill") as kill:
                evidence = postgres_runtime._cleanup_owned_postgresql_backends(
                    build_dir,
                    "bench-e2e-opt",
                    application_name="bioseq-q1024-" + "b" * 32,
                    database_name="bioseq",
                    database_user="postgres",
                    log_handle=io.StringIO(),
                    deadline=time.time() + 60.0,
                    allow_sigkill_backend=True,
                )

        kill.assert_called_once_with(444, signal.SIGKILL)
        self.assertTrue(evidence["cleanup_verified"])
        self.assertTrue(evidence["last_resort"]["recovery_ready"])
        self.assertEqual(
            evidence["sealed_backends"][0]["backend_start"], backend_start)

    def test_exact_m4_mps_parent_is_authorized_as_one_eight_cell_pack(self):
        with self._nonempty_current_ledger_patch():
            authority, error = (
                build_worker._algorithm_matrix_campaign_authority_status(
                    self.M4_MPS_PACKED_CONFIG))

        self.assertIsNone(error)
        self.assertEqual(authority["execution_role"], "q1024_packed")
        member = authority["packed_member"]
        self.assertEqual(member["expected_result_cells"], 8)
        self.assertEqual(member["expected_raw_records"], 40)
        self.assertEqual(member["planned_supported_cells"], 6)
        self.assertEqual(member["planned_selector_rejection_cells"], 2)

    def test_adopted_cell_singleton_is_superseded(self):
        authority, error = (
            build_worker._algorithm_matrix_campaign_authority_status(
                self.SUPERSEDED_SINGLETON))

        self.assertIsNone(authority)
        self.assertIn("superseded by exact packed/adopted ownership", error)

    def test_reserved_lookalike_is_not_authority(self):
        lookalike = (
            "bench/e2e_algorithm_matrix_shards/"
            "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
            "0081_0000000000000000.json"
        )
        authority, error = (
            build_worker._algorithm_matrix_campaign_authority_status(lookalike))

        self.assertIsNone(authority)
        self.assertIn("not manifest-owned", error)

    def test_empty_ledger_rejects_a_valid_nonfirst_singleton(self):
        with self._ledger_document_patch(self._legacy_empty_ledger()):
            authority, error = (
                build_worker._algorithm_matrix_campaign_authority_status(
                    self.PROBE_SINGLETON))

        self.assertIsNone(authority)
        self.assertIn("must start with deterministic baseline", error)
        self.assertIn("adoption s056", error)

    def test_worker_and_aggregator_share_exact_legacy_empty_boundary(self):
        aggregator_path = REPOSITORY_ROOT / "bench/aggregate_e2e_algorithm_matrix.py"
        spec = importlib.util.spec_from_file_location(
            "worker_test_algorithm_matrix_aggregator", aggregator_path)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        aggregator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(aggregator)
        aggregator._activate_campaign_profile("ALL_RESULTS.yaml")
        original = self._legacy_empty_ledger()
        mutations = {
            "result_cell_count": ("aggregation_state", "result_cell_count", 1),
            "accepted_cell_count": ("aggregation_state", "accepted_cell_count", 1),
            "measurements_collected": ("campaign", "measurements_collected", 5),
            "canonical_result_cell_count": (
                "aggregation_state", "canonical_result_cell_count", 1295),
            "report_count": ("aggregation_state", "report_count", 1),
            "raw_record_count": ("aggregation_state", "raw_record_count", 5),
            "processed_reports": (
                "aggregation_state", "processed_reports", [{}]),
            "run_receipt_membership": (
                "aggregation_state", "build_receipts", []),
            "unexpected_running_count": (
                "aggregation_state", "running_cell_count", 1),
            "missing_count": (
                "aggregation_state", "missing_canonical_cell_count", 1295),
            "pending_count": ("aggregation_state", "pending_cell_count", 1),
            "rejected_count": ("aggregation_state", "rejected_cell_count", 1),
            "invalid_count": ("aggregation_state", "invalid_cell_count", 1),
            "membership_complete": (
                "aggregation_state", "canonical_membership_complete", True),
            "statuses_invalid": (
                "aggregation_state", "canonical_statuses_valid", False),
            "publication_count": ("publication_state", "result_count", 1),
            "publication_status": (
                "publication_state", "status", "complete"),
            "publication_collection": (
                "publication_state", "result_collection", "published_results"),
            "publication_calibration_membership": (
                "publication_state", "calibration_results_included", True),
            "campaign_publishable": (
                "campaign", "results_are_publishable", True),
            "campaign_status": ("campaign", "status", "complete"),
            "campaign_started_count": (
                "campaign", "benchmark_runs_started", 999),
            "results_collection": (None, "results", [{}]),
            "top_level_status": (None, "status", "complete"),
            "deadline_checkpoint_tamper": (
                None, "deadline_v2_checkpoint", {}),
        }
        worker_args = (
            build_worker._ALGORITHM_MATRIX_CAMPAIGN_ID,
            build_worker._ALGORITHM_MATRIX_CAMPAIGN_DIGEST,
        )
        self.assertTrue(build_worker._legacy_q1024_ledger_is_exactly_empty(
            original, *worker_args))
        self.assertTrue(aggregator._legacy_q1024_ledger_is_exactly_empty(
            original, *worker_args))
        for name, (section, field, value) in mutations.items():
            with self.subTest(name=name):
                document = copy.deepcopy(original)
                if section is None:
                    document[field] = value
                else:
                    document[section][field] = value
                worker_result = (
                    build_worker._legacy_q1024_ledger_is_exactly_empty(
                        document, *worker_args))
                aggregator_result = (
                    aggregator._legacy_q1024_ledger_is_exactly_empty(
                        document, *worker_args))
                self.assertEqual(worker_result, aggregator_result)
                self.assertFalse(worker_result)

    def test_campaign_authority_rejects_populated_legacy_ledger(self):
        stale = self._legacy_empty_ledger()
        stale["campaign"]["measurements_collected"] = 5
        with self._ledger_document_patch(stale):
            authority, error = (
                build_worker._algorithm_matrix_campaign_authority_status(
                    self.PROBE_SINGLETON))

        self.assertIsNone(authority)
        self.assertIn("does not bind the audited campaign authority", error)

    def test_q1024_canonical_configs_are_topology_only(self):
        authority, error = (
            build_worker._algorithm_matrix_campaign_authority_status(
                "bench/bench_e2e_algorithm_matrix_campaign.json"))

        self.assertIsNone(error)
        self.assertEqual(authority["execution_role"], "canonical_topology")

    def test_start_rejects_singleton_without_run_only(self):
        with self._nonempty_current_ledger_patch(), mock.patch.object(
            build_worker, "_refuse_root_build_identity", return_value=None,
        ), mock.patch.object(
            build_worker, "_ensure_build_worker_timesharing_policy",
        ):
            result = build_worker.tool_start_build(
                mode="benchmark-e2e",
                bench_filter={"benchmarks": ["bench_e2e"]},
                filter_supplied=True,
                bench_e2e_config=self.PROBE_SINGLETON,
            )

        self.assertIn("requires run_only=true", result)

    def test_singleton_terminal_flow_seals_receipt_before_ingestion(self):
        self._assert_terminal_flow_seals_receipt_before_consumer(self.PROBE_SINGLETON)

    def test_observed_terminal_flow_seals_receipt_before_validation(self):
        self._assert_terminal_flow_seals_receipt_before_consumer(self.OBSERVED_CONFIG)

    def _assert_terminal_flow_seals_receipt_before_consumer(self, config):
        events = []
        build_receipt = {
            "build_receipt_id": "br_test",
            "debug": False,
            "authorized_artifacts": [{"sealed": True}],
        }
        run_receipt = {
            "build_receipt_id": "rr_" + "a" * 32,
            "content_sha256": "b" * 64,
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            report = root / "report_1.json"
            report.write_text("{}\n", encoding="utf-8")
            with (root / "run.log").open("w", encoding="utf-8") as log_handle, \
                    self._nonempty_current_ledger_patch(), \
                    mock.patch.object(
                        build_worker,
                        "_build_worker_descendant_environment",
                        return_value={},
                    ), mock.patch.object(
                        build_worker, "_ensure_local_toolchain_environment",
                    ), mock.patch.object(
                        build_worker, "_stream_subprocess", return_value=0,
                    ), mock.patch.object(
                        build_worker, "_restart_postgresql",
                    ) as restart_postgresql, mock.patch.object(
                        build_worker, "_prepare_postgresql_test_database",
                    ) as prepare_postgresql, mock.patch.object(
                        build_worker,
                        "_benchmark_e2e_runtime_environment",
                        return_value={},
                    ), mock.patch.object(
                        build_worker,
                        "_ensure_algorithm_matrix_launch_nice",
                        side_effect=lambda *args: events.append("priority"),
                    ), mock.patch.object(
                        build_worker,
                        "_run_mode_commands",
                        side_effect=lambda *args, **kwargs: (
                            events.append("run")
                            or (report, [report], [], [])),
                    ) as run_commands, mock.patch.object(
                        build_worker,
                        "_validate_build_receipt",
                        return_value=(build_receipt, None),
                    ), mock.patch.object(
                        build_worker,
                        "_seal_run_receipt",
                        side_effect=lambda **_: (
                            events.append("seal") or run_receipt),
                    ), mock.patch.object(
                        build_worker,
                        "_merge_algorithm_matrix_all_results",
                        autospec=True,
                        side_effect=lambda **kwargs: events.append(
                            ("merge", kwargs.get("run_receipt_id"))),
                    ), mock.patch.object(build_worker, "_update_entry"):
                result = build_worker._run_only_from_build_receipt(
                    mode="benchmark-e2e",
                    entry={"started_at": time.time()},
                    receipt=build_receipt,
                    active_build_dir=root,
                    active_bin_dir=root / "bioseq",
                    active_preset="bench-e2e-opt",
                    bench_flags=["--filter", "benchmark=bench_e2e"],
                    test_flags=[],
                    bench_e2e_config=config,
                    log_handle=log_handle,
                    deadline=time.time() + 60,
                )

        self.assertEqual(result, 0)
        self.assertEqual(
            events,
            [
                "priority", "run", "seal",
                ("merge", "rr_" + "a" * 32),
            ],
        )
        self.assertEqual(
            run_commands.call_args.kwargs["state_mode"], "benchmark-e2e")
        self.assertEqual(
            run_commands.call_args.kwargs["phase"], "run_only_runtime")
        self.assertEqual(
            restart_postgresql.call_args.kwargs["state_mode"],
            "benchmark-e2e",
        )
        self.assertEqual(
            restart_postgresql.call_args.kwargs["phase"],
            "run_only_postgresql_restart",
        )
        self.assertEqual(
            prepare_postgresql.call_args.kwargs["state_mode"],
            "benchmark-e2e",
        )
        self.assertEqual(
            prepare_postgresql.call_args.kwargs["phase"],
            "run_only_postgresql_database_prepare",
        )

    def test_consumer_failure_preserves_factual_run_receipt(self):
        build_receipt = {
            "build_receipt_id": "br_test",
            "debug": False,
            "authorized_artifacts": [{"sealed": True}],
        }
        run_receipt = {
            "build_receipt_id": "rr_" + "a" * 32,
            "content_sha256": "b" * 64,
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            report = root / "report_1.json"
            report.write_text("{}\n", encoding="utf-8")
            with (root / "run.log").open("w", encoding="utf-8") as log_handle, \
                    self._nonempty_current_ledger_patch(), \
                    mock.patch.object(
                        build_worker, "_build_worker_descendant_environment",
                        return_value={},
                    ), mock.patch.object(
                        build_worker, "_ensure_local_toolchain_environment",
                    ), mock.patch.object(
                        build_worker, "_stream_subprocess", return_value=0,
                    ), mock.patch.object(
                        build_worker, "_restart_postgresql",
                    ), mock.patch.object(
                        build_worker, "_prepare_postgresql_test_database",
                    ), mock.patch.object(
                        build_worker, "_benchmark_e2e_runtime_environment",
                        return_value={},
                    ), mock.patch.object(
                        build_worker, "_ensure_algorithm_matrix_launch_nice",
                    ), mock.patch.object(
                        build_worker, "_run_mode_commands",
                        return_value=(report, [report], [], []),
                    ), mock.patch.object(
                        build_worker, "_validate_build_receipt",
                        return_value=(build_receipt, None),
                    ), mock.patch.object(
                        build_worker, "_seal_run_receipt",
                        return_value=run_receipt,
                    ) as seal, mock.patch.object(
                        build_worker, "_merge_algorithm_matrix_all_results",
                        autospec=True,
                        side_effect=RuntimeError("consumer rejected report"),
                    ), mock.patch.object(
                        build_worker, "_update_entry",
                    ) as update:
                with self.assertRaisesRegex(
                        RuntimeError, "consumer rejected report"):
                    build_worker._run_only_from_build_receipt(
                        mode="benchmark-e2e",
                        entry={"started_at": time.time()},
                        receipt=build_receipt,
                        active_build_dir=root,
                        active_bin_dir=root / "bioseq",
                        active_preset="bench-e2e-opt",
                        bench_flags=["--filter", "benchmark=bench_e2e"],
                        test_flags=[],
                        bench_e2e_config=self.PROBE_SINGLETON,
                        log_handle=log_handle,
                        deadline=time.time() + 60,
                    )
        seal.assert_called_once()
        self.assertNotIn("persist", seal.call_args.kwargs)
        self.assertTrue(any(
            call.kwargs.get("run_receipt_id")
            == run_receipt["build_receipt_id"]
            for call in update.call_args_list
        ))
        self.assertFalse(any(
            call.kwargs.get("status") == "completed"
            for call in update.call_args_list
        ))


class ObservedRungAuthorityTests(unittest.TestCase):
    PRIORITY_SINGLETON_SUFFIX = "0725_925f4f05e3a6c239.json"

    @classmethod
    def config_for(cls, query_count):
        return (
            f"bench/e2e_algorithm_matrix_q{query_count}_observed_singletons/"
            f"bench_e2e_algorithm_matrix_q{query_count}_observed_singleton_"
            f"{cls.PRIORITY_SINGLETON_SUFFIX}")

    def test_all_nine_observed_rungs_bind_exact_manifest_members(self):
        self.assertEqual(
            tuple(
                profile["query_count"]
                for profile in build_worker._ALGORITHM_MATRIX_OBSERVED_RUNG_PROFILES
            ),
            (2, 8, 16, 32, 64, 128, 256, 512, 1024),
        )
        for profile in build_worker._ALGORITHM_MATRIX_OBSERVED_RUNG_PROFILES:
            query_count = profile["query_count"]
            config = self.config_for(query_count)
            with self.subTest(query_count=query_count):
                authority, error = (
                    build_worker._algorithm_matrix_campaign_authority_status(
                        config))
                self.assertIsNone(error)
                self.assertEqual(authority["name"], "observed_rung")
                self.assertEqual(
                    authority["execution_role"], "observed_rung_singleton")
                self.assertEqual(authority["query_count"], query_count)
                self.assertEqual(authority["manifest"], profile["manifest"])
                self.assertEqual(
                    authority["campaign_digest"], profile["campaign_digest"])
                self.assertEqual(authority["selected_config"]["path"], config)
                self.assertEqual(
                    authority["partition_manifest_sha256"],
                    profile["singleton_partition_manifest_sha256"],
                )

    def test_observed_parent_configs_remain_topology_only(self):
        parent = (
            "bench/e2e_algorithm_matrix_q2_observed_shards/"
            "bench_e2e_algorithm_matrix_q2_observed_shard_"
            "m2_d4_k15_w10_just_index_bounded_nthash_simd_mpfragment.json")
        authority, error = (
            build_worker._algorithm_matrix_campaign_authority_status(parent))
        self.assertIsNone(error)
        self.assertEqual(authority["execution_role"], "observed_rung_topology")

    def test_observed_rung_absolute_path_normalizes_but_aliases_do_not(self):
        config = self.config_for(2)
        absolute = str(REPOSITORY_ROOT / config)
        resolved, error = build_worker._validate_bench_e2e_config_override(
            "benchmark-e2e", absolute)
        self.assertIsNone(error)
        self.assertEqual(resolved, config)

        directory = REPOSITORY_ROOT / (
            "bench/e2e_algorithm_matrix_q2_observed_singletons")
        aliased = str(directory / ".." / directory.name / Path(config).name)
        rejected, alias_error = (
            build_worker._validate_bench_e2e_config_override(
                "benchmark-e2e", aliased))
        self.assertIsNone(rejected)
        self.assertIn("lexical alias", alias_error)

    def test_observed_rung_rejects_unowned_path_and_selected_hash(self):
        forged = (
            "bench/e2e_algorithm_matrix_q2_observed_singletons/"
            "bench_e2e_algorithm_matrix_q2_observed_singleton_"
            "9999_0000000000000000.json")
        authority, error = (
            build_worker._algorithm_matrix_campaign_authority_status(forged))
        self.assertIsNone(authority)
        self.assertIn("not manifest-owned", error)

        config = self.config_for(2)
        selected_path = REPOSITORY_ROOT / config
        real_sha256_file = build_worker.sha256_file

        def mismatched_selected_hash(path):
            if Path(path) == selected_path:
                return "0" * 64
            return real_sha256_file(path)

        with mock.patch.object(
                build_worker, "sha256_file",
                side_effect=mismatched_selected_hash):
            authority, error = (
                build_worker._algorithm_matrix_campaign_authority_status(
                    config))
        self.assertIsNone(authority)
        self.assertIn("config bytes differ from the manifest", error)

    def test_observed_rung_start_requires_run_only_filter_and_receipt(self):
        config = self.config_for(2)
        with mock.patch.object(
            build_worker, "_refuse_root_build_identity", return_value=None,
        ), mock.patch.object(
            build_worker, "_ensure_build_worker_timesharing_policy",
        ):
            full_result = build_worker.tool_start_build(
                mode="benchmark-e2e",
                bench_filter={"benchmarks": ["bench_e2e"]},
                filter_supplied=True,
                bench_e2e_config=config,
            )
            with mock.patch.object(
                    build_worker, "_validate_build_receipt") as validate:
                filter_result = build_worker.tool_start_build(
                    mode="benchmark-e2e",
                    run_only=True,
                    reuse_build_receipt="br_test",
                    bench_filter={"benchmarks": ["bench_e2e", "other"]},
                    filter_supplied=True,
                    bench_e2e_config=config,
                )
                validate.assert_not_called()

            wrong_receipt = {
                "debug": False,
                "preset": "test-opt",
                "mode": "unit-integration",
                "preset_family": "test",
                "binary_subdir": "bioseq",
            }
            with mock.patch.object(
                    build_worker, "_validate_build_receipt",
                    return_value=(wrong_receipt, None)):
                receipt_result = build_worker.tool_start_build(
                    mode="benchmark-e2e",
                    run_only=True,
                    reuse_build_receipt="br_test",
                    bench_filter={"benchmarks": ["bench_e2e"]},
                    filter_supplied=True,
                    bench_e2e_config=config,
                )

        self.assertIn("requires run_only=true", full_result)
        self.assertIn("accept only the exact filter", filter_result)
        self.assertIn("qualified debug=false Clang E2E receipt", receipt_result)

    def test_observed_worker_rejects_unreceipted_execution_before_commands(self):
        config = self.config_for(2)
        with mock.patch.object(
            build_worker, "_refuse_root_build_identity", return_value=None,
        ), mock.patch.object(
            build_worker, "_ensure_build_worker_timesharing_policy",
        ), mock.patch.object(
            build_worker, "_load_state", return_value={
                "benchmark-e2e": {"bench_e2e_config": config},
            },
        ), mock.patch.object(
            build_worker, "_stream_subprocess",
        ) as stream, mock.patch.object(
            build_worker, "_run_mode_commands",
        ) as run_commands, mock.patch.object(build_worker, "_log") as log:
            result = build_worker._run_build_worker(
                "benchmark-e2e",
                bench_flags=["--filter", "benchmark=bench_e2e"],
                bench_e2e_config=config,
            )

        self.assertEqual(result, 1)
        stream.assert_not_called()
        run_commands.assert_not_called()
        self.assertIn("requires run_only=true", log.call_args.args[0])

    def test_observed_rung_forces_runtime_environment_and_normal_timeout(self):
        config = self.config_for(2)
        original = {
            "BIOSEQ_E2E_KEEP_ARTIFACTS": "1",
            "BIOSEQ_E2E_RETAIN_MATRIX_JUST_INDEX_TARGET": "1",
            "BIOSEQ_E2E_SKIP_INDEX_BASELINE_SETUP": "1",
            "BIOSEQ_E2E_SKIP_MINIMAP2_BASELINE_SETUP": "1",
            "BIOSEQ_E2E_JUST_INDEX_SETUP_ONLY": "1",
            "BIOSEQ_E2E_JUST_INDEX_SETUP_PIPELINE_MODE": "serial",
            "BIOSEQ_E2E_PG_RESTART_CMD": "ambient restart",
        }
        config_open_path = str(REPOSITORY_ROOT / config)
        with mock.patch.object(
            build_worker, "_postgresql_e2e_restart_shell_command",
            return_value="sealed restart",
        ), mock.patch.object(
            build_worker, "_validated_benchmark_e2e_config_open_path",
            return_value=config_open_path,
        ):
            environment = build_worker._benchmark_e2e_runtime_environment(
                original, Path("/unused"), "bench-e2e-opt", config)

            later_environment = build_worker._benchmark_e2e_runtime_environment(
                original, Path("/unused"), "bench-e2e-opt",
                self.config_for(8))

            no_index_environment = build_worker._benchmark_e2e_runtime_environment(
                original, Path("/unused"), "bench-e2e-opt",
                "bench/e2e_algorithm_matrix_q2_observed_singletons/"
                "bench_e2e_algorithm_matrix_q2_observed_singleton_"
                "0000_0ed6b7419bc8e21f.json")
            minimap2_environment = build_worker._benchmark_e2e_runtime_environment(
                original, Path("/unused"), "bench-e2e-opt",
                "bench/e2e_algorithm_matrix_q2_observed_singletons/"
                "bench_e2e_algorithm_matrix_q2_observed_singleton_"
                "0640_423ec685980909f5.json")

        expected_environment = dict(
            build_worker._ALGORITHM_MATRIX_RUNTIME_ENVIRONMENT)
        expected_environment[
            "BIOSEQ_E2E_RETAIN_MATRIX_JUST_INDEX_TARGET"] = "1"
        expected_environment[
            "BIOSEQ_E2E_SKIP_INDEX_BASELINE_SETUP"] = "1"
        for key, value in expected_environment.items():
            self.assertEqual(environment[key], value)
        self.assertEqual(
            environment["BIOSEQ_E2E_PG_RESTART_CMD"], "sealed restart")
        self.assertEqual(
            environment["BIOSEQ_E2E_RETAIN_MATRIX_JUST_INDEX_TARGET"], "1")
        self.assertEqual(
            environment["BIOSEQ_E2E_SKIP_INDEX_BASELINE_SETUP"], "1")
        self.assertEqual(
            later_environment[
                "BIOSEQ_E2E_RETAIN_MATRIX_JUST_INDEX_TARGET"], "1")
        self.assertEqual(
            later_environment["BIOSEQ_E2E_SKIP_INDEX_BASELINE_SETUP"], "1")
        self.assertEqual(later_environment["BIOSEQ_E2E_KEEP_ARTIFACTS"], "0")
        for unchanged_environment in (
                no_index_environment, minimap2_environment):
            self.assertEqual(
                unchanged_environment[
                    "BIOSEQ_E2E_RETAIN_MATRIX_JUST_INDEX_TARGET"], "0")
            self.assertEqual(
                unchanged_environment[
                    "BIOSEQ_E2E_SKIP_INDEX_BASELINE_SETUP"], "0")
            self.assertEqual(
                unchanged_environment["BIOSEQ_E2E_KEEP_ARTIFACTS"], "0")
        self.assertEqual(
            environment[build_worker._BENCHMARK_E2E_CONFIG_IDENTITY_ENV],
            config_open_path,
        )
        self.assertEqual(
            environment[build_worker._BENCHMARK_E2E_CONFIG_OPEN_ENV],
            config_open_path,
        )
        self.assertEqual(original["BIOSEQ_E2E_KEEP_ARTIFACTS"], "1")
        self.assertEqual(
            original["BIOSEQ_E2E_RETAIN_MATRIX_JUST_INDEX_TARGET"], "1")
        self.assertEqual(
            build_worker._run_only_runtime_timeout_seconds(
                "benchmark-e2e", config, 14_400.0),
            14_400.0,
        )

    def test_observed_rung_consumer_is_exact_read_only_validator(self):
        config = self.config_for(2)
        report = (
            REPOSITORY_ROOT
            / "reporting/bioseq_benchmark_e2e/report_999999.json")
        merger = REPOSITORY_ROOT / "bench/aggregate_e2e_algorithm_matrix.py"
        run_receipt_id = "rr_" + "a" * 32
        original_internal_file = build_worker._campaign_internal_regular_file

        def verified_internal_file(path, label):
            if Path(path) == report:
                return report
            return original_internal_file(path, label)

        log = io.StringIO()
        with mock.patch.object(
            build_worker, "_campaign_internal_regular_file",
            side_effect=verified_internal_file,
        ), mock.patch.object(
            build_worker, "_stream_subprocess", return_value=0,
        ) as stream, mock.patch.object(build_worker, "_update_entry"):
            build_worker._merge_algorithm_matrix_all_results(
                mode="benchmark-e2e",
                report_path=report,
                bench_e2e_config=config,
                environment={},
                log_handle=log,
                deadline=time.time() + 60,
                run_receipt_id=run_receipt_id,
            )

        command = stream.call_args.args[0]
        profile = build_worker._ALGORITHM_MATRIX_OBSERVED_RUNG_PROFILES[0]
        self.assertEqual(command, [
            sys.executable,
            str(merger),
            "validate-observed-rung-report",
            "--manifest", profile["manifest"],
            "--expected-campaign-digest", profile["campaign_digest"],
            "--query-count", "2",
            "--report", "reporting/bioseq_benchmark_e2e/report_999999.json",
            "--config", config,
            "--run-receipt-id", run_receipt_id,
        ])
        serialized = json.dumps(command)
        for forbidden in (
                "Q8_RESULTS.yaml", "ALL_RESULTS.yaml",
                "ALL_RESULTS_TUPLES.yaml", "--all-results"):
            self.assertNotIn(forbidden, serialized)
        self.assertEqual(
            stream.call_args.kwargs["phase"],
            "algorithm_matrix_observed_rung_validation",
        )


class AlgorithmMatrixLaunchNiceTests(unittest.TestCase):
    CONFIG = AlgorithmMatrixSingletonAuthorityTests.PROBE_SINGLETON

    def test_explicit_e2e_priority_flag_is_boolean_and_opt_in(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "config.json"
            config.write_text(json.dumps({
                "test_cases": {
                    "future": {
                        "data": {"require_nice_attestation": True},
                    },
                },
            }), encoding="utf-8")
            with mock.patch.object(
                build_worker,
                "_validated_benchmark_e2e_config_open_path",
                return_value=str(config),
            ):
                self.assertTrue(
                    build_worker._e2e_config_requests_nice_attestation(
                        "future.json"))

    def test_campaign_launches_preserve_inherited_nice(self):
        for platform_name, authority_name, inherited_nice in (
            ("Linux", "q1024", -20),
            ("Darwin", "observed_rung", 0),
            ("Linux", "q8", 7),
        ):
            with self.subTest(
                    platform=platform_name, authority=authority_name,
                    inherited_nice=inherited_nice):
                self._assert_campaign_launch_preserves_inherited_nice(
                    platform_name, authority_name, inherited_nice)

    def _assert_campaign_launch_preserves_inherited_nice(
            self, platform_name, authority_name, inherited_nice):
        log = io.StringIO()
        with mock.patch.object(
            build_worker, "_algorithm_matrix_campaign_authority_status",
            return_value=({"name": authority_name}, None),
        ), mock.patch.object(
            build_worker, "_e2e_config_requests_nice_attestation",
            return_value=False,
        ), mock.patch.object(
            build_worker.host_platform, "system", return_value=platform_name,
        ), mock.patch.object(
            build_worker.os, "setpriority",
        ) as setpriority, mock.patch.object(
            build_worker.os, "getpriority", return_value=inherited_nice,
        ) as getpriority:
            build_worker._ensure_algorithm_matrix_launch_nice(
                "benchmark-e2e", self.CONFIG, log)

        setpriority.assert_not_called()
        getpriority.assert_called_once_with(build_worker.os.PRIO_PROCESS, 0)
        self.assertIn(
            f"worker_NI={inherited_nice} inherited_preserved",
            log.getvalue())
        self.assertIn(f"platform={platform_name}", log.getvalue())
        self.assertIn("authority=canonical", log.getvalue())
        self.assertIn("setpriority_requested=false", log.getvalue())
        self.assertIn(
            "postgres_backend_priority_requires_exact_e2e_observation=true",
            log.getvalue(),
        )

    def test_darwin_explicit_e2e_attestation_establishes_ni_zero(self):
        log = io.StringIO()
        with mock.patch.object(
            build_worker, "_algorithm_matrix_campaign_authority_status",
            return_value=(None, None),
        ), mock.patch.object(
            build_worker, "_e2e_config_requests_nice_attestation",
            return_value=True,
        ), mock.patch.object(
            build_worker.host_platform, "system", return_value="Darwin",
        ), mock.patch.object(
            build_worker.os, "setpriority",
        ) as setpriority, mock.patch.object(
            build_worker.os, "getpriority", return_value=0,
        ) as getpriority:
            build_worker._ensure_algorithm_matrix_launch_nice(
                "benchmark-e2e", "build/mcp/future.json", log)

        setpriority.assert_called_once_with(build_worker.os.PRIO_PROCESS, 0, 0)
        getpriority.assert_called_once_with(build_worker.os.PRIO_PROCESS, 0)
        self.assertIn("authority=explicit_e2e_attestation", log.getvalue())
        self.assertIn("setpriority_requested=true", log.getvalue())

    def test_canonical_explicit_attestation_still_establishes_ni_zero(self):
        log = io.StringIO()
        with mock.patch.object(
            build_worker, "_algorithm_matrix_campaign_authority_status",
            return_value=({"name": "q1024"}, None),
        ), mock.patch.object(
            build_worker, "_e2e_config_requests_nice_attestation",
            return_value=True,
        ), mock.patch.object(
            build_worker.host_platform, "system", return_value="Linux",
        ), mock.patch.object(
            build_worker.os, "setpriority",
        ) as setpriority, mock.patch.object(
            build_worker.os, "getpriority", return_value=0,
        ) as getpriority:
            build_worker._ensure_algorithm_matrix_launch_nice(
                "benchmark-e2e", self.CONFIG, log)

        setpriority.assert_called_once_with(build_worker.os.PRIO_PROCESS, 0, 0)
        getpriority.assert_called_once_with(build_worker.os.PRIO_PROCESS, 0)
        self.assertIn(
            "authority=canonical_explicit_e2e_attestation", log.getvalue())

    def test_explicit_attestation_rejects_nonzero_nice(self):
        with mock.patch.object(
            build_worker, "_algorithm_matrix_campaign_authority_status",
            return_value=(None, None),
        ), mock.patch.object(
            build_worker, "_e2e_config_requests_nice_attestation",
            return_value=True,
        ), mock.patch.object(
            build_worker.host_platform, "system", return_value="Darwin",
        ), mock.patch.object(
            build_worker.os, "setpriority",
        ), mock.patch.object(
            build_worker.os, "getpriority", return_value=1,
        ):
            with self.assertRaisesRegex(RuntimeError, "verified worker NI=0"):
                build_worker._ensure_algorithm_matrix_launch_nice(
                    "benchmark-e2e", self.CONFIG, io.StringIO())

    def test_explicit_attestation_rejects_priority_api_errors(self):
        for api_name in ("setpriority", "getpriority"):
            with self.subTest(api=api_name), mock.patch.object(
                build_worker, "_algorithm_matrix_campaign_authority_status",
                return_value=(None, None),
            ), mock.patch.object(
                build_worker, "_e2e_config_requests_nice_attestation",
                return_value=True,
            ), mock.patch.object(
                build_worker.host_platform, "system", return_value="Darwin",
            ), mock.patch.object(
                build_worker.os, "setpriority",
                side_effect=OSError("denied") if api_name == "setpriority" else None,
            ), mock.patch.object(
                build_worker.os, "getpriority",
                side_effect=OSError("unavailable") if api_name == "getpriority" else None,
                return_value=0,
            ):
                with self.assertRaisesRegex(RuntimeError, "could not establish"):
                    build_worker._ensure_algorithm_matrix_launch_nice(
                        "benchmark-e2e", self.CONFIG, io.StringIO())

    def test_campaign_launch_rejects_unobservable_inherited_nice(self):
        with mock.patch.object(
            build_worker, "_algorithm_matrix_campaign_authority_status",
            return_value=({"name": "q1024"}, None),
        ), mock.patch.object(
            build_worker, "_e2e_config_requests_nice_attestation",
            return_value=False,
        ), mock.patch.object(
            build_worker.host_platform, "system", return_value="Linux",
        ), mock.patch.object(
            build_worker.os, "setpriority",
        ) as setpriority, mock.patch.object(
            build_worker.os, "getpriority", side_effect=OSError("unavailable"),
        ):
            with self.assertRaisesRegex(
                    RuntimeError, "could not observe inherited worker nice"):
                build_worker._ensure_algorithm_matrix_launch_nice(
                    "benchmark-e2e", self.CONFIG, io.StringIO())
        setpriority.assert_not_called()

    def test_priority_gate_does_not_touch_non_campaign_paths(self):
        for mode, authority, system_name in (
            ("benchmark", {"name": "q1024"}, "Darwin"),
            ("benchmark-e2e", None, "Darwin"),
            ("benchmark-e2e", None, "Linux"),
        ):
            with self.subTest(mode=mode, authority=authority, system=system_name), \
                    mock.patch.object(
                        build_worker, "_algorithm_matrix_campaign_authority_status",
                        return_value=(authority, None),
                    ), mock.patch.object(
                        build_worker.host_platform, "system", return_value=system_name,
                    ), mock.patch.object(
                        build_worker,
                        "_e2e_config_requests_nice_attestation",
                        return_value=False,
                    ), mock.patch.object(
                        build_worker.os, "setpriority",
                    ) as setpriority, mock.patch.object(
                        build_worker.os, "getpriority",
                    ) as getpriority:
                build_worker._ensure_algorithm_matrix_launch_nice(
                    mode, self.CONFIG, io.StringIO())
                setpriority.assert_not_called()
                getpriority.assert_not_called()



class OrdinaryBenchE2eFilterTests(unittest.TestCase):
    def test_omitted_filter_selects_exact_preexisting_e2e_suite(self):
        bench_filter = build_worker._ordinary_bench_e2e_default_filter(
            "benchmark-e2e",
            None,
            build_only=False,
            debug_report=False,
            filter_supplied=False,
        )

        self.assertEqual(
            bench_filter,
            {
                "benchmarks": [
                    "bench_e2e",
                    "bench_e2e_signoff",
                    "bench_e2e_gpu_batch_validation",
                    "bench_e2e_routing_poc",
                    "bench_sort_scan",
                ],
            },
        )
        cli_args, error = build_presets._filter_to_cli_args(bench_filter)
        self.assertIsNone(error)
        self.assertEqual(
            cli_args,
            [
                "--filter",
                "benchmark=bench_e2e,bench_e2e_signoff,"
                "bench_e2e_gpu_batch_validation,bench_e2e_routing_poc,"
                "bench_sort_scan",
            ],
        )

    def test_explicit_filter_is_preserved(self):
        explicit = {
            "benchmarks": ["bench_sql_codegen_guardrail"],
        }

        actual = build_worker._ordinary_bench_e2e_default_filter(
            "benchmark-e2e",
            explicit,
            build_only=False,
            debug_report=False,
            filter_supplied=True,
        )

        self.assertIs(actual, explicit)

    def test_explicit_null_filter_is_not_reinterpreted(self):
        actual = build_worker._ordinary_bench_e2e_default_filter(
            "benchmark-e2e",
            None,
            build_only=False,
            debug_report=False,
            filter_supplied=True,
        )

        self.assertIsNone(actual)


    def test_other_benchmark_modes_remain_unfiltered(self):
        actual = build_worker._ordinary_bench_e2e_default_filter(
            "benchmark",
            None,
            build_only=False,
            debug_report=False,
            filter_supplied=False,
        )

        self.assertIsNone(actual)

    def test_build_only_and_unfocused_debug_keep_no_runtime_filter(self):
        for build_only, debug_report in (
            (True, False),
            (False, True),
        ):
            with self.subTest(
                    build_only=build_only,
                    debug_report=debug_report):
                actual = build_worker._ordinary_bench_e2e_default_filter(
                    "benchmark-e2e",
                    None,
                    build_only=build_only,
                    debug_report=debug_report,
                    filter_supplied=False,
                )

                self.assertIsNone(actual)


class UnitIntegrationQualificationTests(unittest.TestCase):
    def test_builder_co_schedules_independent_compile_graphs(self):
        mode = build_presets.MODES["unit-integration"]

        self.assertTrue(mode.build_targets_together)
        self.assertEqual(
            build_worker._build_target_groups_for("unit-integration"),
            (tuple(mode.targets),),
        )
        self.assertEqual(
            build_worker._cmake_build_argv(
                Path("/tmp/test-opt"), tuple(mode.targets)),
            [
                "cmake", "--build", "/tmp/test-opt", "--target",
                *mode.targets, "--parallel", str(build_worker.CMAKE_PARALLEL_JOBS),
            ],
        )
        self.assertFalse(build_presets.MODES["benchmark"].build_targets_together)
        self.assertEqual(
            build_worker._build_target_groups_for("benchmark"),
            tuple((target,) for target in build_presets.MODES["benchmark"].targets),
        )

    def test_builder_builds_and_runs_e2e_serializer_contract(self):
        mode = build_presets.MODES["unit-integration"]

        self.assertIn("bioseq_benchmark_e2e_contract_tests", mode.targets)
        self.assertIn(
            "bioseq_benchmark_e2e_contract_tests",
            [command[0] for command in mode.commands],
        )

    def test_builder_builds_and_runs_benchmark_framework_contracts(self):
        mode = build_presets.MODES["unit-integration"]

        self.assertIn(
            "bioseq_benchmark_framework_contract_tests", mode.targets)
        self.assertIn(
            "bioseq_benchmark_framework_contract_tests",
            [command[0] for command in mode.commands],
        )

    def test_focused_filter_allows_zero_matches_in_every_correctness_binary(self):
        test_flags, error = build_presets._unit_test_filter_to_cli_args(
            {"tags": ["[grouped-soa]"]})

        self.assertIsNone(error)
        self.assertEqual(
            test_flags,
            ["--allow-running-no-tests", "[grouped-soa]"],
        )
        expected_command_counts = {
            "unit-integration": 4,
            build_presets.ALL_ALPHABETS_UNIT_INTEGRATION_MODE: 4,
            "cache-unit": 5,
            "cache-integration": 1,
            "cache-bioseq-integration": 2,
        }
        self.assertEqual(
            build_presets.CATCH_FILTER_MODES,
            frozenset(expected_command_counts),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact_values = {}
            for key in build_worker._CACHE_NATIVE_ARTIFACT_KEYS:
                path = root / "artifacts" / key
                if key.endswith(("_postgres_bin_dir", "_postgres_stage_root")):
                    path.mkdir(parents=True)
                else:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(b"sealed artifact")
                artifact_values[key] = str(path)
            for mode, expected_count in expected_command_counts.items():
                with self.subTest(mode=mode):
                    binary_dir = root / build_presets.MODES[mode].binary_subdir
                    with mock.patch.object(
                        build_worker,
                        "_cache_native_artifacts",
                        return_value=artifact_values,
                    ):
                        resolved_commands = [
                            build_worker._run_command_argv(
                                mode,
                                binary_dir,
                                command,
                                test_flags=test_flags,
                                report_path=root / "report.json",
                            )
                            for command in build_presets.MODES[mode].commands
                        ]
                    self.assertEqual(len(resolved_commands), expected_count)
                    for command in resolved_commands:
                        self.assertEqual(command[-2:], test_flags)

    def test_all_focused_correctness_modes_support_receipt_pinned_run_only(self):
        self.assertLessEqual(
            build_presets.CATCH_FILTER_MODES,
            build_worker._RUN_ONLY_RECEIPT_MODES,
        )


class UnitIntegrationCacheIsolationTests(unittest.TestCase):
    def test_optional_component_mode_invariants_are_explicit(self):
        self.assertEqual(
            build_presets.MODES[
                build_presets.ALL_ALPHABETS_UNIT_INTEGRATION_MODE
            ].required_cache_variables,
            {
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
                "BIOSEQ_BUILD_WEBSITE": "OFF",
                "BIOSEQ_TEMPLATE_FLATTEN_PROFILE": "all_alphabets_uint64",
            },
        )
        self.assertEqual(
            build_presets.MODES["unit-integration"].required_cache_variables,
            {
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
                "BIOSEQ_BUILD_WEBSITE": "OFF",
            },
        )
        self.assertEqual(
            build_presets.MODES["external-api"].required_cache_variables,
            {
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "ON",
                "BIOSEQ_BUILD_WEBSITE": "OFF",
                "BIOSEQ_WEBSITE_BROWSER_TESTS": "OFF",
            },
        )
        self.assertEqual(
            build_presets.MODES["website"].required_cache_variables,
            {
                "BIOSEQ_BUILD_WEBSITE": "ON",
                "BIOSEQ_BUILD_CACHE": "ON",
                "BIOSEQ_WEBSITE_BROWSER_TESTS": "ON",
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "ON",
            },
        )

    def _run_optional_transition(
            self, mode, preset, cache_entries):
        commands = []
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / preset
            build_dir.mkdir()
            (build_dir / "build.ninja").touch()
            cache = build_dir / "CMakeCache.txt"
            cache.write_text(
                "BIOSEQ_TEST_INDEXES:BOOL=OFF\n" + "".join(
                    f"{name}:BOOL={value}\n"
                    for name, value in cache_entries.items()),
                encoding="utf-8",
            )
            configure_stamp = {
                "artifact_kind": build_presets._CMAKE_CONFIGURE_STAMP_KIND,
                "schema_revision": 1,
                "inputs": {"preset": preset},
                "inputs_sha256": "0" * 64,
            }
            (build_dir / build_presets._CMAKE_CONFIGURE_STAMP_NAME).write_text(
                json.dumps(configure_stamp),
                encoding="utf-8",
            )
            log_path = Path(temporary) / "worker.log"
            state = {
                mode: {
                    "started_at": time.time(),
                    "log_file": str(log_path),
                    "rt_priority": None,
                },
            }

            def run(command, *_args, **_kwargs):
                commands.append(list(command))
                return 0

            with mock.patch.multiple(
                    build_worker,
                    _refuse_root_build_identity=mock.DEFAULT,
                    _ensure_build_worker_timesharing_policy=mock.DEFAULT,
                    _load_state=mock.DEFAULT,
                    _update_entry=mock.DEFAULT,
                    preset_for=mock.DEFAULT,
                    build_dir_for=mock.DEFAULT,
                    _build_worker_descendant_environment=mock.DEFAULT,
                    _ensure_local_toolchain_environment=mock.DEFAULT,
                    _configured_local_build_rt_priority=mock.DEFAULT,
                    _local_clang_rt_is_enabled=mock.DEFAULT,
                    _preflight_local_clang_rt_launcher=mock.DEFAULT,
                    _stream_subprocess=mock.DEFAULT,
                    _clang_rt_cmake_cache_repairs=mock.DEFAULT,
                    _cmake_preset_cache_contract=mock.DEFAULT,
                    _cmake_preset_cache_repairs=mock.DEFAULT,
                    _postgresql_discovery_cache_reset_args=mock.DEFAULT,
                    _cmake_configure_stamp_document=mock.DEFAULT,
                    ensure_exact_doxygen=mock.DEFAULT,
            ) as patched:
                patched["_refuse_root_build_identity"].return_value = None
                patched["_load_state"].return_value = state
                patched["preset_for"].return_value = preset
                patched["build_dir_for"].return_value = build_dir
                patched[
                    "_build_worker_descendant_environment"
                ].return_value = {"PATH": os.environ.get("PATH", "")}
                patched["_configured_local_build_rt_priority"].return_value = None
                patched["_local_clang_rt_is_enabled"].return_value = False
                patched["_stream_subprocess"].side_effect = run
                patched["_clang_rt_cmake_cache_repairs"].return_value = ([], [])
                patched["_cmake_preset_cache_contract"].return_value = {}
                patched["_cmake_preset_cache_repairs"].return_value = {}
                patched[
                    "_postgresql_discovery_cache_reset_args"
                ].return_value = ()
                patched["_cmake_configure_stamp_document"].return_value = (
                    configure_stamp)
                patched["ensure_exact_doxygen"].return_value = (
                    doxygen_toolchain.ProvisionedDoxygen(
                        executable=Path(
                            "/managed/doxygen/1.9.8/bin/doxygen"),
                        version="1.9.8",
                        version_output="1.9.8",
                        platform_name="linux",
                        architecture="x86_64",
                        archive_sha256="1" * 64,
                        executable_sha256="2" * 64,
                        cache_hit=True,
                    )
                )

                result = build_worker._run_build_worker(mode, build_only=True)

            self.assertEqual(result, 0, log_path.read_text(encoding="utf-8"))
        return commands

    def test_website_to_external_transition_is_atomic(self):
        commands = self._run_optional_transition(
            "external-api",
            build_presets.EXTERNAL_API_BUILD_PRESET,
            {
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
                "BIOSEQ_BUILD_WEBSITE": "ON",
                "BIOSEQ_BUILD_CACHE": "ON",
                "BIOSEQ_WEBSITE_BROWSER_TESTS": "ON",
            },
        )
        self.assertEqual(
            commands[0][:3],
            ["cmake", "--preset", build_presets.EXTERNAL_API_BUILD_PRESET],
        )
        self.assertEqual(
            set(commands[0][3:]),
            {
                "-DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON",
                "-DBIOSEQ_BUILD_WEBSITE=OFF",
                "-DBIOSEQ_WEBSITE_BROWSER_TESTS=OFF",
            },
        )
        self.assertEqual(commands[1][:2], ["cmake", "--build"])

    def test_external_to_website_transition_is_atomic(self):
        commands = self._run_optional_transition(
            "website",
            build_presets.WEBSITE_BUILD_PRESET,
            {
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "ON",
                "BIOSEQ_BUILD_WEBSITE": "OFF",
                "BIOSEQ_BUILD_CACHE": "OFF",
                "BIOSEQ_WEBSITE_BROWSER_TESTS": "OFF",
            },
        )
        self.assertEqual(
            commands[0][:3],
            ["cmake", "--preset", build_presets.WEBSITE_BUILD_PRESET],
        )
        self.assertEqual(
            set(commands[0][3:]),
            {
                "-DBIOSEQ_BUILD_WEBSITE=ON",
                "-DBIOSEQ_BUILD_CACHE=ON",
                "-DBIOSEQ_WEBSITE_BROWSER_TESTS=ON",
                "-DDOXYGEN_EXECUTABLE=/managed/doxygen/1.9.8/bin/doxygen",
            },
        )
        self.assertEqual(commands[1][:2], ["cmake", "--build"])

    def test_website_repairs_a_legacy_external_disabled_tree_before_build(self):
        commands = self._run_optional_transition(
            "website",
            build_presets.WEBSITE_BUILD_PRESET,
            {
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
                "BIOSEQ_BUILD_WEBSITE": "ON",
                "BIOSEQ_BUILD_CACHE": "ON",
                "BIOSEQ_WEBSITE_BROWSER_TESTS": "ON",
            },
        )
        self.assertEqual(
            commands[0][:3],
            ["cmake", "--preset", build_presets.WEBSITE_BUILD_PRESET],
        )
        self.assertIn("-DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON", commands[0])
        self.assertNotIn("-DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=OFF", commands[0])
        self.assertEqual(commands[1][:2], ["cmake", "--build"])

    def test_external_and_website_cannot_share_a_live_build_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            shared_tree = Path(temporary) / "shared-opt"
            shared_tree.mkdir()
            shared_alias = Path(temporary) / "shared-alias"
            try:
                shared_alias.symlink_to(shared_tree, target_is_directory=True)
            except OSError:
                # Windows test hosts may deny unprivileged symlink creation;
                # exact path identity still pins the scheduler contract there.
                shared_alias = shared_tree

            cases = (
                ("external-api", "website", shared_alias),
                ("website", "external-api", shared_tree),
            )
            for requested_mode, running_mode, recorded_tree in cases:
                with self.subTest(
                        requested=requested_mode, running=running_mode):
                    state = {
                        running_mode: {
                            "status": "running",
                            "build_only": True,
                            "debug": False,
                            "build_dir": str(recorded_tree),
                        },
                    }
                    with mock.patch.object(
                            build_worker, "_refresh",
                            side_effect=lambda current, candidate,
                            service_boundary_provider=None:
                            current.get(candidate)):
                        conflict_mode, conflict_entry = (
                            build_worker._running_in_same_group(
                                state, requested_mode, True, shared_tree))

                    self.assertEqual(conflict_mode, running_mode)
                    self.assertIs(conflict_entry, state[running_mode])

    def test_running_entry_without_paths_uses_recorded_debug_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            shared_debug_tree = Path(temporary) / "shared-debug"
            state = {
                "external-api": {
                    "status": "running",
                    "build_only": True,
                    "debug": True,
                },
            }

            def fallback_build_dirs(mode, debug):
                self.assertEqual((mode, debug), ("external-api", True))
                return (shared_debug_tree,)

            with mock.patch.object(
                    build_worker, "_refresh",
                    side_effect=lambda current, candidate,
                    service_boundary_provider=None:
                    current.get(candidate)), mock.patch.object(
                        build_worker, "build_dirs_for",
                        side_effect=fallback_build_dirs):
                conflict_mode, conflict_entry = build_worker._running_in_same_group(
                    state, "website", True, shared_debug_tree)

            self.assertEqual(conflict_mode, "external-api")
            self.assertIs(conflict_entry, state["external-api"])

    def test_different_optional_mode_trees_remain_independent(self):
        with tempfile.TemporaryDirectory() as temporary:
            external_tree = Path(temporary) / "external-opt"
            website_tree = Path(temporary) / "website-opt"
            state = {
                "website": {
                    "status": "running",
                    "build_only": True,
                    "debug": False,
                    "build_dir": str(website_tree),
                },
            }
            with mock.patch.object(
                    build_worker, "_refresh",
                    side_effect=lambda current, candidate,
                    service_boundary_provider=None:
                    current.get(candidate)):
                conflict = build_worker._running_in_same_group(
                    state, "external-api", True, external_tree)

            self.assertEqual(conflict, (None, None))

    def test_doubly_polluted_tree_is_repaired_in_first_configure(self):
        commands = []
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "test-opt"
            build_dir.mkdir()
            (build_dir / "build.ninja").touch()
            cache = build_dir / "CMakeCache.txt"
            cache.write_text(
                "BIOSEQ_TEST_INDEXES:BOOL=OFF\n"
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS:BOOL=ON\n"
                "BIOSEQ_BUILD_WEBSITE:BOOL=ON\n",
                encoding="utf-8",
            )
            log_path = Path(temporary) / "worker.log"
            state = {
                "unit-integration": {
                    "started_at": time.time(),
                    "log_file": str(log_path),
                    "rt_priority": None,
                },
            }

            def run(command, *_args, **_kwargs):
                command = list(command)
                commands.append(command)
                if command[:3] == ["cmake", "--preset", "test-opt"]:
                    cache.write_text(
                        "BIOSEQ_TEST_INDEXES:BOOL=ON\n"
                        "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS:BOOL=OFF\n"
                        "BIOSEQ_BUILD_WEBSITE:BOOL=OFF\n",
                        encoding="utf-8",
                    )
                return 0

            with mock.patch.multiple(
                    build_worker,
                    _refuse_root_build_identity=mock.DEFAULT,
                    _ensure_build_worker_timesharing_policy=mock.DEFAULT,
                    _load_state=mock.DEFAULT,
                    _update_entry=mock.DEFAULT,
                    preset_for=mock.DEFAULT,
                    build_dir_for=mock.DEFAULT,
                    _build_worker_descendant_environment=mock.DEFAULT,
                    _ensure_local_toolchain_environment=mock.DEFAULT,
                    _configured_local_build_rt_priority=mock.DEFAULT,
                    _local_clang_rt_is_enabled=mock.DEFAULT,
                    _preflight_local_clang_rt_launcher=mock.DEFAULT,
                    _initial_clang_rt_cmake_args=mock.DEFAULT,
                    _stream_subprocess=mock.DEFAULT,
                    _clang_rt_cmake_cache_repairs=mock.DEFAULT,
                    _cmake_preset_cache_contract=mock.DEFAULT,
                    _cmake_preset_cache_repairs=mock.DEFAULT,
            ) as patched:
                patched["_refuse_root_build_identity"].return_value = None
                patched["_load_state"].return_value = state
                patched["preset_for"].return_value = "test-opt"
                patched["build_dir_for"].return_value = build_dir
                patched["_build_worker_descendant_environment"].return_value = {}
                patched["_configured_local_build_rt_priority"].return_value = None
                patched["_local_clang_rt_is_enabled"].return_value = False
                patched["_initial_clang_rt_cmake_args"].return_value = [
                    "-DCMAKE_C_COMPILER_LAUNCHER:FILEPATH=/trusted/launcher",
                    "-DCMAKE_CXX_COMPILER_LAUNCHER:FILEPATH=/trusted/launcher",
                ]
                patched["_stream_subprocess"].side_effect = run
                patched["_clang_rt_cmake_cache_repairs"].return_value = ([], [])
                patched["_cmake_preset_cache_contract"].return_value = {}
                patched["_cmake_preset_cache_repairs"].return_value = {}

                result = build_worker._run_build_worker(
                    "unit-integration", build_only=True)

            self.assertEqual(result, 0, log_path.read_text(encoding="utf-8"))

        repair = [
            "cmake", "--preset", "test-opt",
            "-DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=OFF",
            "-DBIOSEQ_BUILD_WEBSITE=OFF",
            "-DCMAKE_C_COMPILER_LAUNCHER:FILEPATH=/trusted/launcher",
            "-DCMAKE_CXX_COMPILER_LAUNCHER:FILEPATH=/trusted/launcher",
        ]
        self.assertEqual(commands[0], repair)
        first_build = next(
            index for index, command in enumerate(commands)
            if command[:2] == ["cmake", "--build"])
        self.assertLess(0, first_build)


class DoxygenToolchainProvisioningTests(unittest.TestCase):
    """Exact website Doxygen provisioning is locked, isolated, and atomic."""

    _EXECUTABLE_MEMBER = "doxygen-1.9.8/bin/doxygen"
    _EXECUTABLE_BYTES = b"#!/bin/sh\nprintf '1.9.8\\n'\n"

    def _tar_archive(
            self,
            *,
            executable_bytes: bytes | None = None,
            extra_members: list[tarfile.TarInfo] | None = None) -> bytes:
        executable_payload = (
            self._EXECUTABLE_BYTES
            if executable_bytes is None
            else executable_bytes
        )
        output = io.BytesIO()
        with tarfile.open(fileobj=output, mode="w:gz") as archive:
            executable = tarfile.TarInfo(self._EXECUTABLE_MEMBER)
            executable.mode = 0o755
            executable.size = len(executable_payload)
            archive.addfile(executable, io.BytesIO(executable_payload))
            for member in extra_members or []:
                archive.addfile(member)
        return output.getvalue()

    def _write_lock(
            self,
            root: Path,
            archive: bytes,
            *,
            executable_bytes: bytes | None = None) -> Path:
        executable_payload = (
            self._EXECUTABLE_BYTES
            if executable_bytes is None
            else executable_bytes
        )
        payload = json.loads(
            doxygen_toolchain.DOXYGEN_TOOLCHAIN_LOCK.read_text(
                encoding="utf-8"))
        linux = payload["platforms"]["linux-x86_64"]
        linux.update({
            "archive_sha256": hashlib.sha256(archive).hexdigest(),
            "archive_size_bytes": len(archive),
            "archive_url": (
                "https://github.com/doxygen/doxygen/releases/download/"
                "Release_1_9_8/test-doxygen.tar.gz"
            ),
            "executable_member": self._EXECUTABLE_MEMBER,
            "executable_sha256": hashlib.sha256(
                executable_payload).hexdigest(),
            "executable_size_bytes": len(executable_payload),
            "version_output": "1.9.8",
        })
        lock_path = root / "doxygen.lock.json"
        lock_path.write_text(json.dumps(payload), encoding="utf-8")
        return lock_path

    def _ensure_linux(
            self,
            cache_root: Path,
            lock_path: Path) -> doxygen_toolchain.ProvisionedDoxygen:
        return doxygen_toolchain.ensure_exact_doxygen(
            cache_root,
            platform_name="linux",
            machine_name="x86_64",
            lock_path=lock_path,
        )

    def test_production_lock_selects_darwin_and_linux_by_architecture(self):
        darwin_arm = doxygen_toolchain._select_asset(
            platform_name="Darwin",
            machine_name="arm64",
        )
        darwin_x64 = doxygen_toolchain._select_asset(
            platform_name="darwin",
            machine_name="AMD64",
        )
        linux_x64 = doxygen_toolchain._select_asset(
            platform_name="linux-gnu",
            machine_name="x86_64",
        )

        self.assertEqual(darwin_arm[2].host_key, "darwin-arm64")
        self.assertEqual(darwin_x64[2].host_key, "darwin-x86_64")
        self.assertEqual(linux_x64[2].host_key, "linux-x86_64")
        with self.assertRaisesRegex(
                doxygen_toolchain.DoxygenProvisioningError,
                "no locked Doxygen 1.9.8 artifact"):
            doxygen_toolchain._select_asset(
                platform_name="linux",
                machine_name="arm64",
            )

    def test_cold_cache_provisions_then_reuses_without_network(self):
        archive = self._tar_archive()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock_path = self._write_lock(root, archive)
            cache_root = root / "cache"
            with mock.patch.object(
                doxygen_toolchain.urllib.request,
                "urlopen",
                side_effect=lambda *_args, **_kwargs: io.BytesIO(archive),
            ) as download:
                first = self._ensure_linux(cache_root, lock_path)
            self.assertFalse(first.cache_hit)
            self.assertEqual(
                first.executable.read_bytes(),
                self._EXECUTABLE_BYTES,
            )
            self.assertEqual(first.version_output, "1.9.8")
            download.assert_called_once()

            with mock.patch.object(
                doxygen_toolchain.urllib.request,
                "urlopen",
                side_effect=AssertionError("network must not be used"),
            ) as download:
                second = self._ensure_linux(cache_root, lock_path)
            self.assertTrue(second.cache_hit)
            self.assertEqual(second.executable, first.executable)
            download.assert_not_called()

    def test_concurrent_callers_serialize_and_download_once(self):
        archive = self._tar_archive()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock_path = self._write_lock(root, archive)
            cache_root = root / "cache"
            download_started = threading.Event()
            release_download = threading.Event()
            second_lock_attempted = threading.Event()
            counter_lock = threading.Lock()
            download_count = 0
            exclusive_lock_attempts = 0
            results: list[doxygen_toolchain.ProvisionedDoxygen] = []
            errors: list[Exception] = []
            real_flock = doxygen_toolchain.fcntl.flock

            def tracked_flock(descriptor: int, operation: int):
                nonlocal exclusive_lock_attempts
                if operation == doxygen_toolchain.fcntl.LOCK_EX:
                    with counter_lock:
                        exclusive_lock_attempts += 1
                        if exclusive_lock_attempts == 2:
                            second_lock_attempted.set()
                return real_flock(descriptor, operation)

            def download(*_args, **_kwargs):
                nonlocal download_count
                with counter_lock:
                    download_count += 1
                download_started.set()
                if not release_download.wait(timeout=5.0):
                    raise OSError("timed out waiting to release test download")
                return io.BytesIO(archive)

            def provision() -> None:
                try:
                    results.append(self._ensure_linux(cache_root, lock_path))
                except Exception as exc:
                    errors.append(exc)

            first = threading.Thread(target=provision, daemon=True)
            second = threading.Thread(target=provision, daemon=True)
            download_was_started = False
            second_reached_lock = False
            download_count_while_locked = -1
            second_was_blocked = False
            with mock.patch.object(
                doxygen_toolchain.fcntl,
                "flock",
                side_effect=tracked_flock,
            ), mock.patch.object(
                doxygen_toolchain.urllib.request,
                "urlopen",
                side_effect=download,
            ):
                first.start()
                try:
                    download_was_started = download_started.wait(timeout=5.0)
                    if download_was_started:
                        second.start()
                        second_reached_lock = second_lock_attempted.wait(
                            timeout=5.0)
                        with counter_lock:
                            download_count_while_locked = download_count
                        second_was_blocked = second.is_alive()
                finally:
                    release_download.set()
                    first.join(timeout=5.0)
                    if second.ident is not None:
                        second.join(timeout=5.0)

            self.assertTrue(download_was_started)
            self.assertTrue(second_reached_lock)
            self.assertEqual(download_count_while_locked, 1)
            self.assertTrue(second_was_blocked)
            self.assertFalse(first.is_alive())
            self.assertFalse(second.is_alive())
            self.assertEqual(errors, [])
            self.assertEqual(exclusive_lock_attempts, 2)
            self.assertEqual(download_count, 1)
            self.assertEqual(
                sorted(result.cache_hit for result in results),
                [False, True],
            )

    def test_corrupt_executable_repairs_from_authenticated_archive(self):
        archive = self._tar_archive()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock_path = self._write_lock(root, archive)
            cache_root = root / "cache"
            with mock.patch.object(
                doxygen_toolchain.urllib.request,
                "urlopen",
                side_effect=lambda *_args, **_kwargs: io.BytesIO(archive),
            ):
                first = self._ensure_linux(cache_root, lock_path)
            first.executable.write_bytes(b"corrupt")
            first.executable.chmod(0o755)

            with mock.patch.object(
                doxygen_toolchain.urllib.request,
                "urlopen",
                side_effect=AssertionError("verified archive must be reused"),
            ) as download:
                repaired = self._ensure_linux(cache_root, lock_path)

            self.assertFalse(repaired.cache_hit)
            self.assertEqual(
                repaired.executable.read_bytes(), self._EXECUTABLE_BYTES)
            download.assert_not_called()

    def test_wrong_archive_hash_is_never_published(self):
        archive = self._tar_archive()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock_path = self._write_lock(root, archive)
            payload = json.loads(lock_path.read_text(encoding="utf-8"))
            payload["platforms"]["linux-x86_64"]["archive_sha256"] = "0" * 64
            lock_path.write_text(json.dumps(payload), encoding="utf-8")
            cache_root = root / "cache"
            with mock.patch.object(
                doxygen_toolchain.urllib.request,
                "urlopen",
                side_effect=lambda *_args, **_kwargs: io.BytesIO(archive),
            ), self.assertRaisesRegex(
                doxygen_toolchain.DoxygenProvisioningError,
                "archive SHA-256 differs",
            ):
                self._ensure_linux(cache_root, lock_path)

            archives = cache_root / "archives"
            self.assertEqual(list(archives.iterdir()), [])

    def test_wrong_archive_size_is_never_published(self):
        archive = self._tar_archive()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock_path = self._write_lock(root, archive)
            payload = json.loads(lock_path.read_text(encoding="utf-8"))
            payload["platforms"]["linux-x86_64"][
                "archive_size_bytes"] += 1
            lock_path.write_text(json.dumps(payload), encoding="utf-8")
            cache_root = root / "cache"
            with mock.patch.object(
                doxygen_toolchain.urllib.request,
                "urlopen",
                side_effect=lambda *_args, **_kwargs: io.BytesIO(archive),
            ), self.assertRaisesRegex(
                doxygen_toolchain.DoxygenProvisioningError,
                "archive byte count differs",
            ):
                self._ensure_linux(cache_root, lock_path)

            self.assertEqual(list((cache_root / "archives").iterdir()), [])

    def test_interrupted_publication_leaves_no_partial_cache_entry(self):
        archive = self._tar_archive()

        class InterruptedResponse:
            headers = None

            def __init__(self):
                self._first_read = True

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

            def read(self, _size: int) -> bytes:
                if self._first_read:
                    self._first_read = False
                    return archive[:16]
                raise OSError("injected interrupted archive download")

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock_path = self._write_lock(root, archive)
            cache_root = root / "archive-cache"
            with mock.patch.object(
                doxygen_toolchain.urllib.request,
                "urlopen",
                return_value=InterruptedResponse(),
            ), self.assertRaisesRegex(
                doxygen_toolchain.DoxygenProvisioningError,
                "cannot download locked Doxygen archive",
            ):
                self._ensure_linux(cache_root, lock_path)

            archive_directory = cache_root / "archives"
            self.assertEqual(list(archive_directory.iterdir()), [])

        def interrupt_executable(
                _source,
                destination_descriptor: int,
                _asset) -> None:
            os.write(destination_descriptor, b"partial executable")
            raise OSError("injected interrupted executable publication")

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock_path = self._write_lock(root, archive)
            cache_root = root / "executable-cache"
            with mock.patch.object(
                doxygen_toolchain.urllib.request,
                "urlopen",
                side_effect=lambda *_args, **_kwargs: io.BytesIO(archive),
            ), mock.patch.object(
                doxygen_toolchain,
                "_copy_verified_executable",
                side_effect=interrupt_executable,
            ), self.assertRaisesRegex(
                doxygen_toolchain.DoxygenProvisioningError,
                "cannot extract locked Doxygen tar archive",
            ):
                self._ensure_linux(cache_root, lock_path)

            executable_directory = (
                cache_root / "1.9.8/linux-x86_64/bin"
            )
            self.assertFalse((executable_directory / "doxygen").exists())
            self.assertEqual(list(executable_directory.iterdir()), [])

    def test_tar_member_size_and_sha_mismatches_are_not_published(self):
        wrong_size = self._EXECUTABLE_BYTES[:-1]
        wrong_sha = bytes([
            self._EXECUTABLE_BYTES[0] ^ 1,
        ]) + self._EXECUTABLE_BYTES[1:]
        for executable_bytes, diagnostic in (
            (wrong_size, "tar member size differs"),
            (wrong_sha, "executable SHA-256 differs"),
        ):
            with self.subTest(diagnostic=diagnostic):
                archive = self._tar_archive(
                    executable_bytes=executable_bytes)
                with tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    lock_path = self._write_lock(root, archive)
                    cache_root = root / "cache"
                    with mock.patch.object(
                        doxygen_toolchain.urllib.request,
                        "urlopen",
                        side_effect=(
                            lambda *_args, **_kwargs: io.BytesIO(archive)),
                    ), self.assertRaisesRegex(
                        doxygen_toolchain.DoxygenProvisioningError,
                        diagnostic,
                    ):
                        self._ensure_linux(cache_root, lock_path)

                    executable = (
                        cache_root / "1.9.8/linux-x86_64/bin/doxygen"
                    )
                    self.assertFalse(executable.exists())

    def test_wrong_runtime_version_is_rejected_and_not_retained(self):
        wrong_executable = b"#!/bin/sh\nprintf '1.9.9\\n'\n"
        archive = self._tar_archive(executable_bytes=wrong_executable)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock_path = self._write_lock(
                root,
                archive,
                executable_bytes=wrong_executable,
            )
            cache_root = root / "cache"
            with mock.patch.object(
                doxygen_toolchain.urllib.request,
                "urlopen",
                side_effect=lambda *_args, **_kwargs: io.BytesIO(archive),
            ), self.assertRaisesRegex(
                doxygen_toolchain.DoxygenProvisioningError,
                "version identity differs",
            ):
                self._ensure_linux(cache_root, lock_path)

            executable = (
                cache_root
                / "1.9.8"
                / "linux-x86_64"
                / "bin"
                / "doxygen"
            )
            self.assertFalse(executable.exists())

    def test_tar_traversal_and_symlink_members_are_rejected(self):
        traversal = tarfile.TarInfo("../escape")
        traversal.size = 0
        symlink = tarfile.TarInfo("doxygen-1.9.8/bin/alias")
        symlink.type = tarfile.SYMTYPE
        symlink.linkname = self._EXECUTABLE_MEMBER

        for unsafe, diagnostic in (
            (traversal, "unsafe member"),
            (symlink, "link, device, or unsupported member"),
        ):
            with self.subTest(diagnostic=diagnostic):
                archive = self._tar_archive(extra_members=[unsafe])
                with tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    lock_path = self._write_lock(root, archive)
                    with mock.patch.object(
                        doxygen_toolchain.urllib.request,
                        "urlopen",
                        side_effect=(
                            lambda *_args, **_kwargs: io.BytesIO(archive)),
                    ), self.assertRaisesRegex(
                        doxygen_toolchain.DoxygenProvisioningError,
                        diagnostic,
                    ):
                        self._ensure_linux(root / "cache", lock_path)
                    self.assertFalse((root / "escape").exists())

    def test_symlink_lock_and_cache_root_are_rejected(self):
        archive = self._tar_archive()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            real_lock = self._write_lock(root, archive)
            lock_alias = root / "lock-alias.json"
            lock_alias.symlink_to(real_lock)
            with self.assertRaisesRegex(
                    doxygen_toolchain.DoxygenProvisioningError,
                    "non-symlink file"):
                doxygen_toolchain.load_doxygen_toolchain_lock(lock_alias)

            real_cache = root / "real-cache"
            real_cache.mkdir()
            cache_alias = root / "cache-alias"
            cache_alias.symlink_to(real_cache, target_is_directory=True)
            with self.assertRaisesRegex(
                    doxygen_toolchain.DoxygenProvisioningError,
                    "not a symlink"):
                self._ensure_linux(cache_alias, real_lock)

    def test_rosetta_failure_is_actionable_on_apple_silicon(self):
        asset = doxygen_toolchain.load_doxygen_toolchain_lock()[
            "darwin-arm64"]
        failed = subprocess.CompletedProcess(
            ["/usr/bin/arch"], 1, stdout=b"", stderr=b"missing")
        with mock.patch.object(
            doxygen_toolchain.subprocess,
            "run",
            return_value=failed,
        ), self.assertRaisesRegex(
            doxygen_toolchain.DoxygenProvisioningError,
            "softwareupdate --install-rosetta --agree-to-license",
        ):
            doxygen_toolchain._require_rosetta_if_needed(
                "darwin", "arm64", asset)

    def test_only_website_mode_prepares_doxygen(self):
        provisioned = doxygen_toolchain.ProvisionedDoxygen(
            executable=Path("/cache/doxygen/1.9.8/bin/doxygen"),
            version="1.9.8",
            version_output="1.9.8",
            platform_name="linux",
            architecture="x86_64",
            archive_sha256="1" * 64,
            executable_sha256="2" * 64,
            cache_hit=True,
        )
        environment = {"PATH": "/usr/bin:/bin"}
        log = io.StringIO()
        with mock.patch.object(
            build_worker,
            "ensure_exact_doxygen",
            return_value=provisioned,
        ) as ensure:
            cache, identity = build_worker._prepare_mode_toolchain(
                "website",
                environment=environment,
                log_handle=log,
            )
            ordinary_cache, ordinary_identity = (
                build_worker._prepare_mode_toolchain(
                    "unit-integration",
                    environment=environment,
                    log_handle=log,
                )
            )

        ensure.assert_called_once_with()
        self.assertEqual(
            cache,
            {"DOXYGEN_EXECUTABLE": str(provisioned.executable)},
        )
        self.assertEqual(identity["doxygen"], provisioned.configure_identity())
        self.assertEqual(ordinary_cache, {})
        self.assertEqual(ordinary_identity, {})
        self.assertEqual(
            environment["PATH"].split(os.pathsep)[0],
            str(provisioned.executable.parent),
        )

    def test_exact_cmake_policy_and_bootstrap_prewarm_are_static_contracts(self):
        website_cmake = (REPOSITORY_ROOT / "website/CMakeLists.txt").read_text(
            encoding="utf-8")
        bootstrap_script = (
            REPOSITORY_ROOT / "infra/testing_infra/scripts/bootstrap.sh"
        ).read_text(encoding="utf-8")

        self.assertEqual(
            website_cmake.count(
                "find_package(Doxygen 1.9.8 EXACT REQUIRED)"),
            1,
        )
        self.assertNotIn("file(DOWNLOAD", website_cmake)
        self.assertIn("build_mcp/doxygen_toolchain.py", bootstrap_script)
        self.assertIn(
            '[ "$DOXYGEN_VERSION_OUTPUT" = "1.9.8" ]',
            bootstrap_script,
        )
        syntax = subprocess.run(
            ["bash", "-n", str(
                REPOSITORY_ROOT
                / "infra/testing_infra/scripts/bootstrap.sh")],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=10.0,
        )
        self.assertEqual(
            syntax.returncode,
            0,
            syntax.stderr.decode("utf-8", errors="replace"),
        )


class RemoteRunWebsiteRejectionTests(unittest.TestCase):
    """website is a local build-tree-only target (Doxygen + private PostgreSQL
    + pinned browser); remote-run must refuse it explicitly."""

    def test_website_is_a_valid_build_mode(self):
        # It must be a real mode, otherwise the rejection below is meaningless
        # (it would be caught by the generic invalid-mode guard instead).
        self.assertIn("website", build_presets.VALID_MODES)

    def test_website_is_registered_as_rejected(self):
        self.assertIn("website", build_presets.REMOTE_RUN_REJECTED_MODES)

    def test_remote_run_build_rejects_website(self):
        message = remote.tool_remote_run({"operation": "build", "mode": "website"})

        self.assertIn("not supported by remote-run", message)
        self.assertIn("start_build mode=website", message)

    def test_remote_run_does_not_reject_ordinary_modes(self):
        self.assertNotIn("unit-integration", build_presets.REMOTE_RUN_REJECTED_MODES)
        self.assertIn("unit-integration", build_presets.REMOTE_RUN_BUILD_MODES)


class RemoteActivityReservationTests(unittest.TestCase):
    def test_remote_slot_is_reserved_before_worker_spawn(self):
        process = SimpleNamespace(pid=43210)
        saved = []
        running = {
            "op_id": "observed",
            "kind": "status",
            "label": "status",
            "status": "running",
            "step": "running",
            "started_at": time.time(),
            "pid": process.pid,
            "log_file": "/tmp/remote.log",
        }
        with mock.patch.object(
            remote, "_remote_global_lock",
        ), mock.patch.object(
            remote, "_load_remote_state", return_value={},
        ), mock.patch.object(
            remote, "_refresh_remote", side_effect=lambda value: value,
        ), mock.patch.object(
            remote, "_save_remote_state",
            side_effect=lambda value: saved.append(dict(value)),
        ), mock.patch.object(
            remote.subprocess, "Popen", return_value=process,
        ), mock.patch.object(
            remote, "_remote_grace_wait", return_value=running,
        ):
            remote._start_or_poll_remote(
                "status", "status", argv=["provisioner", "status"])

        self.assertEqual(saved[0]["pid"], os.getpid())
        self.assertEqual(saved[0]["step"], "spawning")
        self.assertEqual(saved[1]["pid"], process.pid)

    def test_remote_worker_waits_for_exact_pid_handoff(self):
        op_id = "status-test"
        started = time.time()
        with tempfile.TemporaryDirectory() as temporary:
            log_file = str(Path(temporary) / "remote.log")
            scheduler_state = {
                "op_id": op_id,
                "status": "running",
                "pid": os.getpid() + 100000,
            }
            worker_state = {
                "op_id": op_id,
                "kind": "status",
                "label": "status",
                "status": "running",
                "step": "spawning",
                "started_at": started,
                "pid": os.getpid(),
                "log_file": log_file,
                "timeout_sec": 60,
                "argv": ["provisioner", "status"],
            }
            with mock.patch.object(
                remote,
                "_load_remote_state",
                side_effect=(scheduler_state, worker_state),
            ) as load, mock.patch.object(
                remote.time, "sleep",
            ) as sleeper, mock.patch.object(
                remote, "_update_remote_entry",
            ), mock.patch.object(
                remote, "_remote_exec", return_value=0,
            ):
                self.assertEqual(remote._run_remote_worker(op_id), 0)

        self.assertEqual(load.call_count, 2)
        sleeper.assert_called_once_with(0.1)


class CanonicalClangE2EReceiptPortabilityTests(unittest.TestCase):
    @staticmethod
    def _receipt(
        build_dir: Path,
        *,
        preset: str,
        system: str,
        machine: str,
        target_architecture: str | None = None,
        target_architecture_type: str = "STRING",
        target_triplet: str | None = None,
        intervals: str = "ON",
        test_indexes: str = "OFF",
        profile: str = "all_alphabets_uint64",
        compiler: str = "/opt/llvm-22/bin/clang++",
        compiler_id: str | None = "Clang",
        compiler_version: str | None = "22.1.8",
        flags: str = (
            "-O3 -DNDEBUG -fno-omit-frame-pointer -stdlib=libc++"),
    ) -> dict[str, object]:
        build_dir.mkdir(parents=True)
        cache = [
            f"BIOSEQ_BENCHMARK_E2E_INTERVALS:BOOL={intervals}",
            f"BIOSEQ_TEST_INDEXES:BOOL={test_indexes}",
            f"BIOSEQ_TEMPLATE_FLATTEN_PROFILE:STRING={profile}",
        ]
        if target_architecture is not None:
            cache.append(
                "VCPKG_TARGET_ARCHITECTURE:"
                + target_architecture_type + "=" + target_architecture)
        if target_triplet is not None:
            cache.append("VCPKG_TARGET_TRIPLET:STRING=" + target_triplet)
        (build_dir / "CMakeCache.txt").write_text(
            "\n".join(cache) + "\n", encoding="utf-8")
        return {
            "mode": "benchmark-e2e",
            "debug": False,
            "preset": preset,
            "preset_family": "e2e",
            "binary_subdir": "bioseq",
            "build_dir": str(build_dir),
            "host": {
                "node": "worker",
                "system": system,
                "machine": machine,
                "release": "test",
            },
            "configuration": {
                "compiler_contract": {
                    "CMAKE_CXX_COMPILER": compiler,
                    "CMAKE_CXX_COMPILER_ID": compiler_id,
                    "CMAKE_CXX_COMPILER_VERSION": compiler_version,
                    "CMAKE_CXX_FLAGS": flags,
                },
            },
        }

    def test_accepts_each_supported_native_release_clang_e2e_identity(self):
        cases = (
            ("bench-e2e-opt", "Darwin", "arm64", None, None),
            (
                "aws-x64-linux-e2e", "Linux", "x86_64", "x64",
                "x64-linux-libcxx",
            ),
            (
                "native-arm64-clang22-linux-e2e", "Linux", "aarch64",
                "arm64", "arm64-linux-libcxx",
            ),
            (
                "native-x64-clang22-linux-e2e", "Linux", "x86_64",
                "x64", "x64-linux-libcxx",
            ),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for index, (preset, system, machine, arch, triplet) in enumerate(
                    cases):
                with self.subTest(preset=preset):
                    receipt = self._receipt(
                        root / str(index),
                        preset=preset,
                        system=system,
                        machine=machine,
                        target_architecture=arch,
                        target_triplet=triplet,
                    )
                    self.assertIsNone(
                        build_worker.
                        _canonical_algorithm_matrix_clang_e2e_receipt_error(
                            receipt))

    def test_accepts_cmake_cache_without_exported_id_version_when_path_is_22(self):
        with tempfile.TemporaryDirectory() as temporary:
            receipt = self._receipt(
                Path(temporary) / "arm64",
                preset="native-arm64-clang22-linux-e2e",
                system="Linux",
                machine="aarch64",
                target_architecture="arm64",
                target_triplet="arm64-linux-libcxx",
                compiler=(
                    "/opt/bioseq-package-toolchain/llvm-22/bin/clang++"),
                compiler_id=None,
                compiler_version=None,
            )
            self.assertIsNone(
                build_worker.
                _canonical_algorithm_matrix_clang_e2e_receipt_error(receipt))

    def test_accepts_actual_approved_darwin_receipt_contract(self):
        # Mirrors sealed br_2e3718eb1b464541bf9319ce28857a12: Darwin's
        # LLVM 22 defaults to libc++, so its recorded flags have no -stdlib.
        with tempfile.TemporaryDirectory() as temporary:
            receipt = self._receipt(
                Path(temporary) / "bench-e2e-opt",
                preset="bench-e2e-opt",
                system="Darwin",
                machine="arm64",
                compiler="/opt/homebrew/opt/llvm@22/bin/clang++",
                compiler_id=None,
                compiler_version=None,
                flags="-O3 -DNDEBUG -fno-omit-frame-pointer",
            )
            receipt["configuration"]["compiler_contract"].update(
                CMAKE_BUILD_TYPE="Debug")
            self.assertIsNone(
                build_worker.
                _canonical_algorithm_matrix_clang_e2e_receipt_error(receipt))
            receipt["configuration"]["compiler_contract"].update(
                CMAKE_CXX_FLAGS=(
                    "-O3 -DNDEBUG -fno-omit-frame-pointer "
                    "-stdlib=libstdc++"))
            self.assertIsNotNone(
                build_worker.
                _canonical_algorithm_matrix_clang_e2e_receipt_error(receipt))

    def test_accepts_actual_aws_x64_linux_cache_contract(self):
        # Mirrors the preserved M7A aws-x64-linux-e2e CMake cache: the
        # architecture is UNINITIALIZED=x64, and an empty profile cache value
        # selects the authored default rather than an invented profile gate.
        with tempfile.TemporaryDirectory() as temporary:
            receipt = self._receipt(
                Path(temporary) / "aws-x64-linux-e2e",
                preset="aws-x64-linux-e2e",
                system="Linux",
                machine="x86_64",
                target_architecture="x64",
                target_architecture_type="UNINITIALIZED",
                target_triplet="x64-linux-libcxx",
                profile="",
                compiler=(
                    "/opt/bioseq-package-toolchain/llvm-22/bin/clang++"),
                compiler_id=None,
                compiler_version=None,
            )
            receipt["configuration"]["compiler_contract"].update(
                CMAKE_BUILD_TYPE="Debug")
            self.assertIsNone(
                build_worker.
                _canonical_algorithm_matrix_clang_e2e_receipt_error(receipt))

    def test_rejects_name_host_compiler_mode_and_flag_lookalikes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = self._receipt(
                root / "base",
                preset="native-arm64-clang22-linux-e2e",
                system="Linux",
                machine="aarch64",
                target_architecture="arm64",
                target_triplet="arm64-linux-libcxx",
            )
            mutations = {
                "gcc preset": lambda value: value.update(
                    preset="aws-x64-linux-gcc161-e2e"),
                "debug preset": lambda value: value.update(
                    preset="native-arm64-clang22-linux-e2e-debug"),
                "arbitrary preset": lambda value: value.update(
                    preset="invented-e2e"),
                "wrong mode": lambda value: value.update(mode="benchmark"),
                "debug tree": lambda value: value.update(debug=True),
                "wrong family": lambda value: value.update(
                    preset_family="test"),
                "wrong subdirectory": lambda value: value.update(
                    binary_subdir="bioseq_caching"),
                "wrong host": lambda value: value["host"].update(
                    machine="x86_64"),
                "gcc compiler": lambda value: value["configuration"]
                    ["compiler_contract"].update(
                        CMAKE_CXX_COMPILER="/usr/bin/g++",
                        CMAKE_CXX_COMPILER_ID="GNU"),
                "clang 23": lambda value: value["configuration"]
                    ["compiler_contract"].update(
                        CMAKE_CXX_COMPILER="/opt/llvm-23/bin/clang++",
                        CMAKE_CXX_COMPILER_VERSION="23.1.1"),
                "O2": lambda value: value["configuration"]
                    ["compiler_contract"].update(
                        CMAKE_CXX_FLAGS=(
                            "-O2 -DNDEBUG -fno-omit-frame-pointer "
                            "-stdlib=libc++")),
                "sanitizer": lambda value: value["configuration"]
                    ["compiler_contract"].update(
                        CMAKE_CXX_FLAGS=(
                            "-O3 -DNDEBUG -fno-omit-frame-pointer "
                            "-stdlib=libc++ -fsanitize=address")),
                "explicit libstdc++": lambda value: value["configuration"]
                    ["compiler_contract"].update(
                        CMAKE_CXX_FLAGS=(
                            "-O3 -DNDEBUG -fno-omit-frame-pointer "
                            "-stdlib=libstdc++")),
            }
            for label, mutate in mutations.items():
                with self.subTest(label=label):
                    candidate = copy.deepcopy(base)
                    mutate(candidate)
                    self.assertIsNotNone(
                        build_worker.
                        _canonical_algorithm_matrix_clang_e2e_receipt_error(
                            candidate))

    def test_rejects_missing_e2e_cache_and_cross_architecture_contracts(self):
        cases = (
            {"intervals": "OFF"},
            {"test_indexes": "ON"},
            {"target_architecture": "x64"},
            {"target_triplet": "x64-linux-libcxx"},
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for index, overrides in enumerate(cases):
                with self.subTest(overrides=overrides):
                    options = {
                        "preset": "native-arm64-clang22-linux-e2e",
                        "system": "Linux",
                        "machine": "aarch64",
                        "target_architecture": "arm64",
                        "target_triplet": "arm64-linux-libcxx",
                        **overrides,
                    }
                    receipt = self._receipt(root / str(index), **options)
                    self.assertIsNotNone(
                        build_worker.
                        _canonical_algorithm_matrix_clang_e2e_receipt_error(
                            receipt))


class BuildReceiptWorkflowTests(unittest.TestCase):
    @staticmethod
    def _inspect_receipt(
        mode: str,
        build_dir: Path,
        *,
        debug: bool,
        preset: str,
        receipt_id: str = "br_debug",
    ) -> dict[str, object]:
        details = build_presets.MODES[mode]
        return {
            "build_receipt_id": receipt_id,
            "mode": mode,
            "debug": debug,
            "preset": preset,
            "preset_family": details.preset_family,
            "binary_subdir": details.binary_subdir,
            "targets": list(details.targets),
            "build_dir": str(build_dir),
            "source_graph_fingerprint": "source-digest",
        }

    @staticmethod
    def _write_receipted_bench_command(build_dir: Path) -> Path:
        bin_dir = build_dir / "bioseq"
        bin_dir.mkdir(parents=True, exist_ok=True)
        command = bin_dir / "bioseq_benchmark"
        command.write_bytes(b"test benchmark executable\n")
        command.chmod(0o755)
        return command

    def test_public_schemas_expose_build_and_receipt_controls(self):
        start_schema = next(
            item for item in protocol.TOOL_DEFINITIONS
            if item["name"] == "start_build"
        )["inputSchema"]
        inspect_schema = next(
            item for item in protocol.TOOL_DEFINITIONS
            if item["name"] == "inspect"
        )["inputSchema"]
        self.assertTrue({
            "fresh",
            "build_timeout_sec",
            "run_only",
            "reuse_build_receipt",
        }.issubset(start_schema["properties"]))
        self.assertEqual(
            start_schema["properties"]["build_timeout_sec"]["maximum"],
            3300,
        )
        self.assertIn(
            "reuse_build_receipt", inspect_schema["properties"])
        self.assertIn(
            "historical_artifact_only", inspect_schema["properties"])
        self.assertFalse(
            inspect_schema["properties"]["historical_artifact_only"]["default"])

    def test_status_schema_is_closed_and_accepts_empty_aggregate_request(self):
        status_schema = next(
            item for item in protocol.TOOL_DEFINITIONS
            if item["name"] == "status"
        )["inputSchema"]
        self.assertEqual(len(status_schema["oneOf"]), 2)
        aggregate, per_mode = status_schema["oneOf"]
        self.assertFalse(aggregate["additionalProperties"])
        self.assertEqual(aggregate["properties"], {})
        self.assertFalse(per_mode["additionalProperties"])
        self.assertEqual(per_mode["required"], ["mode"])
        self.assertEqual(
            per_mode["properties"]["mode"]["enum"],
            sorted(build_presets.VALID_MODES),
        )

    def test_status_aggregate_has_exact_path_free_closed_shape(self):
        document = {
            "document_update": {"queued": 0, "running": 0, "state": "idle"},
            "document_embed": {"queued": 0, "running": 0, "state": "idle"},
            "qmd_server": {"persistent": True, "state": "idle"},
        }
        registries = (
            ({"benchmark": {"status": "completed"}}, True),
            ({}, False),
        )
        with mock.patch.object(
            build_worker, "_read_activity_registry", side_effect=registries,
        ), mock.patch.object(
            build_worker, "_document_activity", return_value=document,
        ), mock.patch.object(
            build_worker, "subagent_activity_snapshot",
            return_value={"queued": 0, "running": 0, "state": "idle"},
        ), mock.patch.object(
            build_worker.shutil, "disk_usage",
            return_value=SimpleNamespace(free=12345),
        ):
            response = json.loads(build_worker.tool_status())

        self.assertEqual(set(response), {"local_activity"})
        activity = response["local_activity"]
        self.assertEqual(
            set(activity), {"quiescent", "free_disk_bytes", "workers"})
        self.assertTrue(activity["quiescent"])
        self.assertEqual(activity["free_disk_bytes"], 12345)
        self.assertEqual(
            set(activity["workers"]),
            {
                "build", "document_update", "document_embed",
                "mcp_subagent", "papers", "remote_operation",
                "other_detached", "qmd_server",
            },
        )
        for name in set(activity["workers"]) - {"qmd_server"}:
            self.assertEqual(
                set(activity["workers"][name]), {"queued", "running", "state"})
        self.assertEqual(
            set(activity["workers"]["qmd_server"]), {"persistent", "state"})
        self.assertNotIn("/", json.dumps(response, sort_keys=True))

    def test_status_aggregate_fails_closed_on_malformed_registry(self):
        document = {
            "document_update": {"queued": 0, "running": 0, "state": "absent"},
            "document_embed": {"queued": 0, "running": 0, "state": "absent"},
            "qmd_server": {"persistent": False, "state": "absent"},
        }
        with mock.patch.object(
            build_worker,
            "_read_activity_registry",
            side_effect=((None, True), ({}, False)),
        ), mock.patch.object(
            build_worker, "_document_activity", return_value=document,
        ), mock.patch.object(
            build_worker, "subagent_activity_snapshot",
            return_value={"queued": 0, "running": 0, "state": "absent"},
        ), mock.patch.object(
            build_worker.shutil, "disk_usage",
            return_value=SimpleNamespace(free=1),
        ):
            activity = json.loads(build_worker.tool_status())["local_activity"]
        self.assertFalse(activity["quiescent"])
        self.assertEqual(activity["workers"]["build"]["state"], "unknown")
        self.assertEqual(activity["workers"]["papers"]["state"], "unknown")
        self.assertEqual(
            activity["workers"]["other_detached"]["state"], "unknown")

    def test_status_dispatch_rejects_unknown_arguments(self):
        with self.assertRaisesRegex(RuntimeError, "unknown argument"):
            protocol._handle_tools_call({
                "name": "status",
                "arguments": {"unexpected": True},
            })

    def test_status_dispatch_enforces_the_closed_schema(self):
        for arguments in (None, [], False, "", {"mode": None}):
            with self.subTest(arguments=arguments), self.assertRaisesRegex(
                RuntimeError, "arguments must be an object|mode must be a string"
            ):
                protocol._handle_tools_call({
                    "name": "status",
                    "arguments": arguments,
                })
        with self.assertRaisesRegex(RuntimeError, "supported build mode"):
            protocol._handle_tools_call({
                "name": "status",
                "arguments": {"mode": "not-a-mode"},
            })

    def test_activity_marks_unverified_terminal_cleanup_unknown(self):
        for entry in (
            {"status": "aborted"},
            {"status": "killed_timeout"},
            {"status": "completed", "cleanup_unverified": True},
            {
                "status": "failed",
                "termination_evidence": {"cleanup_verified": False},
            },
            {
                "status": "completed",
                "termination_evidence": {
                    "cleanup_verified": True,
                    "survivor_pids": [123],
                },
            },
        ):
            with self.subTest(entry=entry):
                self.assertEqual(
                    process_state._transient_activity([entry])["state"], "unknown")
        verified_timeout = {
            "status": "killed_timeout",
            "termination_evidence": {"cleanup_verified": True},
            "cleanup_unverified": False,
        }
        self.assertEqual(
            process_state._transient_activity([verified_timeout])["state"], "idle")

    def test_public_input_matrix_fails_closed_before_scheduling(self):
        invalid = (
            ({"fresh": True}, "requires build_only"),
            ({"build_only": True, "build_timeout_sec": 3301}, "between 60"),
            ({"build_only": True, "build_timeout_sec": 3300},
             "requires fresh=true"),
            ({"run_only": True}, "requires reuse_build_receipt"),
            ({"build_only": True, "run_only": True}, "mutually exclusive"),
            ({"reuse_build_receipt": "br_x"}, "only with run_only"),
        )
        with mock.patch.object(
            build_worker, "_refuse_root_build_identity", return_value=None,
        ), mock.patch.object(
            build_worker, "_ensure_build_worker_timesharing_policy",
        ):
            for request, expected in invalid:
                with self.subTest(request=request):
                    message = build_worker.tool_start_build(
                        "unit-integration", **request)
                    self.assertIn(expected, message)

    def test_guard_terms_at_54_minutes_and_kills_by_55(self):
        soft, hard = build_worker._build_phase_deadlines(100.0, 3300)
        self.assertEqual(soft, 100.0 + 54 * 60)
        self.assertEqual(hard, 100.0 + 55 * 60)

    def test_start_rechecks_cleanup_after_running_entry_refresh(self):
        with mock.patch.object(
            build_worker, "_refuse_root_build_identity", return_value=None,
        ), mock.patch.object(
            build_worker, "_ensure_build_worker_timesharing_policy",
        ), mock.patch.object(
            build_worker, "_git_commit", return_value="a" * 40,
        ), mock.patch.object(
            build_worker, "_source_graph_fingerprint", return_value="fingerprint",
        ), mock.patch.object(
            build_worker, "_load_state", return_value={},
        ), mock.patch.object(
            build_worker,
            "_unverified_guard_cleanup",
            side_effect=(None, "newly discovered unverified cleanup"),
        ) as cleanup, mock.patch.object(
            build_worker, "_running_in_same_group", return_value=(None, None),
        ) as refresh, mock.patch.object(
            build_worker.subprocess, "Popen",
        ) as popen:
            message = build_worker.tool_start_build(
                "unit-integration",
                build_only=True,
                fresh=True,
                build_timeout_sec=3300,
            )

        self.assertIn("newly discovered unverified cleanup", message)
        self.assertEqual(cleanup.call_count, 2)
        refresh.assert_called_once()
        popen.assert_not_called()

    def test_source_fingerprint_ignores_docs_build_and_reports(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "DOCS/CURRENT").mkdir(parents=True)
            (root / "build/x").mkdir(parents=True)
            (root / "reporting").mkdir(parents=True)
            (root / "bioseq").mkdir(parents=True)
            (root / "bioseq/source.cppm").write_text(
                "export module source;\n", encoding="utf-8")
            (root / "DOCS/CURRENT/log.md").write_text(
                "one\n", encoding="utf-8")
            (root / "build/x/output").write_text(
                "one\n", encoding="utf-8")
            (root / "reporting/report.json").write_text(
                "{}\n", encoding="utf-8")
            self.assertEqual(
                subprocess.run(
                    ["git", "init", "-q", str(root)], check=False,
                ).returncode,
                0,
            )
            first = build_worker._source_graph_fingerprint(root)
            (root / "DOCS/CURRENT/log.md").write_text(
                "two\n", encoding="utf-8")
            (root / "build/x/output").write_text(
                "two\n", encoding="utf-8")
            (root / "reporting/report.json").write_text(
                '{"changed":true}\n', encoding="utf-8")
            self.assertEqual(first, build_worker._source_graph_fingerprint(root))
            (root / "bioseq/source.cppm").write_text(
                "export module changed;\n", encoding="utf-8")
            self.assertNotEqual(first, build_worker._source_graph_fingerprint(root))

    def test_source_fingerprint_uses_shared_snapshot_allowlist_in_git_argv(
            self):
        completed = subprocess.CompletedProcess([], 0, b"", b"")
        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(
                build_worker.subprocess, "run", return_value=completed) as run:
            repository_root = Path(temporary).resolve()
            build_worker._source_graph_fingerprint(repository_root)
        argv = run.call_args.args[0]
        self.assertEqual(argv, [
            "git",
            "-C",
            str(repository_root),
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
            "--",
            *build_worker._BUILD_SOURCE_FINGERPRINT_PATHS,
        ])


    def test_descendant_capture_includes_new_sessions(self):
        root = process_state._ProcessIdentity(10, 1, 10, 10, "start-root", "cmake")
        child = process_state._ProcessIdentity(11, 10, 11, 11, "start-child", "ninja")
        compiler = process_state._ProcessIdentity(
            12, 11, 12, 12, "start-compiler", "clang")
        with mock.patch.object(
            process_state,
            "_process_snapshot",
            return_value={10: root, 11: child, 12: compiler},
        ):
            groups, evidence = process_state._validated_process_tree(root)
        self.assertEqual(set(groups), {10, 11, 12})
        self.assertEqual(evidence["descendant_pids"], [10, 11, 12])

    def test_receipt_round_trip_and_content_tamper(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            repository = root
            build_dir = repository / "build/mcp/run/test-opt"
            build_dir.mkdir(parents=True)
            receipt_path = root / "receipts.json"
            host = {
                "node": build_worker.host_platform.node(),
                "system": build_worker.host_platform.system(),
                "machine": build_worker.host_platform.machine(),
                "release": build_worker.host_platform.release(),
            }
            receipt = {
                "kind": "bioseq-build-receipt",
                "schema_revision": 2,
                "build_receipt_id": "br_round_trip",
                "status": "completed",
                "exit_code": 0,
                "created_at": 1.0,
                "mode": "unit-integration",
                "debug": False,
                "fresh": True,
                "preset": "test-opt",
                "preset_family": "test",
                "binary_subdir": "bioseq",
                "targets": list(
                    build_presets.MODES["unit-integration"].targets),
                "source_repository_root": str(repository),
                "source_commit": "a" * 40,
                "source_graph_fingerprint": "source-digest",
                "build_dir": str(build_dir),
                "build_timeout_sec": 3300,
                "build_phase_started_at": 1.0,
                "configure_completed_at": 2.0,
                "configure_duration_sec": 1.0,
                "build_phase_completed_at": 3.0,
                "build_phase_duration_sec": 2.0,
                "target_timings": [],
                "termination_evidence": None,
                "configuration": {"sealed": True},
                "runtime_artifacts": [],
                "authorized_artifacts": [{"sealed": True}],
                "artifact_counts": {"sealed": 1},
                "host": host,
            }
            receipt["content_sha256"] = integrity.canonical_json_sha256(
                receipt)
            with mock.patch.object(
                process_state, "BUILD_RECEIPT_FILE", receipt_path,
            ), mock.patch.object(
                build_worker, "REPOSITORY_ROOT", repository,
            ), mock.patch.object(
                build_worker, "_source_graph_fingerprint",
                side_effect=AssertionError("reuse must not inspect current sources"),
            ), mock.patch.object(
                build_worker, "_git_commit",
                side_effect=AssertionError("reuse must not inspect current HEAD"),
            ), mock.patch.object(
                build_worker, "_build_configuration_evidence",
                return_value={"sealed": True},
            ), mock.patch.object(
                build_worker, "_runtime_artifact_evidence", return_value=[],
            ), mock.patch.object(
                build_worker,
                "_receipt_authorized_artifact_evidence",
                return_value=[{"sealed": True}],
            ), mock.patch.object(
                build_worker, "_build_artifact_counts",
                return_value={"sealed": 1},
            ):
                process_state._store_build_receipt(receipt)
                observed, error = build_worker._validate_build_receipt(
                    "br_round_trip", expected_mode="unit-integration")
                self.assertIsNone(error)
                self.assertEqual(
                    observed["build_receipt_id"], "br_round_trip")
                # A build that succeeds after a commit/source edit must also
                # seal without consulting current sources. Preserve the
                # scheduling metadata; don't relabel the artifact as current.
                sealed_after_edit = build_worker._seal_successful_build_receipt(
                    mode="unit-integration",
                    entry={**receipt, "pending_build_receipt_id": "br_after_edit"},
                    build_dir=build_dir, preset="test-opt", debug=False)
                self.assertEqual(sealed_after_edit["source_commit"], "a" * 40)
                self.assertEqual(sealed_after_edit["source_graph_fingerprint"],
                                 "source-digest")
                reused, error = build_worker._validate_build_receipt("br_after_edit")
                self.assertIsNone(error)
                self.assertEqual(reused, sealed_after_edit)
                for checker, value in (
                    ("_build_configuration_evidence", {"changed": True}),
                    ("_runtime_artifact_evidence", [{"changed": True}]),
                ):
                    with self.subTest(checker=checker), mock.patch.object(
                            build_worker, checker, return_value=value):
                        rejected, rejection = build_worker._validate_build_receipt(
                            "br_round_trip")
                        self.assertIsNone(rejected)
                        self.assertIsNotNone(rejection)
                schema_mutations = {
                    "legacy": lambda value: value.update(
                        schema_version=value.pop("schema_revision")),
                    "legacy-kind": lambda value: value.update(
                        kind="bioseq-build-receipt-v2"),
                    "wrong": lambda value: value.update(schema_revision=1),
                    "boolean": lambda value: value.update(schema_revision=True),
                    "unknown": lambda value: value.update(unexpected=True),
                }
                for name, mutate in schema_mutations.items():
                    with self.subTest(name=name):
                        invalid = dict(receipt)
                        invalid.pop("content_sha256")
                        mutate(invalid)
                        invalid["content_sha256"] = (
                            integrity.canonical_json_sha256(invalid))
                        with mock.patch.object(
                            build_worker, "_get_build_receipt",
                            return_value=invalid,
                        ):
                            rejected, rejection = (
                                build_worker._validate_build_receipt(
                                    "br_round_trip"))
                        self.assertIsNone(rejected)
                        self.assertIsNotNone(rejection)
                ledger = json.loads(receipt_path.read_text(encoding="utf-8"))
                ledger["br_round_trip"]["mode"] = "benchmark"
                receipt_path.write_text(
                    json.dumps(ledger), encoding="utf-8")
                observed, error = build_worker._validate_build_receipt(
                    "br_round_trip")
                self.assertIsNone(observed)
                self.assertIn("content seal", error)

    def test_run_receipt_uses_current_closed_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            report = Path(temporary) / "report.json"
            report.write_text('{"passed":true}\n', encoding="utf-8")
            build_receipt = {
                "build_receipt_id": "br_source",
                "content_sha256": "a" * 64,
                "source_commit": "b" * 40,
                "source_graph_fingerprint": "source-graph",
                "host": {"machine": "test"},
                "runtime_artifacts": [],
                "authorized_artifacts": [],
            }
            with mock.patch.object(
                build_worker, "_store_build_receipt",
            ) as store:
                receipt = build_worker._seal_run_receipt(
                    mode="unit-integration",
                    build_receipt=build_receipt,
                    report_files=[report],
                    bench_flags=[],
                    test_flags=[],
                    bench_e2e_config=None,
                )

        self.assertEqual(receipt["kind"], "bioseq-run-receipt")
        self.assertIs(type(receipt["schema_revision"]), int)
        self.assertEqual(receipt["schema_revision"], 1)
        self.assertNotIn("schema_version", receipt)
        sealed = dict(receipt)
        digest = sealed.pop("content_sha256")
        self.assertEqual(digest, integrity.canonical_json_sha256(sealed))
        store.assert_called_once_with(receipt)

    def test_reparented_survivor_is_still_killed_by_start_identity(self):
        root = process_state._ProcessIdentity(20, 1, 20, 20, "root-start", "cmake")
        child = process_state._ProcessIdentity(21, 20, 21, 21, "child-start", "clang")
        reparented = process_state._ProcessIdentity(
            21, 1, 21, 21, "child-start", "clang --still-running")
        snapshots = [
            {20: root, 21: child},
            {21: reparented},
            {21: reparented},
        ]
        clock = iter((0.0, 0.0, 10.0, 10.0, 10.0, 10.0))
        killed = []
        with mock.patch.object(
            process_state, "_process_snapshot", side_effect=snapshots,
        ), mock.patch.object(
            process_state.os, "killpg",
            side_effect=lambda pgid, sig: killed.append(
                (pgid, sig)),
        ), mock.patch.object(
            process_state.time, "time", side_effect=lambda: next(clock),
        ), mock.patch.object(process_state.time, "sleep"):
            evidence = process_state._terminate_validated_process_tree(
                root, hard_kill_deadline=10.0)
        self.assertIn((21, signal.SIGKILL), killed)
        self.assertIn(21, evidence["survivor_pids"])
        self.assertFalse(evidence["cleanup_verified"])
        self.assertEqual(
            [item["pid"] for item in evidence["survivor_identities"]], [21])

    def test_artifact_authority_detects_extension_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            bin_dir = build_dir / "bioseq"
            bin_dir.mkdir()
            for command in build_presets.MODES["unit-integration"].commands:
                path = bin_dir / command[0]
                path.write_bytes(b"test executable\n")
                path.chmod(0o755)
            suffix = build_worker._native_shared_library_suffix(
                build_worker.host_platform.system())
            (build_dir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")
            core = bin_dir / f"bioseq{suffix}"
            cache = (
                build_dir
                / "bioseq_caching/artifacts/cache_production/Release"
                / f"bioseq_cache{suffix}"
            )
            cache.parent.mkdir(parents=True)
            core.write_bytes(b"core dso\n")
            cache.write_bytes(b"cache dso\n")
            (build_dir / "cmake_install.cmake").write_text(
                "# install plan\n", encoding="utf-8")
            (bin_dir / "bioseq.control").write_text(
                "default_version='1.0'\n", encoding="utf-8")
            (bin_dir / "bioseq--1.0.sql").write_text(
                "SELECT 1;\n", encoding="utf-8")

            before = build_worker._receipt_authorized_artifact_evidence(
                "unit-integration", build_dir, debug=False)
            core.write_bytes(b"mutated core dso\n")
            after = build_worker._receipt_authorized_artifact_evidence(
                "unit-integration", build_dir, debug=False)

        self.assertNotEqual(before, after)
        relative_paths = {item["relative_path"] for item in before}
        self.assertIn(str(core.relative_to(build_dir)), relative_paths)
        self.assertIn(str(cache.relative_to(build_dir)), relative_paths)
        self.assertIn("cmake_install.cmake", relative_paths)

    def test_cache_receipt_seals_artifact_manifest_and_referenced_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            bin_dir = build_dir / "bioseq_caching"
            bin_dir.mkdir()
            runner = bin_dir / "bioseq_cache_integration_tests"
            runner.write_bytes(b"cache integration runner\n")
            manifest = bin_dir / "cache_native_artifacts.json"
            manifest.write_text('{"schema_revision":2}\n', encoding="utf-8")
            stage_root = build_dir / "staged/postgres"
            stage_bin = stage_root / "bin"
            stage_bin.mkdir(parents=True)
            stage_manifest = stage_root / "manifest.json"
            stage_manifest.write_text("{}\n", encoding="utf-8")
            referenced = build_dir / "staged/cache--1.0.sql"
            referenced.write_text("SELECT 1;\n", encoding="utf-8")
            cache_control = build_dir / "staged/bioseq_cache.control"
            cache_control.write_text("default_version='1.0'\n", encoding="utf-8")
            cache_dso_input = build_dir / "staged/bioseq_cache.dylib"
            cache_dso_input.write_bytes(b"cache test support dso\n")
            artifacts = {
                key: "" for key in cache_modes._CACHE_NATIVE_ARTIFACT_KEYS
            }
            artifacts.update({
                "cache_test_support_postgres_bin_dir": str(stage_bin),
                "cache_test_support_postgres_stage_root": str(stage_root),
                "cache_test_support_postgres_stage_manifest":
                    str(stage_manifest),
                "cache_test_control": str(cache_control),
                "cache_test_install_sql": str(referenced),
                "cache_test_dso": str(cache_dso_input),
                # This configured profile is not consumed by cache-integration.
                "cache_production_postgres_stage_root":
                    str(build_dir / "unused/missing-production-stage"),
            })
            suffix = build_worker._native_shared_library_suffix(
                build_worker.host_platform.system())
            (build_dir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")
            cache_dso = (
                build_dir
                / "bioseq_caching/artifacts/cache_production/Release"
                / f"bioseq_cache{suffix}"
            )
            cache_dso.parent.mkdir(parents=True)
            cache_dso.write_bytes(b"cache dso\n")

            with mock.patch.object(
                build_worker,
                "_cache_native_artifacts",
                return_value=artifacts,
            ):
                before = build_worker._receipt_authorized_artifact_evidence(
                    "cache-integration", build_dir, debug=False)
                referenced.write_text("SELECT 2;\n", encoding="utf-8")
                after = build_worker._receipt_authorized_artifact_evidence(
                    "cache-integration", build_dir, debug=False)
                referenced.unlink()
                with self.assertRaisesRegex(
                        RuntimeError,
                        "cache native artifact manifest references a file "
                        "outside the receipted build tree"):
                    build_worker._receipt_authorized_artifact_evidence(
                        "cache-integration", build_dir, debug=False)

        before_by_path = {
            item["relative_path"]: item for item in before}
        after_by_path = {
            item["relative_path"]: item for item in after}
        manifest_path = str(manifest.relative_to(build_dir))
        referenced_path = str(referenced.relative_to(build_dir))
        self.assertIn(manifest_path, before_by_path)
        self.assertIn(referenced_path, before_by_path)
        self.assertNotEqual(
            before_by_path[referenced_path]["sha256"],
            after_by_path[referenced_path]["sha256"],
        )

    def test_artifact_authority_seals_vcpkg_library_alias_chains(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            self._write_receipted_bench_command(build_dir)
            expected = {}
            for relative_library_dir in (
                Path("vcpkg_installed/test-triplet/lib"),
                Path("vcpkg_installed/test-triplet/debug/lib"),
            ):
                library_dir = build_dir / relative_library_dir
                library_dir.mkdir(parents=True)
                terminal = library_dir / "libvulkan.1.4.341.dylib"
                terminal.write_bytes(
                    b"debug vulkan\n"
                    if "debug" in relative_library_dir.parts
                    else b"release vulkan\n")
                middle = library_dir / "libvulkan.1.dylib"
                entry = library_dir / "libvulkan.dylib"
                middle.symlink_to(terminal.name)
                entry.symlink_to(middle.name)
                expected[str(entry.relative_to(build_dir))] = (
                    middle.name,
                    str(terminal.relative_to(build_dir)),
                    hashlib.sha256(terminal.read_bytes()).hexdigest(),
                )

            evidence = build_worker._receipt_authorized_artifact_evidence(
                "benchmark", build_dir, debug=False)

        by_path = {item["relative_path"]: item for item in evidence}
        for alias_path, (direct_target, terminal_path, digest) in expected.items():
            alias = by_path[alias_path]
            self.assertEqual(
                alias["artifact_kind"], "vcpkg-shared-library-alias")
            self.assertEqual(alias["link_target"], direct_target)
            self.assertEqual(len(alias["symlink_chain"]), 2)
            self.assertEqual(alias["target_relative_path"], terminal_path)
            self.assertEqual(alias["target_sha256"], digest)
            self.assertEqual(by_path[terminal_path]["sha256"], digest)
            middle_path = str(Path(alias_path).parent / direct_target)
            self.assertEqual(
                by_path[middle_path]["artifact_kind"],
                "vcpkg-shared-library-alias",
            )
            self.assertEqual(len(by_path[middle_path]["symlink_chain"]), 1)
        self.assertEqual(
            [item["relative_path"] for item in evidence],
            sorted(item["relative_path"] for item in evidence),
        )

    def test_artifact_authority_seals_same_content_alias_retarget(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            self._write_receipted_bench_command(build_dir)
            library_dir = (
                build_dir / "vcpkg_installed/test-triplet/lib")
            library_dir.mkdir(parents=True)
            first = library_dir / "libchoice.1.dylib"
            second = library_dir / "libchoice.2.dylib"
            first.write_bytes(b"same library bytes\n")
            second.write_bytes(b"same library bytes\n")
            alias = library_dir / "libchoice.dylib"
            alias.symlink_to(first.name)
            before = build_worker._receipt_authorized_artifact_evidence(
                "benchmark", build_dir, debug=False)
            alias.unlink()
            alias.symlink_to(second.name)
            after = build_worker._receipt_authorized_artifact_evidence(
                "benchmark", build_dir, debug=False)

        before_regular = [
            item for item in before if "artifact_kind" not in item]
        after_regular = [
            item for item in after if "artifact_kind" not in item]
        before_alias = next(
            item for item in before
            if item["relative_path"].endswith("libchoice.dylib"))
        after_alias = next(
            item for item in after
            if item["relative_path"].endswith("libchoice.dylib"))
        self.assertEqual(before_regular, after_regular)
        self.assertNotEqual(before_alias, after_alias)
        self.assertEqual(before_alias["link_target"], first.name)
        self.assertEqual(after_alias["link_target"], second.name)

    def test_artifact_authority_rejects_alias_replacement_during_capture(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            self._write_receipted_bench_command(build_dir)
            library_dir = (
                build_dir / "vcpkg_installed/test-triplet/lib")
            library_dir.mkdir(parents=True)
            first = library_dir / "libchoice.1.dylib"
            second = library_dir / "libchoice.2.dylib"
            first.write_bytes(b"first library\n")
            second.write_bytes(b"second library\n")
            alias = library_dir / "libchoice.dylib"
            alias.symlink_to(first.name)
            original_readlink = build_worker.os.readlink
            replaced = False

            def replace_after_readlink(path):
                nonlocal replaced
                target = original_readlink(path)
                if Path(path) == alias and not replaced:
                    replaced = True
                    alias.unlink()
                    alias.symlink_to(second.name)
                return target

            with mock.patch.object(
                    build_worker.os, "readlink",
                    side_effect=replace_after_readlink):
                with self.assertRaisesRegex(
                        RuntimeError, "changed while inspected"):
                    build_worker._receipt_authorized_artifact_evidence(
                        "benchmark", build_dir, debug=False)

    def test_artifact_authority_seals_alias_terminal_content(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            self._write_receipted_bench_command(build_dir)
            library_dir = (
                build_dir / "vcpkg_installed/test-triplet/lib")
            library_dir.mkdir(parents=True)
            terminal = library_dir / "libcontent.1.dylib"
            terminal.write_bytes(b"content A\n")
            alias = library_dir / "libcontent.dylib"
            alias.symlink_to(terminal.name)
            before = build_worker._receipt_authorized_artifact_evidence(
                "benchmark", build_dir, debug=False)
            terminal.write_bytes(b"content B\n")
            after = build_worker._receipt_authorized_artifact_evidence(
                "benchmark", build_dir, debug=False)

        before_by_path = {
            item["relative_path"]: item for item in before}
        after_by_path = {
            item["relative_path"]: item for item in after}
        alias_path = str(alias.relative_to(build_dir))
        terminal_path = str(terminal.relative_to(build_dir))
        self.assertNotEqual(
            before_by_path[alias_path]["target_sha256"],
            after_by_path[alias_path]["target_sha256"],
        )
        self.assertNotEqual(
            before_by_path[terminal_path]["sha256"],
            after_by_path[terminal_path]["sha256"],
        )

    def test_artifact_authority_rejects_unsafe_vcpkg_library_aliases(self):
        cases = {
            "absolute": "/tmp/liboutside.dylib",
            "parent": "../liboutside.dylib",
            "nested": "nested/libtarget.dylib",
            "broken": "libmissing.dylib",
            "non-library": "payload.txt",
            "non-regular": "libdirectory.dylib",
        }
        for label, target in cases.items():
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                build_dir = Path(temporary).resolve()
                self._write_receipted_bench_command(build_dir)
                library_dir = (
                    build_dir / "vcpkg_installed/test-triplet/lib")
                library_dir.mkdir(parents=True)
                if label == "nested":
                    (library_dir / "nested").mkdir()
                    (library_dir / target).write_bytes(b"nested library\n")
                elif label == "non-library":
                    (library_dir / target).write_bytes(b"not a library\n")
                elif label == "non-regular":
                    (library_dir / target).mkdir()
                alias = library_dir / "libunsafe.dylib"
                alias.symlink_to(target)
                with self.assertRaises(RuntimeError):
                    build_worker._receipt_authorized_artifact_evidence(
                        "benchmark", build_dir, debug=False)

        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            self._write_receipted_bench_command(build_dir)
            library_dir = (
                build_dir / "vcpkg_installed/test-triplet/lib")
            library_dir.mkdir(parents=True)
            first = library_dir / "libcycle.dylib"
            second = library_dir / "libcycle.1.dylib"
            first.symlink_to(second.name)
            second.symlink_to(first.name)
            with self.assertRaisesRegex(RuntimeError, "cycle"):
                build_worker._receipt_authorized_artifact_evidence(
                    "benchmark", build_dir, debug=False)

    def test_artifact_authority_rejects_symlinked_vcpkg_library_parents(self):
        parent_paths = (
            Path("vcpkg_installed"),
            Path("vcpkg_installed/test-triplet"),
            Path("vcpkg_installed/test-triplet/lib"),
            Path("vcpkg_installed/test-triplet/debug"),
            Path("vcpkg_installed/test-triplet/debug/lib"),
        )
        for relative_parent in parent_paths:
            with self.subTest(relative_parent=relative_parent), \
                    tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                build_dir = root / "build"
                build_dir.mkdir()
                self._write_receipted_bench_command(build_dir)
                external = root / "external"
                external.mkdir()
                symlink = build_dir / relative_parent
                symlink.parent.mkdir(parents=True, exist_ok=True)
                symlink.symlink_to(external, target_is_directory=True)
                with self.assertRaisesRegex(
                        RuntimeError, "parent must be a real non-symlink"):
                    build_worker._receipt_authorized_artifact_evidence(
                        "benchmark", build_dir, debug=False)

    def test_artifact_authority_keeps_non_vcpkg_symlinks_strict(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            target = build_dir / "real-bench"
            target.write_bytes(b"test executable\n")
            target.chmod(0o755)
            bin_dir = build_dir / "bioseq"
            bin_dir.mkdir()
            (bin_dir / "bioseq_benchmark").symlink_to(target)
            with self.assertRaisesRegex(RuntimeError, "regular non-symlink"):
                build_worker._receipt_authorized_artifact_evidence(
                    "benchmark", build_dir, debug=False)

        for authority_name in ("cmake_install.cmake", "install.sql"):
            with self.subTest(authority_name=authority_name), \
                    tempfile.TemporaryDirectory() as temporary:
                build_dir = Path(temporary).resolve()
                self._write_receipted_bench_command(build_dir)
                payload = build_dir / "payload"
                payload.write_bytes(b"install payload\n")
                (build_dir / authority_name).symlink_to(payload)
                with self.assertRaisesRegex(RuntimeError, "must not be a symlink"):
                    build_worker._receipt_authorized_artifact_evidence(
                        "benchmark", build_dir, debug=False)

        for authority_name, debug in (
            ("install.so.sql", False),
            ("install.so.control", False),
            ("listing.so.s", True),
            ("bitcode.so.bc", True),
        ):
            with self.subTest(overlapping_name=authority_name), \
                    tempfile.TemporaryDirectory() as temporary:
                build_dir = Path(temporary).resolve()
                self._write_receipted_bench_command(build_dir)
                library_dir = (
                    build_dir / "vcpkg_installed/test-triplet/lib")
                library_dir.mkdir(parents=True)
                payload = library_dir / "libpayload.so.1"
                payload.write_bytes(b"library payload\n")
                (library_dir / authority_name).symlink_to(payload.name)
                with self.assertRaisesRegex(RuntimeError, "must not be a symlink"):
                    build_worker._receipt_authorized_artifact_evidence(
                        "benchmark", build_dir, debug=debug)

        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            self._write_receipted_bench_command(build_dir)
            dependency_dir = build_dir / "linked"
            dependency_dir.mkdir()
            target = dependency_dir / "liboutside.1.dylib"
            target.write_bytes(b"library\n")
            (dependency_dir / "liboutside.dylib").symlink_to(target.name)
            with self.assertRaisesRegex(RuntimeError, "must not be a symlink"):
                build_worker._receipt_authorized_artifact_evidence(
                    "benchmark", build_dir, debug=False)

    def test_artifact_authority_keeps_extension_dsos_non_symlink_strict(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            bin_dir = build_dir / "bioseq"
            bin_dir.mkdir()
            for command in build_presets.MODES["unit-integration"].commands:
                path = bin_dir / command[0]
                path.write_bytes(b"test executable\n")
                path.chmod(0o755)
            suffix = build_worker._native_shared_library_suffix(
                build_worker.host_platform.system())
            (build_dir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")
            real_core = bin_dir / f"real-bioseq{suffix}"
            real_core.write_bytes(b"core dso\n")
            (bin_dir / f"bioseq{suffix}").symlink_to(real_core.name)
            cache = (
                build_dir
                / "bioseq_caching/artifacts/cache_production/Release"
                / f"bioseq_cache{suffix}"
            )
            cache.parent.mkdir(parents=True)
            cache.write_bytes(b"cache dso\n")

            with self.assertRaisesRegex(RuntimeError, "has no native"):
                build_worker._receipt_authorized_artifact_evidence(
                    "unit-integration", build_dir, debug=False)

    def test_artifact_authority_rejects_dependency_dso_name_substitution(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            bin_dir = build_dir / "bioseq"
            bin_dir.mkdir()
            for command in build_presets.MODES["unit-integration"].commands:
                path = bin_dir / command[0]
                path.write_bytes(b"test executable\n")
                path.chmod(0o755)
            suffix = build_worker._native_shared_library_suffix(
                build_worker.host_platform.system())
            (build_dir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")
            dependency_dir = build_dir / "vcpkg_installed/test-triplet/lib"
            dependency_dir.mkdir(parents=True)
            (dependency_dir / f"bioseq{suffix}").write_bytes(
                b"unrelated core basename\n")
            (dependency_dir / f"bioseq_cache{suffix}").write_bytes(
                b"unrelated cache basename\n")
            stale_cache = (
                build_dir
                / "bioseq_caching/artifacts/cache_production/OldConfiguration"
                / f"bioseq_cache{suffix}"
            )
            stale_cache.parent.mkdir(parents=True)
            stale_cache.write_bytes(b"stale cache dso\n")

            with self.assertRaisesRegex(RuntimeError, "has no native"):
                build_worker._receipt_authorized_artifact_evidence(
                    "unit-integration", build_dir, debug=False)
            real_cache = (
                build_dir
                / "bioseq_caching/artifacts/cache_production/Release"
                / f"bioseq_cache{suffix}"
            )
            real_cache.parent.mkdir(parents=True)
            real_cache.write_bytes(b"real cache dso\n")
            with self.assertRaisesRegex(RuntimeError, "has no native"):
                build_worker._receipt_authorized_artifact_evidence(
                    "unit-integration", build_dir, debug=False)

    def test_artifact_authority_uses_core_target_path_across_modes(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            suffix = build_worker._native_shared_library_suffix(
                build_worker.host_platform.system())
            (build_dir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n",
                encoding="utf-8",
            )
            core = build_dir / "bioseq" / f"bioseq{suffix}"
            core.parent.mkdir()
            core.write_bytes(b"website-owned core dso\n")
            cache = (
                build_dir
                / "bioseq_caching/artifacts/cache_production/Release"
                / f"bioseq_cache{suffix}"
            )
            cache.parent.mkdir(parents=True)
            cache.write_bytes(b"website-owned cache dso\n")

            evidence = build_worker._receipt_authorized_artifact_evidence(
                "website", build_dir, debug=False)

        relative_paths = {item["relative_path"] for item in evidence}
        self.assertIn(str(core.relative_to(build_dir)), relative_paths)
        self.assertIn(str(cache.relative_to(build_dir)), relative_paths)

    def test_runtime_artifact_must_be_executable(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            bin_dir = build_dir / "bioseq"
            bin_dir.mkdir()
            (bin_dir / "bioseq_benchmark").write_text(
                "not executable\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "not executable"):
                build_worker._runtime_artifact_evidence("benchmark", build_dir)

    def test_artifact_counts_report_actual_compiled_and_linked_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            generated = build_dir / "bioseq/generated/highway/target"
            generated.mkdir(parents=True)
            (generated / "owner.cppm").write_text(
                "export module owner;\n", encoding="utf-8")
            objects = build_dir / "objects"
            objects.mkdir()
            (objects / "one.o").write_bytes(b"one")
            (objects / "two.o").write_bytes(b"two")
            bin_dir = build_dir / "bioseq"
            bin_dir.mkdir(exist_ok=True)
            executable = bin_dir / "bioseq_benchmark"
            executable.write_bytes(b"binary")
            executable.chmod(0o755)
            (bin_dir / f"provider{build_worker._native_shared_library_suffix(build_worker.host_platform.system())}").write_bytes(b"dso")
            (build_dir / "compile_commands.json").write_text(
                json.dumps([{}, {}, {}]), encoding="utf-8")
            (build_dir / "cmake_install.cmake").write_text(
                "# plan\n", encoding="utf-8")
            (objects / "ignored.o.link").symlink_to(objects / "one.o")

            counts = build_worker._build_artifact_counts(build_dir, "benchmark")

        self.assertEqual(counts["compile_commands"], 3)
        self.assertEqual(counts["generated_highway_sources"], 1)
        self.assertEqual(counts["compiled_objects"], 2)
        self.assertEqual(counts["linked_artifacts"], 2)
        self.assertEqual(counts["runtime_artifacts"], 1)
        self.assertEqual(counts["install_scripts"], 1)


    def test_special_postgres_worker_does_not_promise_ordinary_receipt(self):
        self.assertFalse(build_worker._build_receipt_required(
            build_presets.POSTGRES_ERROR_SAFETY_MODE, True))
        self.assertTrue(build_worker._build_receipt_required("benchmark", True))

    def test_io_inspect_target_is_owned_by_io_mode(self):
        self.assertEqual(
            build_worker.INSPECT_TARGETS["bioseq_io_benchmark"].mode,
            "benchmark-io",
        )

    def test_status_surfaces_artifact_counts(self):
        entry = {
            "status": "completed",
            "started_at": 1.0,
            "completed_at": 2.0,
            "build_only": True,
            "debug": False,
            "preset": "bench-opt",
            "log_file": "/tmp/test.log",
            "artifact_counts": {
                "compiled_objects": 2,
                "linked_artifacts": 1,
            },
        }
        with mock.patch.object(
            build_worker, "_load_state", return_value={"benchmark": entry},
        ), mock.patch.object(build_worker, "_refresh", return_value=entry):
            message = build_worker.tool_status("benchmark")
        self.assertIn("Artifact counts", message)
        self.assertIn('"compiled_objects": 2', message)

        failed = dict(entry)
        failed.update({
            "status": "failed",
            "notes": "guard cleanup failed",
            "build_timeout_sec": 3300,
            "timeout_guard_armed": True,
            "build_soft_deadline": 3240.0,
            "build_hard_deadline": 3300.0,
            "active_edge": "[2/10] clang++ owner.cppm",
            "termination_evidence": {"cleanup_verified": False},
            "cleanup_unverified": True,
        })
        with mock.patch.object(
            build_worker, "_load_state", return_value={"benchmark": failed},
        ), mock.patch.object(build_worker, "_refresh", return_value=failed):
            failed_message = build_worker.tool_status("benchmark")
        self.assertIn("[2/10] clang++ owner.cppm", failed_message)
        self.assertIn("Termination evidence", failed_message)
        self.assertIn("cleanup_unverified=True", failed_message)

    def test_run_only_helper_never_invokes_build(self):
        receipt = {"build_receipt_id": "br_test", "debug": False}
        entry = {"started_at": time.time()}
        cases = (
            ("benchmark-fast", "bench-opt", [], []),
            (
                "cache-integration",
                "cache-opt",
                [],
                ["--allow-running-no-tests", "[cache-correctness]"],
            ),
        )
        for mode, preset, bench_flags, test_flags in cases:
            with self.subTest(mode=mode), \
                    tempfile.TemporaryDirectory() as temporary:
                build_dir = Path(temporary)
                log_path = build_dir / "run.log"
                with log_path.open("w", encoding="utf-8") as log_handle, \
                        mock.patch.object(
                            build_worker,
                            "_build_worker_descendant_environment",
                            return_value={},
                        ), mock.patch.object(
                            build_worker, "_ensure_local_toolchain_environment",
                        ), mock.patch.object(
                            build_worker,
                            "_run_mode_commands",
                            return_value=(None, [], [], []),
                        ) as run_commands, mock.patch.object(
                            build_worker, "_stream_subprocess",
                        ) as subprocess_call, mock.patch.object(
                            build_worker, "_update_entry",
                        ), mock.patch.object(
                            build_worker,
                            "_validate_build_receipt",
                            return_value=(receipt, None),
                        ), mock.patch.object(
                            build_worker,
                            "_seal_run_receipt",
                            return_value={"build_receipt_id": "rr_test"},
                        ):
                    result = build_worker._run_only_from_build_receipt(
                        mode=mode,
                        entry=entry,
                        receipt=receipt,
                        active_build_dir=build_dir,
                        active_bin_dir=build_dir / (
                            "bioseq_caching"
                            if mode.startswith("cache-") else "bioseq"
                        ),
                        active_preset=preset,
                        bench_flags=bench_flags,
                        test_flags=test_flags,
                        bench_e2e_config=None,
                        log_handle=log_handle,
                        deadline=time.time() + 60,
                    )
            self.assertEqual(result, 0)
            run_commands.assert_called_once()
            self.assertEqual(
                run_commands.call_args.kwargs["test_flags"], test_flags)
            subprocess_call.assert_not_called()

    def test_inspect_accepts_unit_receipt_for_postgresql_bioseq(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "debug-tree"
            build_dir.mkdir()
            receipt = self._inspect_receipt(
                "unit-integration",
                build_dir,
                debug=True,
                preset="test-opt-debug",
            )
            captured = {}

            def inspect_impl(**arguments):
                captured.update(arguments)
                return {"summary": {}}

            with mock.patch.object(
                build_worker,
                "_validate_build_receipt",
                return_value=(receipt, None),
            ) as validate, mock.patch.object(
                build_worker,
                "_inspect_artifact_candidates",
                return_value=("bioseq.dylib",),
            ), mock.patch.object(
                build_worker, "_run_inspect_impl", side_effect=inspect_impl,
            ):
                result = json.loads(build_worker.tool_inspect({
                    "function_name": "ksw",
                    "target": "postgresql_bioseq",
                    "reuse_build_receipt": "br_debug",
                }))
        validate.assert_called_once_with(
            "br_debug",
            expected_debug=None,
        )
        self.assertEqual(captured["build_dir"], build_dir)
        self.assertEqual(captured["binary_subdir"], "bioseq")
        self.assertEqual(
            result["build_metadata"]["build_receipt_id"], "br_debug")
        self.assertEqual(
            captured["shared_target_names"],
            ("bioseq_modules_nopg", "bioseq_highway_fixed_target",
             "bioseq_runtime_core", "bioseq_pipeline_dispatch_owners"),
        )
        self.assertEqual(
            result["build_metadata"]["mode"], "unit-integration")
        self.assertEqual(result["build_metadata"]["preset_used"],
                         "test-opt-debug")
        self.assertEqual(result["build_metadata"]["preset_family"], "test")
        self.assertTrue(result["build_metadata"]["test_indexes_expected"])

    def test_inspect_receipt_rejects_target_not_owned_by_sealed_mode(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "debug-tree"
            build_dir.mkdir()
            receipt = self._inspect_receipt(
                "unit-integration",
                build_dir,
                debug=True,
                preset="test-opt-debug",
            )
            with mock.patch.object(
                build_worker,
                "_validate_build_receipt",
                return_value=(receipt, None),
            ) as validate, mock.patch.object(
                build_worker, "_inspect_artifact_candidates",
            ) as artifact_candidates, mock.patch.object(
                build_worker, "_run_inspect_impl",
            ) as inspect_impl:
                result = json.loads(build_worker.tool_inspect({
                    "function_name": "ksw",
                    "target": "bioseq_benchmark",
                    "reuse_build_receipt": "br_debug",
                }))

        validate.assert_called_once_with(
            "br_debug",
            expected_debug=None,
        )
        self.assertIn("does not own target 'bioseq_benchmark'", result["error"])
        artifact_candidates.assert_not_called()
        inspect_impl.assert_not_called()

    def test_inspect_receipt_accepts_benchmark_fast_target_ownership(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "debug-tree"
            build_dir.mkdir()
            receipt = self._inspect_receipt(
                "benchmark-fast",
                build_dir,
                debug=True,
                preset="bench-opt-debug",
            )
            captured = {}

            def inspect_impl(**arguments):
                captured.update(arguments)
                return {"summary": {}}

            with mock.patch.object(
                build_worker,
                "_validate_build_receipt",
                return_value=(receipt, None),
            ) as validate, mock.patch.object(
                build_worker,
                "_inspect_artifact_candidates",
                return_value=("bioseq_benchmark",),
            ), mock.patch.object(
                build_worker, "_run_inspect_impl", side_effect=inspect_impl,
            ):
                result = json.loads(build_worker.tool_inspect({
                    "function_name": "ksw",
                    "target": "bioseq_benchmark",
                    "reuse_build_receipt": "br_debug",
                }))

        validate.assert_called_once_with(
            "br_debug",
            expected_debug=None,
        )
        self.assertEqual(captured["build_dir"], build_dir)
        self.assertEqual(captured["binary_subdir"], "bioseq")
        self.assertEqual(result["build_metadata"]["mode"], "benchmark-fast")
        self.assertEqual(result["build_metadata"]["preset_used"],
                         "bench-opt-debug")
        self.assertEqual(result["build_metadata"]["preset_family"], "core")
        self.assertFalse(result["build_metadata"]["test_indexes_expected"])

    def test_inspect_receipt_rejects_nondebug_and_explicit_debug_mismatch(
            self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "fast-tree"
            build_dir.mkdir()
            receipt = self._inspect_receipt(
                "benchmark-fast",
                build_dir,
                debug=False,
                preset="bench-opt",
            )
            with self.subTest(case="validated nondebug receipt"), \
                    mock.patch.object(
                        build_worker,
                        "_validate_build_receipt",
                        return_value=(receipt, None),
                    ) as validate, mock.patch.object(
                        build_worker, "_inspect_artifact_candidates",
                    ) as artifact_candidates, mock.patch.object(
                        build_worker, "_run_inspect_impl",
                    ) as inspect_impl:
                result = json.loads(build_worker.tool_inspect({
                    "function_name": "ksw",
                    "target": "bioseq_benchmark",
                    "reuse_build_receipt": "br_debug",
                }))
                validate.assert_called_once_with(
                    "br_debug",
                    expected_debug=None,
                )
                self.assertIn("requires a debug build receipt", result["error"])
                artifact_candidates.assert_not_called()
                inspect_impl.assert_not_called()

            mismatch = (
                None,
                "build receipt debug-tree mismatch: expected True, got False",
            )
            with self.subTest(case="explicit debug mismatch"), \
                    mock.patch.object(
                        build_worker,
                        "_validate_build_receipt",
                        return_value=mismatch,
                    ) as validate, mock.patch.object(
                        build_worker, "_inspect_artifact_candidates",
                    ) as artifact_candidates, mock.patch.object(
                        build_worker, "_run_inspect_impl",
                    ) as inspect_impl:
                result = json.loads(build_worker.tool_inspect({
                    "function_name": "ksw",
                    "target": "bioseq_benchmark",
                    "debug": True,
                    "reuse_build_receipt": "br_debug",
                }))
                validate.assert_called_once_with(
                    "br_debug",
                    expected_debug=True,
                )
                self.assertIn("debug-tree mismatch", result["error"])
                artifact_candidates.assert_not_called()
                inspect_impl.assert_not_called()

    def test_inspect_historical_artifact_accepts_exact_nondebug_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "fast-tree"
            build_dir.mkdir()
            receipt = self._inspect_receipt(
                "benchmark-fast",
                build_dir,
                debug=False,
                preset="bench-opt",
            )
            receipt["authorized_artifacts"] = [{
                "relative_path": "bioseq/bioseq_benchmark",
                "sha256": "sealed-by-validator",
            }]
            captured = {}

            def inspect_impl(**arguments):
                captured.update(arguments)
                return {"summary": {}}

            with mock.patch.object(
                build_worker,
                "_validate_build_receipt",
                return_value=(receipt, None),
            ) as validate, mock.patch.object(
                build_worker,
                "_inspect_artifact_candidates",
                return_value=("bioseq_benchmark", "old_benchmark"),
            ), mock.patch.object(
                build_worker, "_run_inspect_impl", side_effect=inspect_impl,
            ):
                result = json.loads(build_worker.tool_inspect({
                    "function_name": "scan_sorted_events",
                    "target": "bioseq_benchmark",
                    "reuse_build_receipt": "br_fast",
                    "historical_artifact_only": True,
                }))

        validate.assert_called_once_with(
            "br_fast",
            expected_debug=None,
        )
        self.assertEqual(captured["build_dir"], build_dir)
        self.assertEqual(captured["artifact_names"], ("bioseq_benchmark",))
        self.assertFalse(captured["llc_on_bc"])
        self.assertFalse(result["build_metadata"]["debug_tree_used"])
        self.assertTrue(
            result["build_metadata"]["historical_artifact_only"])
        self.assertFalse(
            result["build_metadata"]["current_worktree_source_verified"])
        self.assertIn("historical artifact",
                      result["build_metadata"]["artifact_scope"])
        self.assertIn("current worktree not verified",
                      result["build_metadata"]["source_scope"])

    def test_inspect_historical_artifact_requires_receipt_and_target_artifact(
            self):
        without_receipt = json.loads(build_worker.tool_inspect({
            "function_name": "scan_sorted_events",
            "historical_artifact_only": True,
        }))
        self.assertIn("requires reuse_build_receipt", without_receipt["error"])

        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "fast-tree"
            build_dir.mkdir()
            receipt = self._inspect_receipt(
                "benchmark-fast",
                build_dir,
                debug=False,
                preset="bench-opt",
            )
            receipt["authorized_artifacts"] = [{
                "relative_path": "bioseq/unrelated_artifact",
            }]
            with mock.patch.object(
                build_worker,
                "_validate_build_receipt",
                return_value=(receipt, None),
            ), mock.patch.object(
                build_worker,
                "_inspect_artifact_candidates",
                return_value=("bioseq_benchmark",),
            ), mock.patch.object(
                build_worker, "_run_inspect_impl",
            ) as inspect_impl:
                result = json.loads(build_worker.tool_inspect({
                    "function_name": "scan_sorted_events",
                    "target": "bioseq_benchmark",
                    "reuse_build_receipt": "br_fast",
                    "historical_artifact_only": True,
                }))

        self.assertIn("does not authorize an artifact", result["error"])
        inspect_impl.assert_not_called()

    def test_inspect_unit_receipt_keeps_cache_target_artifact_layout(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "debug-tree"
            build_dir.mkdir()
            (build_dir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n",
                encoding="utf-8",
            )
            receipt = self._inspect_receipt(
                "unit-integration",
                build_dir,
                debug=True,
                preset="test-opt-debug",
            )
            captured = {}

            def inspect_impl(**arguments):
                captured.update(arguments)
                return {"summary": {}}

            with mock.patch.object(
                build_worker,
                "_validate_build_receipt",
                return_value=(receipt, None),
            ), mock.patch.object(
                build_worker.host_platform, "system", return_value="Darwin",
            ), mock.patch.object(
                build_worker, "_run_inspect_impl", side_effect=inspect_impl,
            ):
                result = json.loads(build_worker.tool_inspect({
                    "function_name": "ksw",
                    "target": "postgresql_bioseq_cache",
                    "reuse_build_receipt": "br_debug",
                }))

        self.assertEqual(captured["binary_subdir"], "bioseq_caching")
        self.assertEqual(captured["artifact_names"], (
            "artifacts/cache_production/RelWithDebInfo/bioseq_cache.dylib",
            "bioseq_cache.dylib",
        ))
        self.assertEqual(result["build_metadata"]["mode"], "unit-integration")
        self.assertEqual(result["build_metadata"]["preset_family"], "test")
        self.assertTrue(result["build_metadata"]["test_indexes_expected"])

    def test_inspect_without_receipt_keeps_target_default_mode(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "default-debug-tree"
            build_dir.mkdir()
            captured = {}

            def inspect_impl(**arguments):
                captured.update(arguments)
                return {"summary": {}}

            with mock.patch.object(
                build_worker, "build_dir_for", return_value=build_dir,
            ) as build_dir_for, mock.patch.object(
                build_worker, "preset_for", return_value="bench-e2e-opt-debug",
            ) as preset_for, mock.patch.object(
                build_worker, "_validate_build_receipt",
            ) as validate, mock.patch.object(
                build_worker,
                "_inspect_artifact_candidates",
                return_value=("bioseq.dylib",),
            ), mock.patch.object(
                build_worker, "_run_inspect_impl", side_effect=inspect_impl,
            ):
                result = json.loads(build_worker.tool_inspect({
                    "function_name": "ksw",
                    "target": "postgresql_bioseq",
                    "debug": True,
                }))

        validate.assert_not_called()
        build_dir_for.assert_called_once_with("benchmark-e2e", True)
        preset_for.assert_called_once_with("benchmark-e2e", True)
        self.assertEqual(captured["build_dir"], build_dir)
        self.assertEqual(captured["binary_subdir"], "bioseq")
        self.assertNotIn("mode", result["build_metadata"])
        self.assertEqual(result["build_metadata"]["preset_family"], "e2e")
        self.assertFalse(result["build_metadata"]["test_indexes_expected"])

    def test_inspect_default_modes_directly_own_their_target_names(self):
        shared_core = ("bioseq_modules_nopg", "bioseq_runtime_core")
        shared_highway = ("bioseq_modules_nopg", "bioseq_highway_fixed_target",
                          "bioseq_runtime_core")
        expected_owners = {
            "bioseq_benchmark": shared_highway,
            "bioseq_io_benchmark": shared_core,
            "bioseq_unit_tests": shared_highway,
            "bioseq_integration_tests": shared_highway,
            "bioseq_benchmark_e2e_contract_tests": shared_core,
            "bioseq_benchmark_framework_contract_tests": shared_core,
            "postgresql_bioseq": shared_highway + (
                "bioseq_pipeline_dispatch_owners",),
        }
        for target, definition in build_worker.INSPECT_TARGETS.items():
            with self.subTest(target=target, mode=definition.mode):
                self.assertIn(
                    target,
                    build_presets.MODES[definition.mode].targets,
                )
                if target in expected_owners:
                    self.assertEqual(
                        definition.shared_target_names, expected_owners[target])
                if target != "postgresql_bioseq":
                    self.assertNotIn(
                        "bioseq_pipeline_dispatch_owners",
                        definition.shared_target_names)


class GuardedProcessTrackerTests(unittest.TestCase):
    @staticmethod
    def _identity(pid, ppid, pgid, session, started, command):
        return process_state._ProcessIdentity(
            pid, ppid, pgid, session, started, command)

    def test_marker_finds_already_reparented_new_session(self):
        root = self._identity(10, 1, 10, 10, "root-start", "cmake")
        escaped = self._identity(
            11, 1, 11, 11, "child-start", "clang")
        tracker = process_state._GuardedProcessTracker(root, "marker")
        with mock.patch.object(
            process_state,
            "_guarded_process_snapshot",
            return_value=({10: root, 11: escaped}, {11}),
        ):
            live = tracker.refresh()
        self.assertEqual(
            {process.pid for process in live.values()}, {10, 11})

    def test_host_snapshot_finds_process_by_isolated_build_cwd(self):
        if not (
            sys.platform == "darwin"
            or sys.platform.startswith("linux")
        ):
            self.skipTest("cwd ownership probe is implemented on macOS/Linux")
        with tempfile.TemporaryDirectory() as temporary:
            ownership_root = Path(temporary).resolve()
            marker = "host-cwd-probe-marker"
            environment = dict(os.environ)
            environment[process_state.BUILD_GUARD_ENV_KEY] = marker
            process = subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(2)"],
                cwd=ownership_root,
                env=environment,
                start_new_session=True,
            )
            try:
                snapshot, owned = process_state._guarded_process_snapshot(
                    marker, ownership_root)
                self.assertIn(process.pid, snapshot)
                self.assertIn(process.pid, owned)
                self.assertEqual(snapshot[process.pid].session, process.pid)
            finally:
                process_state._terminate_unarmed_popen_group(
                    process,
                    guard_marker=marker,
                    ownership_root=ownership_root,
                )

    def test_retains_child_after_reparent_and_marker_clear(self):
        root = self._identity(20, 1, 20, 20, "root-start", "cmake")
        child = self._identity(21, 20, 21, 21, "child-start", "clang")
        reparented = self._identity(
            21, 1, 31, 31, "child-start", "clang")
        tracker = process_state._GuardedProcessTracker(root, "marker")
        with mock.patch.object(
            process_state,
            "_guarded_process_snapshot",
            side_effect=[
                ({20: root, 21: child}, {20, 21}),
                ({20: root, 21: reparented}, set()),
            ],
        ):
            tracker.refresh()
            live = tracker.refresh()
        observed = next(
            process for process in live.values() if process.pid == 21)
        self.assertEqual((observed.ppid, observed.pgid, observed.session),
                         (1, 31, 31))

    def test_pid_reuse_is_not_retained_without_marker_or_ancestry(self):
        root = self._identity(30, 1, 30, 30, "root-start", "cmake")
        child = self._identity(31, 30, 31, 31, "old-start", "clang")
        reused = self._identity(31, 1, 31, 31, "new-start", "unrelated")
        tracker = process_state._GuardedProcessTracker(root, "marker")
        with mock.patch.object(
            process_state,
            "_guarded_process_snapshot",
            side_effect=[
                ({30: root, 31: child}, {30, 31}),
                ({30: root, 31: reused}, set()),
            ],
        ):
            tracker.refresh()
            live = tracker.refresh()
        self.assertEqual({process.pid for process in live.values()}, {30})

    def test_verified_service_closure_is_preserved_but_unknown_child_remains(self):
        client = self._identity(35, 1, 35, 35, "client", "benchmark")
        postmaster = self._identity(36, 35, 36, 36, "pg", "postgres")
        checkpointer = self._identity(
            37, 36, 36, 36, "checkpointer", "postgres: checkpointer")
        unknown = self._identity(38, 35, 38, 38, "unknown", "helper")
        boundary = process_state._GuardedServiceBoundary(
            verified_root=postmaster,
            quarantined_root=None,
            evidence={"runtime_identity_verified": True},
        )
        tracker = process_state._GuardedProcessTracker(
            client,
            "marker",
            service_boundary_provider=lambda *unused: boundary,
        )
        snapshot = {
            value.pid: value
            for value in (client, postmaster, checkpointer, unknown)
        }
        with mock.patch.object(
            process_state,
            "_guarded_process_snapshot",
            return_value=(snapshot, set(snapshot)),
        ):
            live = tracker.refresh()
        self.assertEqual({item.pid for item in live.values()}, {35, 38})
        self.assertEqual(
            {item.pid for item in tracker.last_preserved_service.values()},
            {36, 37},
        )

    def test_unverified_service_candidate_is_quarantined_not_signalled(self):
        client = self._identity(45, 1, 45, 45, "client", "benchmark")
        # Deliberately share the client's process group: the service boundary
        # must force exact-PID signaling instead of killpg(45).
        postmaster = self._identity(46, 45, 45, 46, "pg", "postgres")
        unknown = self._identity(47, 45, 47, 47, "unknown", "helper")
        boundary = process_state._GuardedServiceBoundary(
            verified_root=None,
            quarantined_root=postmaster,
            evidence={"runtime_identity_verified": False},
        )
        tracker = process_state._GuardedProcessTracker(
            client,
            "marker",
            service_boundary_provider=lambda *unused: boundary,
        )
        snapshot = {
            value.pid: value for value in (client, postmaster, unknown)
        }
        with mock.patch.object(
            process_state,
            "_guarded_process_snapshot",
            return_value=(snapshot, set(snapshot)),
        ), mock.patch.object(process_state.os, "killpg") as kill_group, \
                mock.patch.object(process_state.os, "kill") as kill_pid:
            live = tracker.refresh()
            process_state._signal_guarded_processes(
                live, signal.SIGTERM, snapshot=snapshot)
        self.assertEqual(
            {item.pid for item in tracker.last_quarantined_service.values()},
            {46},
        )
        self.assertNotIn(46, [call.args[0] for call in kill_group.call_args_list])
        self.assertNotIn(45, [call.args[0] for call in kill_group.call_args_list])
        self.assertNotIn(46, [call.args[0] for call in kill_pid.call_args_list])

    def test_strict_snapshot_accepts_only_proven_transient_exit(self):
        output = (
            "10 1 10 10 Thu Sep 10 03:00:00 2026 ps -axo\n"
            "11 1 11 11 Thu Sep 10 03:00:01 2026 worker\n"
        )
        completed = subprocess.CompletedProcess([], 0, output, "")

        def pgid(pid):
            if pid == 10:
                raise ProcessLookupError()
            return pid

        with mock.patch.object(
            process_state.subprocess, "run", return_value=completed,
        ), mock.patch.object(
            process_state.os, "getpgid", side_effect=pgid,
        ), mock.patch.object(process_state.os, "getsid", return_value=11):
            snapshot = process_state._process_snapshot(
                time.time() + 1, require_complete=True)
        self.assertEqual(set(snapshot), {11})

        with mock.patch.object(
            process_state.subprocess, "run", return_value=completed,
        ), mock.patch.object(
            process_state.os, "getpgid", side_effect=PermissionError("denied"),
        ):
            with self.assertRaisesRegex(RuntimeError, "PermissionError"):
                process_state._process_snapshot(
                    time.time() + 1, require_complete=True)

    def test_termination_discovers_group_created_after_term_snapshot(self):
        root = self._identity(40, 1, 40, 40, "root-start", "cmake")
        child = self._identity(41, 1, 41, 41, "child-start", "clang")
        tracker = process_state._GuardedProcessTracker(root, "marker")
        snapshots = iter([
            ({40: root}, {40}),
            ({40: root, 41: child}, {40, 41}),
            ({40: root, 41: child}, {40, 41}),
            ({40: root, 41: child}, {40, 41}),
            ({}, set()),
        ])
        now = [0.0]
        signals = []
        direct_signals = []

        def sleep(duration):
            now[0] += max(duration, 0.1)

        with mock.patch.object(
            process_state,
            "_guarded_process_snapshot",
            side_effect=lambda marker, ownership_root=None, **options: next(snapshots),
        ), mock.patch.object(
            process_state,
            "_guarded_worker_validation",
            return_value=({}, None),
        ), mock.patch.object(
            process_state.os,
            "killpg",
            side_effect=lambda pgid, sig: signals.append((pgid, sig)),
        ), mock.patch.object(
            process_state.os,
            "kill",
            side_effect=lambda pid, sig: direct_signals.append((pid, sig)),
        ), mock.patch.object(
            process_state,
            "BUILD_TIMEOUT_TERM_GRACE_SEC",
            0.2,
        ):
            evidence = process_state._terminate_guarded_process_tree(
                tracker,
                hard_kill_deadline=2.0,
                state_mode="unit-integration",
                clock=lambda: now[0],
                sleeper=sleep,
            )
        self.assertIn((41, signal.SIGKILL), signals)
        self.assertIn((41, signal.SIGKILL), direct_signals)
        self.assertEqual(evidence["survivor_pids"], [])

    def test_persistent_snapshot_failure_is_not_reported_as_cleanup(self):
        root = self._identity(50, 1, 50, 50, "root-start", "cmake")
        tracker = process_state._GuardedProcessTracker(root, "marker")
        tracker.last_live = {(root.pid, root.started): root}
        now = [0.0]

        def sleep(duration):
            now[0] += max(duration, 0.1)

        with mock.patch.object(
            process_state,
            "_guarded_process_snapshot",
            side_effect=RuntimeError("ps unavailable"),
        ), mock.patch.object(
            process_state,
            "_guarded_worker_validation",
            return_value=({}, None),
        ), mock.patch.object(process_state.os, "killpg"), mock.patch.object(
            process_state.os, "kill",
        ), mock.patch.object(
            process_state, "BUILD_TIMEOUT_TERM_GRACE_SEC", 0.1,
        ):
            with self.assertRaises(
                process_state._GuardedTerminationError,
            ) as raised:
                process_state._terminate_guarded_process_tree(
                    tracker,
                    hard_kill_deadline=0.5,
                    state_mode="unit-integration",
                    clock=lambda: now[0],
                    sleeper=sleep,
                )
        self.assertFalse(raised.exception.evidence["cleanup_verified"])
        self.assertEqual(
            raised.exception.evidence["survivor_pids"], [50])

    def test_expired_deadline_does_not_spawn(self):
        with tempfile.NamedTemporaryFile(mode="w+") as log_handle, \
                mock.patch.object(process_state.subprocess, "Popen") as popen:
            with self.assertRaises(TimeoutError):
                process_state._stream_subprocess(
                    ["never-run"],
                    log_handle,
                    deadline=10.0,
                    hard_kill_deadline=11.0,
                    require_verified_termination=True,
                    deadline_clock=lambda: 10.0,
                )
        popen.assert_not_called()

    def test_fallback_group_grace_stays_inside_hard_deadline(self):
        signals = []
        with mock.patch.object(
                process_state.os, "getpgid", return_value=71), \
                mock.patch.object(
                    process_state.os,
                    "killpg",
                    side_effect=lambda pgid, sig: signals.append((pgid, sig)),
                ), mock.patch.object(process_state.time, "sleep") as sleep:
            process_state._kill_pgroup(
                71,
                hard_kill_deadline=10.0,
                clock=lambda: 10.0,
            )
        self.assertEqual(
            signals,
            [(71, signal.SIGTERM), (71, signal.SIGKILL)],
        )
        sleep.assert_not_called()

    def test_guard_preflight_cannot_launch_after_deadline(self):
        now = [9.0]

        def preflight(marker, ownership_root=None):
            now[0] = 10.0
            return {}, set()

        with tempfile.NamedTemporaryFile(mode="w+") as log_handle, \
                mock.patch.object(
                    process_state, "_guarded_process_snapshot", side_effect=preflight,
                ), mock.patch.object(
                    process_state,
                    "_load_state",
                    return_value={
                        "unit-integration": {"build_dir": "/tmp/guarded"},
                    },
                ), mock.patch.object(process_state.subprocess, "Popen") as popen:
            with self.assertRaises(TimeoutError):
                process_state._stream_subprocess(
                    ["never-run"],
                    log_handle,
                    deadline=10.0,
                    hard_kill_deadline=11.0,
                    require_verified_termination=True,
                    state_mode="unit-integration",
                    deadline_clock=lambda: now[0],
                )
        popen.assert_not_called()

    def test_parent_accepts_only_new_session_child_topology(self):
        exact = self._identity(
            70, 7, 70, 70, "worker-start", "python server.py")
        wrong_parent = self._identity(
            70, 8, 70, 70, "worker-start", "python server.py")
        wrong_group = self._identity(
            70, 7, 71, 70, "worker-start", "python server.py")
        self.assertTrue(process_state._valid_detached_worker_identity(
            exact, child_pid=70, parent_pid=7))
        self.assertFalse(process_state._valid_detached_worker_identity(
            wrong_parent, child_pid=70, parent_pid=7))
        self.assertFalse(process_state._valid_detached_worker_identity(
            wrong_group, child_pid=70, parent_pid=7))

    def test_guarded_worker_arm_requires_exact_pid_start_and_session(self):
        identity = self._identity(
            77, 1, 77, 77, "worker-start", "python server.py")
        entry = {
            "status": "running",
            "step": "armed",
            "pid": 77,
            "worker_process_identity": identity.__dict__,
        }
        with mock.patch.object(
            process_state, "_load_state", return_value={"unit-integration": entry},
        ), mock.patch.object(process_state.os, "getpid", return_value=77), \
                mock.patch.object(
                    process_state, "_capture_process_identity", return_value=identity):
            observed = process_state._await_guarded_worker_arm("unit-integration")
        self.assertEqual(observed["pid"], 77)

    def test_parent_refuses_to_arm_when_identity_capture_fails(self):
        fake_process = mock.Mock(pid=4321, returncode=1)
        fake_process.wait.return_value = 1
        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(
            process_state, "STATE_FILE", Path(temporary) / "state.json",
        ), mock.patch.object(
            process_state, "LOCK_FILE", Path(temporary) / "state.lock",
        ), mock.patch.object(
            build_worker, "_refuse_root_build_identity", return_value=None,
        ), mock.patch.object(
            build_worker, "_ensure_build_worker_timesharing_policy",
        ), mock.patch.object(
            build_worker, "_source_graph_fingerprint", return_value="fingerprint",
        ), mock.patch.object(
            build_worker, "_git_commit", return_value="a" * 40,
        ), mock.patch.object(
            build_worker, "_configured_local_build_rt_priority", return_value=None,
        ), mock.patch.object(
            build_worker, "_preflight_local_clang_rt_launcher",
        ), mock.patch.object(
            build_worker, "_local_clang_rt_is_enabled", return_value=False,
        ), mock.patch.object(
            build_worker.subprocess, "Popen", return_value=fake_process,
        ), mock.patch.object(
            build_worker, "_capture_process_identity", return_value=None,
        ), mock.patch.object(
            build_worker,
            "_terminate_unarmed_popen_group",
            return_value={"cleanup_verified": True},
        ) as terminate_unarmed:
            message = build_worker.tool_start_build(
                "unit-integration",
                build_only=True,
                fresh=True,
                build_timeout_sec=3300,
            )
            state = process_state._load_state()["unit-integration"]
        self.assertIn("identity error", message.lower())
        self.assertEqual(state["step"], "worker_identity_failed")
        terminate_unarmed.assert_called_once_with(fake_process)

    def test_initial_tracker_failure_persists_cleanup_block(self):
        root = self._identity(
            90, 1, 90, 90, "root-start", "cmake --build")
        fake_process = mock.Mock(pid=90)
        fake_process.stdout = mock.Mock()
        cleanup_error = process_state._GuardedTerminationError(
            "cleanup unverified",
            {
                "survivor_identities": [root.__dict__],
                "cleanup_verified": False,
            },
        )
        with tempfile.NamedTemporaryFile(mode="w+") as log_handle, \
                mock.patch.object(
                    process_state,
                    "_load_state",
                    return_value={
                        "unit-integration": {"build_dir": "/tmp/guarded"},
                    },
                ), mock.patch.object(
                    process_state, "_guarded_process_snapshot", return_value=({}, set()),
                ), mock.patch.object(
                    process_state.subprocess, "Popen", return_value=fake_process,
                ), mock.patch.object(
                    process_state, "_capture_process_identity", return_value=root,
                ), mock.patch.object(
                    process_state._GuardedProcessTracker,
                    "refresh",
                    side_effect=RuntimeError("snapshot failed"),
                ), mock.patch.object(
                    process_state,
                    "_terminate_guarded_process_tree",
                    side_effect=cleanup_error,
                ), mock.patch.object(process_state, "_update_entry") as update:
            with self.assertRaises(process_state._GuardedTerminationError):
                process_state._stream_subprocess(
                    ["cmake", "--build", "/tmp/guarded"],
                    log_handle,
                    deadline=10.0,
                    hard_kill_deadline=11.0,
                    state_mode="unit-integration",
                    require_verified_termination=True,
                    deadline_clock=lambda: 0.0,
                )
        self.assertTrue(any(
            call.kwargs.get("cleanup_unverified") is True
            for call in update.call_args_list
        ))

    def test_dead_guarded_worker_runs_orphan_recovery(self):
        worker = self._identity(
            101, 1, 101, 101, "worker-start", "python server.py")
        entry = {
            "status": "running",
            "pid": 101,
            "worker_process_identity": worker.__dict__,
            "build_timeout_sec": 3300,
            "build_dir": "/tmp/guarded",
            "started_at": 1.0,
        }
        state = {"unit-integration": entry}
        evidence = {"cleanup_verified": False, "survivor_identities": []}
        with mock.patch.object(
            process_state, "_capture_process_identity", return_value=None,
        ), mock.patch.object(
            process_state, "_is_pid_alive", return_value=False,
        ), mock.patch.object(
            process_state,
            "_recover_guarded_worker_descendants",
            return_value=(evidence, True),
        ) as recover, mock.patch.object(process_state, "_save_state"):
            observed = process_state._refresh(state, "unit-integration")
        recover.assert_called_once()
        self.assertEqual(observed["status"], "aborted")
        self.assertTrue(observed["cleanup_unverified"])
        self.assertEqual(observed["termination_evidence"], evidence)

    def test_unexpected_worker_death_preserves_postgres_cleanup_block(self):
        entry = {
            "status": "running",
            "pid": 909,
            "started_at": 1.0,
            "postgres_cleanup_unverified": True,
            "cleanup_unverified": True,
            "postgres_cleanup_evidence": {
                "cleanup_verified": False,
                "reason": "armed before E2E client launch",
            },
        }
        state = {"benchmark-e2e": entry}
        with mock.patch.object(
                process_state, "_is_pid_alive", return_value=False), \
                mock.patch.object(process_state, "_save_state"):
            observed = process_state._refresh(state, "benchmark-e2e")
        self.assertEqual(observed["status"], "aborted")
        self.assertTrue(observed["cleanup_unverified"])
        self.assertTrue(observed["postgres_cleanup_unverified"])
        self.assertTrue(
            observed["postgres_cleanup_evidence"]["unexpected_worker_exit"])

    def test_q1024_postgres_cleanup_preserves_process_uncertainty_or(self):
        owner = "bioseq-q1024-" + "a" * 32
        cases = (
            (
                "prior uncertainty",
                {"cleanup_unverified_before_postgres_owner": True},
                True,
            ),
            (
                "unverified termination",
                {
                    "cleanup_unverified_before_postgres_owner": False,
                    "termination_evidence": {
                        "cleanup_verified": False,
                        "survivor_pids": [42],
                    },
                },
                True,
            ),
            (
                "sticky process uncertainty survives later clean evidence",
                {
                    "cleanup_unverified_before_postgres_owner": False,
                    "process_cleanup_unverified": True,
                    "termination_evidence": {
                        "cleanup_verified": True,
                        "survivor_pids": [],
                        "marker_survivor_pids": [],
                        "survivor_identities": [],
                    },
                },
                True,
            ),
            (
                "fully verified",
                {
                    "cleanup_unverified_before_postgres_owner": False,
                    "termination_evidence": {
                        "cleanup_verified": True,
                        "survivor_pids": [],
                        "marker_survivor_pids": [],
                        "survivor_identities": [],
                    },
                },
                False,
            ),
        )
        for name, extra_state, expected_unverified in cases:
            with self.subTest(name=name):
                state_entry = {
                    "postgres_backend_owner_started": True,
                    "postgres_backend_identity": {
                        "application_name": owner,
                        "database": "bioseq",
                        "user": "postgres",
                    },
                    "postgres_cleanup_unverified": True,
                    "cleanup_unverified": True,
                    **extra_state,
                }
                with mock.patch.object(
                    build_worker, "_load_state",
                    return_value={"benchmark-e2e": state_entry},
                ), mock.patch.object(
                    build_worker, "_cleanup_owned_postgresql_backends",
                    return_value={"cleanup_verified": True},
                ), mock.patch.object(
                    build_worker, "_update_entry",
                ) as update:
                    verified = (
                        build_worker._cleanup_armed_q1024_postgresql_owner(
                            mode="benchmark-e2e",
                            active_build_dir=Path("/tmp/receipt-tree"),
                            active_preset="bench-e2e-opt",
                            postgres_application_name=owner,
                            log_handle=io.StringIO(),
                            cleanup_deadline=time.time() + 60,
                        ))
                self.assertTrue(verified)
                terminal = update.call_args.kwargs
                self.assertFalse(terminal["postgres_backend_owner_started"])
                self.assertFalse(terminal["postgres_cleanup_unverified"])
                self.assertEqual(
                    terminal["cleanup_unverified"], expected_unverified)

    def test_unverified_cleanup_requires_successful_empty_ownership_sweep(self):
        entry = {
            "cleanup_unverified": True,
            "build_dir": "/tmp/guarded",
            "termination_evidence": {"survivor_identities": []},
        }
        child = self._identity(
            111, 1, 111, 111, "child-start", "clang")
        with mock.patch.object(
            build_worker,
            "_guarded_process_snapshot",
            return_value=({111: child}, {111}),
        ):
            blocked = build_worker._unverified_guard_cleanup({"benchmark": entry})
        self.assertIn("remain live", blocked)
        with mock.patch.object(
            build_worker, "_guarded_process_snapshot", return_value=({}, set()),
        ):
            cleared = build_worker._unverified_guard_cleanup({"benchmark": entry})
        self.assertIsNone(cleared)

        postgres_entry = {
            **entry,
            "postgres_cleanup_unverified": True,
        }
        with mock.patch.object(
                build_worker,
                "_guarded_process_snapshot",
                side_effect=AssertionError("process sweep cannot prove PG cleanup")):
            postgres_blocked = build_worker._unverified_guard_cleanup(
                {"benchmark-e2e": postgres_entry})
        self.assertIn("PostgreSQL backend cleanup", postgres_blocked)


class Q1024ServiceLifecycleTests(unittest.TestCase):
    @staticmethod
    def _identity(pid, ppid, pgid, session, started, command):
        return process_state._ProcessIdentity(
            pid, ppid, pgid, session, started, command)

    def _legacy_entry(self, build_dir: Path) -> dict[str, object]:
        token = "a" * 32
        application = f"bioseq-q1024-{token}"
        identities = {
            "worker_process_identity": self._identity(
                101, 1, 101, 101, "worker", "server.py").__dict__,
            "active_process_identity": self._identity(
                102, 101, 102, 102, "client", "bioseq_benchmark_e2e").__dict__,
            "q1024_watchdog_process_identity": self._identity(
                103, 1, 103, 103, "watchdog", "q1024_watchdog.py").__dict__,
        }
        return {
            "mode": "benchmark-e2e",
            "status": "aborted",
            "step": "aborted",
            "exit_code": None,
            "report_files": ["/tmp/report274.json"],
            "q1024_run_token": token,
            "postgres_application_name": application,
            "postgres_backend_owner_started": True,
            "postgres_cleanup_unverified": True,
            "process_cleanup_unverified": True,
            "cleanup_unverified": True,
            "postgres_backend_identity": {
                "application_name": application,
                "database": "bioseq",
                "user": "postgres",
            },
            "bench_e2e_config": "sealed-q1024.json",
            "reuse_build_receipt": "br_build33",
            "build_dir": str(build_dir),
            "preset": "bench-e2e-opt",
            "preset_family": "e2e",
            **identities,
        }

    def test_persisted_boundary_context_or_token_mismatch_quarantines(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            data_dir = root / "data"
            data_dir.mkdir()
            postgres = root / "postgres"
            dso = root / "bioseq.dylib"
            postgres.write_bytes(b"postgres")
            dso.write_bytes(b"dso")
            client = self._identity(200, 100, 200, 200, "client", "benchmark")
            worker = self._identity(100, 1, 100, 100, "worker", "server")
            postmaster = self._identity(201, 200, 201, 201, "pg", "postgres")
            marker = "b" * 32
            token = "c" * 32

            def file_identity(path):
                metadata = path.stat()
                return {
                    "path": str(path),
                    "device": metadata.st_dev,
                    "inode": metadata.st_ino,
                    "size": metadata.st_size,
                    "sha256": "d" * 64,
                }

            boundary = {
                "schema_version": 1,
                "mode": "benchmark-e2e",
                "run_token": token,
                "application_name": f"bioseq-q1024-{token}",
                "client_process_identity": client.__dict__,
                "guard_marker": marker,
                "data_directory": {
                    "path": str(data_dir),
                    "device": data_dir.stat().st_dev,
                    "inode": data_dir.stat().st_ino,
                },
                "postgres_executable_identity": file_identity(postgres),
                "bioseq_dso_identity": file_identity(dso),
                "postmaster_process_identity": postmaster.__dict__,
                "runtime_identity_verified": True,
                "verified_at": 1.0,
            }
            entry = {
                "mode": "benchmark-e2e",
                "q1024_run_token": token,
                "postgres_application_name": f"bioseq-q1024-{token}",
                "active_guard_marker": marker,
                "active_process_identity": client.__dict__,
                "worker_process_identity": worker.__dict__,
                "postgres_service_boundary": boundary,
            }
            observed_wrong_session = self._identity(
                201, 200, 201, 999, "pg", "postgres")
            provider = process_state._persisted_guarded_service_boundary_provider(
                entry)
            self.assertIsNotNone(provider)
            observation = provider(
                {201: observed_wrong_session}, client, marker, None, {201})
            self.assertIsNone(observation.verified_root)
            self.assertEqual(observation.quarantined_root, postmaster)

            bad_token_entry = copy.deepcopy(entry)
            bad_token_entry["postgres_service_boundary"]["run_token"] = "e" * 32
            bad_provider = (
                process_state._persisted_guarded_service_boundary_provider(
                    bad_token_entry))
            bad_observation = bad_provider(
                {201: postmaster}, client, marker, None, {201})
            self.assertIsNone(bad_observation.verified_root)
            self.assertEqual(bad_observation.quarantined_root, postmaster)

    def test_runtime_provider_quarantines_first_observation_and_pid_gap(self):
        token = "4" * 32
        client = self._identity(300, 1, 300, 300, "client", "benchmark")
        first_postmaster = self._identity(
            301, 300, 301, 301, "pg1",
            "/opt/postgresql/bin/postgres -D /tmp/one")
        first_child = self._identity(
            302, 301, 301, 301, "pg1-child", "postgres: checkpointer")
        second_postmaster = self._identity(
            303, 300, 303, 303, "pg2", "postmaster -D /tmp/two")
        snapshot = {
            item.pid: item
            for item in (
                client, first_postmaster, first_child, second_postmaster)
        }
        entry = {
            "mode": "benchmark-e2e",
            "status": "running",
            "q1024_run_token": token,
            "postgres_application_name": f"bioseq-q1024-{token}",
            "postgres_backend_owner_started": True,
        }
        provider = postgres_runtime._PostgreSQLGuardServiceBoundaryProvider(
            mode="benchmark-e2e",
            run_token=token,
            application_name=f"bioseq-q1024-{token}",
            active_build_dir=Path("/tmp/build"),
            active_preset="bench-e2e-opt",
            log_handle=io.StringIO(),
        )
        with mock.patch.object(
            postgres_runtime, "_load_state",
            return_value={"benchmark-e2e": entry}), mock.patch.object(
            provider, "_runtime_selection",
            side_effect=RuntimeError("selection failed")):
            observation = provider(
                snapshot, client, "5" * 32, time.time() + 1,
                set(snapshot))
        self.assertIsNone(observation.verified_root)
        self.assertEqual(
            {item.pid for item in observation.quarantined_roots},
            {301, 303},
        )

        with mock.patch.object(
            postgres_runtime, "_load_state",
            return_value={"benchmark-e2e": entry}), mock.patch.object(
            provider, "_runtime_selection",
            return_value=("/tmp/data", mock.sentinel.postgres,
                          mock.sentinel.dso)), mock.patch.object(
            postgres_runtime, "_postgresql_postmaster_pid",
            side_effect=RuntimeError("postmaster.pid missing")):
            observation = provider(
                snapshot, client, "5" * 32, time.time() + 1,
                set(snapshot))
        self.assertIsNone(observation.verified_root)
        self.assertEqual(
            {item.pid for item in observation.quarantined_roots},
            {301, 303},
        )

    def test_provider_exception_terminates_exact_root_and_quarantines_service(
            self):
        client = self._identity(310, 1, 310, 310, "client", "benchmark")
        postmaster = self._identity(
            311, 310, 311, 311, "pg", "/opt/bin/postgres -D /tmp/data")
        tracker = process_state._GuardedProcessTracker(
            client,
            "6" * 32,
            service_boundary_provider=mock.Mock(
                side_effect=KeyError("provider failed")),
        )
        calls = [0]
        now = [0.0]
        group_signals = []
        direct_signals = []

        def snapshot(marker, ownership_root=None, **options):
            calls[0] += 1
            if calls[0] == 1:
                return (
                    {client.pid: client, postmaster.pid: postmaster},
                    {client.pid, postmaster.pid},
                )
            return ({postmaster.pid: postmaster}, {postmaster.pid})

        def sleep(duration):
            now[0] += max(duration, 0.1)

        with mock.patch.object(
            process_state, "_guarded_process_snapshot",
            side_effect=snapshot,
        ), mock.patch.object(
            process_state, "_guarded_worker_validation",
            return_value=({}, None),
        ), mock.patch.object(
            process_state.os, "killpg",
            side_effect=lambda pgid, sig: group_signals.append((pgid, sig)),
        ), mock.patch.object(
            process_state.os, "kill",
            side_effect=lambda pid, sig: direct_signals.append((pid, sig)),
        ), mock.patch.object(
            process_state, "BUILD_TIMEOUT_TERM_GRACE_SEC", 0.1,
        ):
            with self.assertRaises(
                process_state._GuardedTerminationError,
            ) as raised:
                process_state._terminate_guarded_process_tree(
                    tracker,
                    hard_kill_deadline=0.5,
                    state_mode="benchmark-e2e",
                    clock=lambda: now[0],
                    sleeper=sleep,
                )

        self.assertIn((310, signal.SIGTERM), group_signals)
        self.assertIn((310, signal.SIGTERM), direct_signals)
        self.assertNotIn(311, [pid for pid, unused in group_signals])
        self.assertNotIn(311, [pid for pid, unused in direct_signals])
        self.assertFalse(raised.exception.evidence["cleanup_verified"])
        self.assertEqual(raised.exception.evidence["survivor_pids"], [311])
        self.assertEqual(
            {item.pid for item in tracker.last_quarantined_service.values()},
            {311},
        )
        self.assertTrue(
            tracker.last_service_boundary_evidence[
                "provider_exception_quarantine"])
        reused_client = self._identity(
            310, 1, 310, 310, "reused-client", "benchmark")
        self.assertEqual(
            process_state._provider_exception_quarantine_roots(
                {310: reused_client}, {310}, client),
            (reused_client,),
        )
        with mock.patch.object(
            process_state, "_process_snapshot",
            return_value={310: client, 311: postmaster},
        ):
            groups, evidence = process_state._validated_process_tree(
                client,
                deadline=time.time() + 1,
                guard_marker="6" * 32,
                service_boundary_provider=mock.Mock(
                    side_effect=KeyError("provider failed")),
            )
        self.assertEqual(set(groups), {310})
        self.assertEqual(
            [item["pid"]
             for item in evidence["quarantined_service_identities"]],
            [311],
        )

    def test_provider_exception_latch_blocks_empty_cleanup_success(self):
        client = self._identity(312, 1, 312, 312, "client", "benchmark")
        tracker = process_state._GuardedProcessTracker(
            client,
            "7" * 32,
            service_boundary_provider=mock.Mock(
                side_effect=KeyError("provider failed")),
        )
        calls = [0]
        now = [0.0]
        direct_signals = []

        def snapshot(marker, ownership_root=None, **options):
            calls[0] += 1
            if calls[0] == 1:
                return ({client.pid: client}, {client.pid})
            return ({}, set())

        def sleep(duration):
            now[0] += max(duration, 0.1)

        with mock.patch.object(
            process_state, "_guarded_process_snapshot", side_effect=snapshot,
        ), mock.patch.object(
            process_state, "_guarded_worker_validation",
            return_value=({}, None),
        ), mock.patch.object(
            process_state.os, "killpg",
        ), mock.patch.object(
            process_state.os, "kill",
            side_effect=lambda pid, sig: direct_signals.append((pid, sig)),
        ), mock.patch.object(
            process_state, "BUILD_TIMEOUT_TERM_GRACE_SEC", 0.1,
        ):
            with self.assertRaises(
                process_state._GuardedTerminationError,
            ) as raised:
                process_state._terminate_guarded_process_tree(
                    tracker,
                    hard_kill_deadline=0.5,
                    state_mode="benchmark-e2e",
                    clock=lambda: now[0],
                    sleeper=sleep,
                )
        self.assertIn((312, signal.SIGTERM), direct_signals)
        self.assertEqual(raised.exception.evidence["survivor_pids"], [])
        self.assertTrue(
            raised.exception.evidence["service_boundary_failed"])
        self.assertFalse(raised.exception.evidence["cleanup_verified"])

        with mock.patch.object(
            process_state, "_process_snapshot",
            side_effect=[{312: client}, {312: client}, {}],
        ), mock.patch.object(
            process_state.os, "killpg",
        ), mock.patch.object(
            process_state.os, "kill",
        ), mock.patch.object(
            process_state.time, "time",
            side_effect=[0.0, 0.0, 10.0, 10.0, 10.0],
        ):
            evidence = process_state._terminate_validated_process_tree(
                client,
                hard_kill_deadline=10.0,
                guard_marker="7" * 32,
                service_boundary_provider=mock.Mock(
                    side_effect=KeyError("provider failed")),
            )
        self.assertTrue(evidence["service_boundary_failed"])
        self.assertFalse(evidence["cleanup_verified"])

    def test_initial_classifier_protects_service_after_snapshot_failure(self):
        client = self._identity(313, 1, 313, 313, "client", "benchmark")
        postmaster = self._identity(
            314, 313, 314, 314, "pg", "postgres -D /tmp/q1024")
        initial_provider = (
            postgres_runtime._postgresql_recovery_service_boundary_provider({
                "mode": "benchmark-e2e",
                "postgres_backend_owner_started": True,
            }))
        self.assertIsNotNone(initial_provider)
        full_provider = mock.Mock()
        fake_process = mock.Mock(pid=313)
        fake_process.stdout = mock.Mock()
        snapshots = [0]
        now = [0.0]
        group_signals = []
        direct_signals = []

        def clock():
            # The cleanup reserve below is 20 ms.  Advancing 500 ms on each
            # clock read exhausts it before the post-TERM snapshot can run.
            now[0] += 0.001
            return now[0]

        def snapshot(marker, ownership_root=None, **options):
            snapshots[0] += 1
            if snapshots[0] == 1:
                return ({}, set())
            if snapshots[0] == 2:
                raise RuntimeError("initial snapshot failed")
            if snapshots[0] == 3:
                return (
                    {313: client, 314: postmaster},
                    {313, 314},
                )
            return ({314: postmaster}, {314})

        with tempfile.NamedTemporaryFile(mode="w+") as log_handle, \
                mock.patch.object(
                    process_state, "_load_state",
                    return_value={
                        "benchmark-e2e": {"build_dir": "/tmp/guarded"},
                    },
                ), mock.patch.object(
                    process_state, "_guarded_process_snapshot",
                    side_effect=snapshot,
                ), mock.patch.object(
                    process_state, "_capture_process_identity",
                    return_value=client,
                ), mock.patch.object(
                    process_state.subprocess, "Popen",
                    return_value=fake_process,
                ), mock.patch.object(
                    process_state, "_guarded_worker_validation",
                    return_value=({}, None),
                ), mock.patch.object(
                    process_state.os, "killpg",
                    side_effect=lambda pgid, sig: group_signals.append(
                        (pgid, sig)),
                ), mock.patch.object(
                    process_state.os, "kill",
                    side_effect=lambda pid, sig: direct_signals.append(
                        (pid, sig)),
                ), mock.patch.object(
                    process_state, "_update_entry",
                ), mock.patch.object(
                    process_state, "BUILD_TIMEOUT_TERM_GRACE_SEC", 0.01,
                ):
            with self.assertRaises(
                process_state._GuardedTerminationError,
            ) as raised:
                process_state._stream_subprocess(
                    ["benchmark"],
                    log_handle,
                    deadline=3.0,
                    hard_kill_deadline=3.5,
                    state_mode="benchmark-e2e",
                    require_verified_termination=True,
                    service_boundary_provider=full_provider,
                    initial_service_boundary_provider=initial_provider,
                    deadline_clock=clock,
                )
        self.assertIn((313, signal.SIGTERM), group_signals)
        self.assertIn((313, signal.SIGTERM), direct_signals)
        self.assertNotIn(314, [pid for pid, unused in group_signals])
        self.assertNotIn(314, [pid for pid, unused in direct_signals])
        self.assertFalse(raised.exception.evidence["cleanup_verified"])
        self.assertEqual(raised.exception.evidence["survivor_pids"], [314])
        full_provider.assert_not_called()

    def test_provider_failure_blocks_normal_client_exit(self):
        client = self._identity(315, 1, 315, 315, "client", "benchmark")
        initial_provider = (
            postgres_runtime._postgresql_recovery_service_boundary_provider({
                "mode": "benchmark-e2e",
                "postgres_backend_owner_started": True,
            }))
        self.assertIsNotNone(initial_provider)
        full_provider = mock.Mock(side_effect=KeyError("provider failed"))
        fake_process = mock.Mock(pid=315)
        fake_process.stdout = mock.Mock()
        fake_process.stdout.fileno.return_value = 99
        fake_process.poll.return_value = 0
        fake_process.wait.return_value = 0
        snapshots = [0]
        now = [0.0]

        def clock():
            now[0] += 1.0
            return now[0]

        def snapshot(marker, ownership_root=None, **options):
            snapshots[0] += 1
            if snapshots[0] == 1:
                return ({}, set())
            if snapshots[0] == 2:
                return ({315: client}, {315})
            return ({}, set())

        with tempfile.NamedTemporaryFile(mode="w+") as log_handle, \
                mock.patch.object(
                    process_state, "_load_state",
                    return_value={
                        "benchmark-e2e": {"build_dir": "/tmp/guarded"},
                    },
                ), mock.patch.object(
                    process_state, "_guarded_process_snapshot",
                    side_effect=snapshot,
                ), mock.patch.object(
                    process_state, "_capture_process_identity",
                    return_value=client,
                ), mock.patch.object(
                    process_state.subprocess, "Popen",
                    return_value=fake_process,
                ), mock.patch.object(
                    process_state.select, "select",
                    return_value=([], [], []),
                ), mock.patch.object(
                    process_state, "_guarded_worker_validation",
                    return_value=({}, None),
                ), mock.patch.object(
                    process_state.os, "killpg",
                ), mock.patch.object(
                    process_state.os, "kill",
                ), mock.patch.object(
                    process_state, "_update_entry",
                ):
            with self.assertRaises(
                process_state._GuardedTerminationError,
            ) as raised:
                process_state._stream_subprocess(
                    ["benchmark"],
                    log_handle,
                    deadline=5.0,
                    hard_kill_deadline=6.0,
                    state_mode="benchmark-e2e",
                    require_verified_termination=True,
                    service_boundary_provider=full_provider,
                    initial_service_boundary_provider=initial_provider,
                    deadline_clock=clock,
                )
        self.assertGreaterEqual(full_provider.call_count, 1)
        self.assertEqual(raised.exception.evidence["survivor_pids"], [])
        self.assertTrue(
            raised.exception.evidence["service_boundary_failed"])
        self.assertFalse(raised.exception.evidence["cleanup_verified"])

    def test_dead_worker_without_persisted_boundary_quarantines_postmasters(self):
        token = "7" * 32
        client = self._identity(320, 100, 320, 320, "client", "benchmark")
        postmaster = self._identity(
            321, 320, 321, 321, "pg", "postgres -D /tmp/data")
        entry = {
            "mode": "benchmark-e2e",
            "q1024_run_token": token,
            "postgres_application_name": f"bioseq-q1024-{token}",
            "postgres_backend_owner_started": True,
        }
        provider = (
            postgres_runtime._postgresql_recovery_service_boundary_provider(
                entry))
        self.assertIsNotNone(provider)
        observation = provider(
            {321: postmaster}, client, "8" * 32, time.time() + 1, {321})
        self.assertIsNone(observation.verified_root)
        self.assertEqual(
            {item.pid for item in observation.quarantined_roots}, {321})

    def test_verifier_waits_for_exact_client_arm_and_rejects_stale_token(self):
        token = "9" * 32
        marker = "a" * 32
        client = self._identity(330, 100, 330, 330, "client", "benchmark")
        shell = self._identity(331, 330, 330, 330, "shell", "sh -c")
        verifier = self._identity(
            332, 331, 330, 330, "verifier", "postgres_runtime.py")
        base_entry = {
            "mode": "benchmark-e2e",
            "status": "running",
            "q1024_run_token": token,
            "postgres_application_name": f"bioseq-q1024-{token}",
            "postgres_backend_owner_started": True,
        }
        stale_arm = {
            **base_entry,
            "active_guard_marker": "b" * 32,
            "active_process_identity": client.__dict__,
        }
        exact_arm = {
            **base_entry,
            "active_guard_marker": marker,
            "active_process_identity": client.__dict__,
        }
        now = [0.0]

        def sleep(duration):
            now[0] += duration

        with mock.patch.object(
            postgres_runtime, "_load_state",
            side_effect=[
                {"benchmark-e2e": stale_arm},
                {"benchmark-e2e": exact_arm},
            ]), mock.patch.object(
            postgres_runtime, "_process_snapshot",
            return_value={330: client, 331: shell, 332: verifier},
        ) as snapshot, mock.patch.object(
            postgres_runtime.os, "getpid", return_value=332):
            observed = postgres_runtime._await_q1024_client_boundary_arm(
                mode="benchmark-e2e",
                run_token=token,
                application_name=f"bioseq-q1024-{token}",
                guard_marker=marker,
                clock=lambda: now[0],
                wall_clock=lambda: 100.0,
                sleeper=sleep,
            )
        self.assertEqual(observed, client)
        snapshot.assert_called_once()

        stale_token = {**exact_arm, "q1024_run_token": "c" * 32}
        with mock.patch.object(
            postgres_runtime, "_load_state",
            return_value={"benchmark-e2e": stale_token}), mock.patch.object(
            postgres_runtime, "_process_snapshot") as snapshot:
            with self.assertRaisesRegex(RuntimeError, "stale"):
                postgres_runtime._await_q1024_client_boundary_arm(
                    mode="benchmark-e2e",
                    run_token=token,
                    application_name=f"bioseq-q1024-{token}",
                    guard_marker=marker,
                )
        snapshot.assert_not_called()

    def test_runtime_cli_arms_client_before_postmaster_verification(self):
        token = "d" * 32
        marker = "e" * 32
        client = self._identity(340, 1, 340, 340, "client", "benchmark")
        postmaster = self._identity(341, 340, 341, 341, "pg", "postgres")
        order = []

        def arm(**unused):
            order.append("arm")
            return client

        def verify(*unused):
            order.append("verify")
            return postmaster

        def evidence(**unused):
            order.append("evidence")
            return {"runtime_identity_verified": True}

        def publish(*unused):
            order.append("publish")
            return True

        arguments = [
            "verify-postgresql-runtime-identity",
            "--data-dir", "/tmp/q1024-data",
            "--active-preset", "bench-e2e-opt",
            "--postgres-path", "/tmp/postgres",
            "--postgres-device", "1",
            "--postgres-inode", "2",
            "--postgres-size", "3",
            "--postgres-sha256", "f" * 64,
            "--bioseq-dso-path", "/tmp/bioseq.dylib",
            "--bioseq-dso-device", "4",
            "--bioseq-dso-inode", "5",
            "--bioseq-dso-size", "6",
            "--bioseq-dso-sha256", "0" * 64,
            "--boundary-mode", "benchmark-e2e",
            "--boundary-run-token", token,
            "--boundary-application-name", f"bioseq-q1024-{token}",
        ]
        with mock.patch.dict(
            os.environ, {process_state.BUILD_GUARD_ENV_KEY: marker}, clear=False,
        ), mock.patch.object(
            postgres_runtime, "_await_q1024_client_boundary_arm",
            side_effect=arm,
        ), mock.patch.object(
            postgres_runtime, "_verify_postgresql_runtime_identity",
            side_effect=verify,
        ), mock.patch.object(
            postgres_runtime, "_postgresql_service_boundary_evidence",
            side_effect=evidence,
        ), mock.patch.object(
            postgres_runtime, "_publish_q1024_postgresql_service_boundary",
            side_effect=publish,
        ):
            self.assertEqual(
                postgres_runtime._postgresql_runtime_identity_cli(arguments), 0)
        self.assertEqual(order, ["arm", "verify", "evidence", "publish"])

    def test_q1024_nonzero_and_timeout_are_not_converted_to_success(self):
        provider = object()
        initial_provider = object()
        common = (
            mock.patch.object(build_worker, "_require_control_deadline"),
            mock.patch.object(
                build_worker, "_run_command_argv", return_value=["bench"]),
            mock.patch.object(
                build_worker, "_cache_apply_runtime_budget",
                side_effect=lambda mode, command, deadline: command),
            mock.patch.object(
                build_worker, "_cache_command_deadline",
                side_effect=lambda mode, command, deadline: deadline),
            mock.patch.object(
                build_worker, "_next_report_path",
                return_value=Path("/tmp/q1024-report.json")),
            mock.patch.object(build_worker, "_require_allocated_command_report"),
            mock.patch.object(build_worker, "_update_entry"),
        )
        with common[0], common[1], common[2], common[3], common[4], \
                common[5], common[6], mock.patch.object(
                    build_worker, "_stream_subprocess", return_value=2,
                ) as stream:
            with self.assertRaisesRegex(RuntimeError, "q1024 guarded client"):
                build_worker._run_mode_commands(
                    "benchmark-e2e", Path("/tmp/bin"), Path("/tmp/build"),
                    {}, io.StringIO(), time.time() + 10, set(),
                    hard_kill_deadline=time.time() + 20,
                    service_boundary_provider=provider,
                    initial_service_boundary_provider=initial_provider,
                )
        self.assertIs(
            stream.call_args.kwargs["service_boundary_provider"], provider)
        self.assertIs(
            stream.call_args.kwargs["initial_service_boundary_provider"],
            initial_provider,
        )

        timeout = TimeoutError("bounded timeout")
        with common[0], common[1], common[2], common[3], common[4], \
                common[5], common[6], mock.patch.object(
                    build_worker, "_stream_subprocess", side_effect=timeout,
                ):
            with self.assertRaises(TimeoutError):
                build_worker._run_mode_commands(
                    "benchmark-e2e", Path("/tmp/bin"), Path("/tmp/build"),
                    {}, io.StringIO(), time.time() + 10, set(),
                    hard_kill_deadline=time.time() + 20,
                    service_boundary_provider=provider,
                    initial_service_boundary_provider=initial_provider,
                )

    def test_legacy_global_zero_rejects_candidates_and_runtime_mismatch(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            bindir = build_dir / "pgbin"
            bindir.mkdir()
            postgres = bindir / "postgres"
            postgres.write_bytes(b"server")
            entry = self._legacy_entry(build_dir)
            metadata = postgres.stat()
            sealed = postgres_runtime._PostgreSQLFileIdentity(
                str(postgres), metadata.st_dev, metadata.st_ino,
                metadata.st_size, "f" * 64)
            patches = (
                mock.patch.object(
                    postgres_runtime, "_pg_config_from_cache",
                    return_value="/configured/pg_config"),
                mock.patch.object(
                    postgres_runtime, "_postgresql_config_path",
                    return_value=str(bindir)),
                mock.patch.object(
                    postgres_runtime, "_cmake_cache_value",
                    return_value=str(postgres)),
                mock.patch.object(
                    postgres_runtime, "_seal_postgresql_file_identity",
                    return_value=sealed),
            )
            with patches[0], patches[1], patches[2], patches[3], \
                    mock.patch.object(
                        postgres_runtime, "_process_snapshot", return_value={}), \
                    mock.patch.object(postgres_runtime.os, "kill") as signal_pid, \
                    mock.patch.object(postgres_runtime.subprocess, "run") as sql:
                evidence = (
                    postgres_runtime._legacy_q1024_global_postgresql_zero_evidence(
                        entry=entry,
                        active_build_dir=build_dir,
                        active_preset="bench-e2e-opt",
                        deadline=time.time() + 5,
                    ))
            self.assertTrue(evidence["cleanup_verified"])
            signal_pid.assert_not_called()
            sql.assert_not_called()

            other_cluster = self._identity(
                500, 1, 500, 500, "other", "postgres: checkpointer")
            with patches[0], patches[1], patches[2], patches[3], \
                    mock.patch.object(
                        postgres_runtime, "_process_snapshot",
                        return_value={500: other_cluster}):
                rejected = (
                    postgres_runtime._legacy_q1024_global_postgresql_zero_evidence(
                        entry=entry,
                        active_build_dir=build_dir,
                        active_preset="bench-e2e-opt",
                        deadline=time.time() + 5,
                    ))
            self.assertFalse(rejected["cleanup_verified"])
            self.assertEqual(
                rejected["postgresql_candidates"][0]["process_identity"]["pid"],
                500,
            )

            # This fixture has no socket or postmaster.pid.  Their absence is
            # deliberately not authority when the global snapshot is not
            # complete.
            self.assertFalse((build_dir / ".s.PGSQL.5432").exists())
            self.assertFalse((build_dir / "postmaster.pid").exists())
            with patches[0], patches[1], patches[2], patches[3], \
                    mock.patch.object(
                        postgres_runtime, "_process_snapshot",
                        side_effect=RuntimeError(
                            "incomplete snapshot: PermissionError")):
                incomplete = (
                    postgres_runtime._legacy_q1024_global_postgresql_zero_evidence(
                        entry=entry,
                        active_build_dir=build_dir,
                        active_preset="bench-e2e-opt",
                        deadline=time.time() + 5,
                    ))
            self.assertFalse(incomplete["cleanup_verified"])
            self.assertIn("PermissionError", incomplete["error"])

            changed = postgres_runtime._PostgreSQLFileIdentity(
                str(postgres), metadata.st_dev, metadata.st_ino + 1,
                metadata.st_size, "f" * 64)
            with patches[0], patches[1], patches[2], mock.patch.object(
                postgres_runtime, "_seal_postgresql_file_identity",
                side_effect=[sealed, changed],
            ), mock.patch.object(
                postgres_runtime, "_process_snapshot",
                side_effect=AssertionError("snapshot must follow identity proof"),
            ):
                rejected = (
                    postgres_runtime._legacy_q1024_global_postgresql_zero_evidence(
                        entry=entry,
                        active_build_dir=build_dir,
                        active_preset="bench-e2e-opt",
                        deadline=time.time() + 5,
                    ))
            self.assertFalse(rejected["cleanup_verified"])
            self.assertIn("differ", rejected["error"])

    def test_terminal_revalidation_uses_historical_receipt_only_for_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = (Path(temporary) / "build").resolve()
            receipt = CanonicalClangE2EReceiptPortabilityTests._receipt(
                build_dir,
                preset="bench-e2e-opt",
                system="Darwin",
                machine="arm64",
                compiler="/opt/homebrew/opt/llvm@22/bin/clang++",
                compiler_id=None,
                compiler_version=None,
                flags="-O3 -DNDEBUG -fno-omit-frame-pointer",
            )
            entry = self._legacy_entry(build_dir)
            receipt.update({
                "build_receipt_id": "br_build33",
                "content_sha256": "1" * 64,
            })
            captured = {}

            def finish(mode, token, claim_token, evidence):
                captured.update(evidence)
                return evidence.get("cleanup_verified") is True

            with mock.patch.object(
                build_worker, "_load_state",
                return_value={"benchmark-e2e": entry}), mock.patch.object(
                build_worker, "_algorithm_matrix_campaign_authority_status",
                return_value=({
                    "name": "q1024", "execution_role": "q1024_singleton",
                }, None)), mock.patch.object(
                build_worker, "_validate_build_receipt",
                return_value=(receipt, None),
            ) as validate, mock.patch.object(
                build_worker, "_claim_q1024_postgresql_revalidation",
                return_value=("claim", dict(entry))), mock.patch.object(
                build_worker,
                "_legacy_q1024_global_postgresql_zero_evidence",
                return_value={"cleanup_verified": True}), mock.patch.object(
                build_worker, "_finish_q1024_postgresql_revalidation",
                side_effect=finish), mock.patch.object(
                build_worker, "_seal_run_receipt",
                side_effect=AssertionError("no receipt may be sealed")):
                verified = (
                    build_worker
                    ._revalidate_legacy_q1024_postgresql_cleanup_before_admission(
                        request_deadline=time.time() + 30))
            self.assertTrue(verified)
            self.assertNotIn("verify_source", validate.call_args.kwargs)
            self.assertLessEqual(
                validate.call_args.kwargs["deadline"], time.time() + 15.1)
            self.assertEqual(captured["preserved_terminal_status"], "aborted")
            self.assertIsNone(captured["preserved_terminal_exit_code"])

    def test_terminal_claim_rejects_live_claimant_and_finish_preserves_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            state_path = root / "state.json"
            lock_path = root / "state.lock"
            entry = self._legacy_entry(root)
            claimant = self._identity(
                900, 1, 900, 900, "server", "server.py")
            previous = self._identity(
                901, 1, 901, 901, "other", "server.py")
            entry["postgres_cleanup_claim"] = {
                "actor": "server_revalidator",
                "claim_token": "prior",
                "process_identity": previous.__dict__,
            }
            state_path.write_text(
                json.dumps({"benchmark-e2e": entry}), encoding="utf-8")
            token = str(entry["q1024_run_token"])
            with mock.patch.object(process_state, "STATE_FILE", state_path), \
                    mock.patch.object(process_state, "LOCK_FILE", lock_path), \
                    mock.patch.object(process_state.os, "getpid", return_value=900), \
                    mock.patch.object(
                        process_state, "_process_snapshot",
                        return_value={900: claimant, 901: previous}):
                self.assertIsNone(
                    process_state._claim_q1024_postgresql_revalidation(
                        "benchmark-e2e", token, deadline=time.time() + 1))
                self.assertIsNone(
                    process_state._claim_q1024_postgresql_revalidation(
                        "benchmark-e2e", "wrong", deadline=time.time() + 1))

            entry.pop("postgres_cleanup_claim")
            state_path.write_text(
                json.dumps({"benchmark-e2e": entry}), encoding="utf-8")
            with mock.patch.object(process_state, "STATE_FILE", state_path), \
                    mock.patch.object(process_state, "LOCK_FILE", lock_path), \
                    mock.patch.object(process_state.os, "getpid", return_value=900), \
                    mock.patch.object(
                        process_state, "_process_snapshot",
                        return_value={900: claimant}):
                claim = process_state._claim_q1024_postgresql_revalidation(
                    "benchmark-e2e", token, deadline=time.time() + 1)
                self.assertIsNotNone(claim)
                claim_token, _ = claim
                self.assertTrue(
                    process_state._finish_q1024_postgresql_revalidation(
                        "benchmark-e2e", token, claim_token,
                        {"cleanup_verified": True}))
                terminal = process_state._load_state()["benchmark-e2e"]
            self.assertEqual(terminal["status"], "aborted")
            self.assertIsNone(terminal["exit_code"])
            self.assertEqual(terminal["report_files"], ["/tmp/report274.json"])
            self.assertFalse(terminal["postgres_backend_owner_started"])
            self.assertFalse(terminal["postgres_cleanup_unverified"])
            self.assertTrue(terminal["cleanup_unverified"])

if __name__ == "__main__":
    unittest.main()
