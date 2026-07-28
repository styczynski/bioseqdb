from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


BUILD_MCP_DIR = Path(__file__).resolve().parents[1]
MAIN_DIR = BUILD_MCP_DIR.parent
sys.path.insert(0, str(BUILD_MCP_DIR))

import server  # noqa: E402


class PostgresErrorSafetyPresetContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        payload = json.loads((MAIN_DIR / "CMakePresets.json").read_text())
        cls.presets = {
            preset["name"]: preset
            for preset in payload["configurePresets"]
        }

    def test_presets_are_fixed_isolated_profile_trees(self) -> None:
        expected = {
            server.POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET: (
                "bench-opt",
                "OFF",
                "OFF",
            ),
            server.POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET: (
                "test-opt",
                "ON",
                "ON",
            ),
        }
        for name, (parent, build_tests, test_indexes) in expected.items():
            with self.subTest(name=name):
                preset = self.presets[name]
                variables = preset["cacheVariables"]
                self.assertEqual(preset["inherits"], parent)
                self.assertEqual(
                    preset["binaryDir"],
                    "${sourceDir}/build/${presetName}",
                )
                self.assertEqual(
                    variables["BIOSEQ_BUILD_TESTS"], build_tests)
                self.assertEqual(
                    variables["BIOSEQ_TEST_INDEXES"], test_indexes)
                self.assertEqual(
                    variables[
                        "BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE"],
                    "${sourceDir}/codegen/safety/fixtures/"
                    "doc0388_runtime_cases.json",
                )
                self.assertEqual(
                    variables[
                        "BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE"],
                    "isolated",
                )
                self.assertEqual(
                    variables["BIOSEQ_BUILD_EXTERNAL_API_CLIENTS"],
                    "OFF",
                )
                self.assertEqual(
                    variables["BIOSEQ_BUILD_WEBSITE"], "OFF")

    def test_production_preset_names_exact_peer_tree(self) -> None:
        variables = self.presets[
            server.POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET
        ]["cacheVariables"]
        self.assertEqual(
            variables[
                server.POSTGRES_ERROR_SAFETY_PEER_BUILD_DIR_CACHE_KEY],
            "${sourceDir}/build/"
            + server.POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET,
        )
        self.assertNotIn(
            server.POSTGRES_ERROR_SAFETY_PEER_BUILD_DIR_CACHE_KEY,
            self.presets[
                server.POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET
            ]["cacheVariables"],
        )


class PostgresErrorSafetyModeContractTest(unittest.TestCase):
    def test_mode_routes_to_two_fixed_trees_and_private_cluster(self) -> None:
        mode = server.POSTGRES_ERROR_SAFETY_MODE
        details = server.MODES[mode]
        self.assertEqual(
            server.preset_for(mode, False),
            server.POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
        )
        self.assertEqual(
            server.build_dir_for(mode, False),
            MAIN_DIR
            / "build"
            / server.POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
        )
        self.assertEqual(
            server.build_dirs_for(mode, False),
            (
                MAIN_DIR
                / "build"
                / server.POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
                MAIN_DIR
                / "build"
                / server.POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET,
            ),
        )
        self.assertTrue(details.uses_private_cluster)
        self.assertFalse(details.install_extension)
        self.assertEqual(details.lock_group, mode)
        self.assertEqual(details.targets, [])
        self.assertEqual(details.commands, [])

    def test_build_sequence_is_profiles_then_candidate_then_evidence(
            self) -> None:
        profiles = server.POSTGRES_ERROR_SAFETY_RUNTIME_PROFILES_TARGET
        expected_build_only = (
            (
                server.POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET,
                profiles,
            ),
            (
                server.POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
                profiles,
            ),
        )
        self.assertEqual(
            server._postgres_error_safety_build_sequence(True),
            expected_build_only,
        )
        self.assertEqual(
            server._postgres_error_safety_build_sequence(False),
            expected_build_only + (
                (
                    server.POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
                    server.POSTGRES_ERROR_SAFETY_CANDIDATE_TARGET,
                ),
                (
                    server.POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
                    server.POSTGRES_ERROR_SAFETY_EVIDENCE_TARGET,
                ),
            ),
        )

    def test_candidate_target_is_non_all_and_owns_all_six_outputs(
            self) -> None:
        cmake = (MAIN_DIR / "bioseq" / "CMakeLists.txt").read_text(
            encoding="utf-8")
        marker = (
            "add_custom_target("
            + server.POSTGRES_ERROR_SAFETY_CANDIDATE_TARGET
        )
        aggregate = cmake.index(
            "set(_bioseq_sql_runtime_candidate_root")
        start = cmake.index(marker, aggregate)
        end = cmake.index(
            "add_custom_target("
            + server.POSTGRES_ERROR_SAFETY_EVIDENCE_TARGET,
            start,
        )
        target = cmake[start:end]
        self.assertNotIn(" ALL", target)
        for output in (
            "_bioseq_sql_runtime_candidate_production_ledger",
            "_bioseq_sql_runtime_candidate_test_indexes_ledger",
            "_bioseq_sql_runtime_candidate_matrix",
            "_bioseq_sql_runtime_candidate_stamp",
            "_bioseq_sql_runtime_candidate_production_stage_manifest",
            "_bioseq_sql_runtime_candidate_test_indexes_stage_manifest",
        ):
            with self.subTest(output=output):
                self.assertIn(f'"${{{output}}}"', target)

    def test_runtime_candidate_is_not_in_the_ordinary_extension_link_gate(
            self) -> None:
        cmake = (MAIN_DIR / "bioseq" / "CMakeLists.txt").read_text(
            encoding="utf-8")
        start = cmake.index(
            "set(BIOSEQ_SQL_POSTGRES_EXTENSION_PROOF_STAMP")
        end = cmake.index(
            "if(TARGET bioseq_gpu_archive_builder)",
            start,
        )
        ordinary_link_gate = cmake[start:end]
        self.assertNotIn(
            server.POSTGRES_ERROR_SAFETY_CANDIDATE_TARGET,
            ordinary_link_gate,
        )
        self.assertNotIn(
            server.POSTGRES_ERROR_SAFETY_EVIDENCE_TARGET,
            ordinary_link_gate,
        )

    def test_local_tool_rejects_debug_filter_and_debug_report_inputs(
            self) -> None:
        mode = server.POSTGRES_ERROR_SAFETY_MODE
        checks = (
            (
                {"debug": True},
                "`debug` input",
            ),
            (
                {"debug": False, "debug_supplied": True},
                "`debug` input",
            ),
            (
                {"bench_filter": {}},
                "`filter` input",
            ),
            (
                {"bench_filter": None, "filter_supplied": True},
                "`filter` input",
            ),
            (
                {
                    "debug_report": False,
                    "debug_report_supplied": True,
                },
                "`debug_report` input",
            ),
        )
        with mock.patch.object(
                server, "_refuse_root_build_identity", return_value=None), \
                mock.patch.object(
                    server, "_ensure_build_worker_timesharing_policy"):
            for arguments, expected in checks:
                with self.subTest(arguments=arguments):
                    result = server.tool_start_build(mode, **arguments)
                    self.assertIn(expected, result)

    def test_full_mode_requires_waiver_and_build_only_rejects_it(
            self) -> None:
        mode = server.POSTGRES_ERROR_SAFETY_MODE
        with mock.patch.object(
                server, "_refuse_root_build_identity", return_value=None), \
                mock.patch.object(
                    server, "_ensure_build_worker_timesharing_policy"):
            missing = server.tool_start_build(mode)
            build_only = server.tool_start_build(
                mode,
                build_only=True,
                postgres_error_safety_review_waiver="waiver.json",
            )
            wrong_mode = server.tool_start_build(
                "unit+integration",
                build_only=True,
                postgres_error_safety_review_waiver="waiver.json",
            )
        self.assertIn("requires", missing)
        self.assertIn("does not accept", build_only)
        self.assertIn("accepted only", wrong_mode)

    def test_worker_snapshot_is_canonical_and_bound_to_state(self) -> None:
        waiver_content = {
            "schema_version": 1,
            "artifact_kind":
                "doc0388_postgres_error_safety_review_waiver",
            "approval_reference":
                "User instruction dated 2026-07-26: skip any reviews.",
            "waiver_reason":
                "User explicitly waived independent review for DOC0388.",
            "independent_review_performed": False,
        }
        snapshot = {
            **waiver_content,
            "waiver_request_path": str(
                MAIN_DIR.parent
                / "DOC0388_SQL_GUARDRAIL_EVIDENCE"
                / "postgres_error_safety_review_waiver.json"
            ),
            "waiver_request_file_sha256": "1" * 64,
            "waiver_request_content_sha256": hashlib.sha256(json.dumps(
                waiver_content,
                sort_keys=True,
                separators=(",", ":"),
            ).encode("utf-8")).hexdigest(),
        }
        serialized = json.dumps(
            snapshot,
            sort_keys=True,
            separators=(",", ":"),
        )
        state = {
            "postgres_error_safety_review_waiver_sha256":
                hashlib.sha256(serialized.encode("utf-8")).hexdigest(),
        }
        self.assertEqual(
            server._checked_postgres_error_safety_review_waiver_snapshot(
                state,
                False,
                serialized,
            ),
            snapshot,
        )
        with self.assertRaisesRegex(RuntimeError, "scheduling state"):
            server._checked_postgres_error_safety_review_waiver_snapshot(
                {
                    "postgres_error_safety_review_waiver_sha256": "0" * 64,
                },
                False,
                serialized,
            )
        with self.assertRaisesRegex(RuntimeError, "does not accept"):
            server._checked_postgres_error_safety_review_waiver_snapshot(
                {},
                True,
                serialized,
            )

    def test_remote_run_rejects_mode_before_dispatch(self) -> None:
        mode = server.POSTGRES_ERROR_SAFETY_MODE
        self.assertIn(mode, server.REMOTE_RUN_REJECTED_MODES)
        self.assertNotIn(mode, server.REMOTE_RUN_BUILD_MODES)
        with mock.patch.object(
                server, "_current_local_branch", return_value="main"):
            result = server.tool_remote_run({
                "operation": "build",
                "mode": mode,
            })
        self.assertIn("local-only", result)
        self.assertIn("not supported by remote-run", result)

    def test_mcp_schema_exposes_only_local_mode(self) -> None:
        definitions = {
            definition["name"]: definition
            for definition in server.TOOL_DEFINITIONS
        }
        local_modes = definitions["start_build"]["inputSchema"][
            "properties"]["mode"]["enum"]
        remote_modes = definitions["remote-run"]["inputSchema"][
            "oneOf"][0]["properties"]["mode"]["enum"]
        self.assertIn(server.POSTGRES_ERROR_SAFETY_MODE, local_modes)
        self.assertNotIn(
            server.POSTGRES_ERROR_SAFETY_MODE, remote_modes)
        waiver = definitions["start_build"]["inputSchema"][
            "properties"]["postgres_error_safety_review_waiver"]
        self.assertEqual(waiver["type"], "string")
        self.assertIn(
            "independent_review_performed=false",
            waiver["description"],
        )

    def test_secondary_tree_participates_in_collision_detection(self) -> None:
        secondary = (
            MAIN_DIR
            / "build"
            / server.POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET)
        state = {
            "bench": {
                "status": "running",
                "pid": 123,
                "build_dir": str(secondary),
                "build_only": True,
                "debug": False,
            },
        }
        with mock.patch.object(
                server,
                "_refresh",
                side_effect=lambda current, mode: current.get(mode),
        ):
            running_mode, entry = server._running_in_same_group(
                state,
                server.POSTGRES_ERROR_SAFETY_MODE,
                True,
                MAIN_DIR
                / "build"
                / server.POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
            )
        self.assertEqual(running_mode, "bench")
        self.assertIs(entry, state["bench"])

    def test_exact_dso_resolution_rejects_missing_ambiguous_and_symlink(
            self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary)
            artifact_dir = build_dir / "bioseq"
            artifact_dir.mkdir()

            with self.assertRaisesRegex(RuntimeError, "found 0"):
                server._resolve_postgres_error_safety_dso(build_dir)

            dylib = artifact_dir / "bioseq.dylib"
            dylib.touch()
            self.assertEqual(
                server._resolve_postgres_error_safety_dso(build_dir),
                dylib.resolve(),
            )

            shared_object = artifact_dir / "bioseq.so"
            shared_object.touch()
            with self.assertRaisesRegex(RuntimeError, "found 2"):
                server._resolve_postgres_error_safety_dso(build_dir)

            shared_object.unlink()
            dylib.unlink()
            dylib.symlink_to(artifact_dir / "missing.dylib")
            with self.assertRaisesRegex(RuntimeError, "must not be symlinks"):
                server._resolve_postgres_error_safety_dso(build_dir)


if __name__ == "__main__":
    unittest.main()
