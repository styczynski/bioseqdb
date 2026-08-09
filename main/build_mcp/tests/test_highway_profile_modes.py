from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from unittest import mock


BUILD_MCP_DIR = Path(__file__).resolve().parents[1]
MAIN_DIR = BUILD_MCP_DIR.parent
sys.path.insert(0, str(BUILD_MCP_DIR))

import server  # noqa: E402


PROFILE_MODES = {
    "highway-profile1": ("test-opt-level1", "1"),
    "highway-profile2": ("test-opt-level2", "2"),
}


class HighwayProfilePresetContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        payload = json.loads((MAIN_DIR / "CMakePresets.json").read_text())
        cls.presets = {
            preset["name"]: preset
            for preset in payload["configurePresets"]
        }

    def test_profile_presets_are_isolated_from_the_everyday_tree(self) -> None:
        self.assertEqual(
            self.presets["test-opt"]["cacheVariables"][
                "BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE"],
            "0",
        )
        for preset_name, profile in PROFILE_MODES.values():
            with self.subTest(preset=preset_name):
                preset = self.presets[preset_name]
                self.assertEqual(preset["inherits"], "test-opt")
                self.assertEqual(
                    preset["binaryDir"],
                    "${sourceDir}/build/${presetName}",
                )
                self.assertEqual(
                    preset["cacheVariables"][
                        "BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE"],
                    profile,
                )


class HighwayProfileModeContractTest(unittest.TestCase):
    def test_modes_pin_the_production_extension_compile_gate(self) -> None:
        for mode, (preset_name, profile) in PROFILE_MODES.items():
            with self.subTest(mode=mode):
                details = server.MODES[mode]
                self.assertTrue(details.requires_build_only)
                self.assertEqual(details.lock_group, "tests")
                self.assertEqual(details.preset_family, "test")
                self.assertEqual(details.preset_override, preset_name)
                self.assertEqual(details.targets, ["postgresql_bioseq"])
                self.assertEqual(details.commands, [])
                self.assertEqual(details.post_build_targets, ())
                self.assertFalse(details.install_extension)
                self.assertEqual(
                    details.required_cache_variables,
                    {
                        "BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE": profile,
                        "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
                        "BIOSEQ_BUILD_WEBSITE": "OFF",
                    },
                )
                self.assertEqual(server.preset_for(mode, False), preset_name)
                self.assertEqual(server.preset_for(mode, True), preset_name)
                self.assertEqual(
                    server.build_dir_for(mode, False),
                    MAIN_DIR / "build" / preset_name,
                )

    def test_modes_are_local_only_and_documented_as_build_only(self) -> None:
        documentation = server.get_modes_short_documentation()
        local_schema = next(
            definition for definition in server.TOOL_DEFINITIONS
            if definition["name"] == "start_build"
        )["inputSchema"]
        remote_schema = next(
            definition for definition in server.TOOL_DEFINITIONS
            if definition["name"] == "remote-run"
        )["inputSchema"]
        remote_build_shape = next(
            shape for shape in remote_schema["oneOf"]
            if shape.get("title") == "build"
        )

        for mode in PROFILE_MODES:
            with self.subTest(mode=mode):
                self.assertIn(mode, server.FIXED_BUILD_ONLY_MODES)
                self.assertIn(mode, server.REMOTE_RUN_REJECTED_MODES)
                self.assertNotIn(mode, server.REMOTE_RUN_BUILD_MODES)
                self.assertIn(
                    mode,
                    local_schema["properties"]["mode"]["enum"],
                )
                self.assertNotIn(
                    mode,
                    remote_build_shape["properties"]["mode"]["enum"],
                )
                self.assertIn(
                    f"'{mode}'",
                    documentation,
                )
        self.assertIn("local build-only only", documentation)
        self.assertEqual(
            local_schema["properties"]["build_timeout_sec"]["maximum"],
            3300,
        )
        for field in (
            "fresh",
            "build_timeout_sec",
            "baseline_ref",
            "run_only",
            "reuse_build_receipt",
        ):
            self.assertIn(field, local_schema["properties"])

    def test_fixed_mode_definition_rejects_runtime_ownership(self) -> None:
        base = {
            "lock_group": "tests",
            "expected_duration_min": 1.0,
            "build_only_expected_duration_min": 1.0,
            "label": "fixed gate",
            "is_verb_plular": False,
            "targets": ["postgresql_bioseq"],
            "commands": [],
            "docs": "contract fixture",
            "requires_build_only": True,
        }
        forbidden = (
            {"commands": [["runtime-command"]]},
            {"post_build_targets": ("runtime-target",)},
            {"install_extension": True},
        )
        for override in forbidden:
            with self.subTest(override=override), self.assertRaisesRegex(
                ValueError,
                "fixed build-only modes",
            ):
                server.ModeDefinition(**(base | override))


class HighwayProfileInputGuardTest(unittest.TestCase):
    def test_public_entry_rejects_every_nonfixed_input(self) -> None:
        invalid_requests = (
            {},
            {"build_only": True, "debug_supplied": True},
            {"build_only": True, "debug": True},
            {"build_only": True, "filter_supplied": True},
            {"build_only": True, "bench_filter": {"tags": ["x"]}},
            {"build_only": True, "debug_report_supplied": True},
            {"build_only": True, "debug_report": True},
            {"build_only": True, "bench_e2e_config": "config.json"},
            {
                "build_only": True,
                "postgres_error_safety_review_waiver": "waiver.json",
            },
        )
        with mock.patch.object(
            server,
            "_refuse_root_build_identity",
            return_value=None,
        ), mock.patch.object(
            server,
            "_ensure_build_worker_timesharing_policy",
        ):
            for mode in PROFILE_MODES:
                for request in invalid_requests:
                    with self.subTest(mode=mode, request=request):
                        message = server.tool_start_build(mode, **request)
                        self.assertIn("fixed local compile gate", message)
                        self.assertIn("requires build_only=true", message)

    def test_profiles_reject_baseline_and_receipt_runtime_controls(self) -> None:
        invalid_requests = (
            {"build_only": True, "run_only": True},
            {
                "build_only": True,
                "fresh": True,
                "baseline_ref": "refs/heads/main",
            },
            {
                "build_only": True,
                "reuse_build_receipt": "br_invalid",
            },
        )
        with mock.patch.object(
            server,
            "_refuse_root_build_identity",
            return_value=None,
        ), mock.patch.object(
            server,
            "_ensure_build_worker_timesharing_policy",
        ):
            for mode in PROFILE_MODES:
                for request in invalid_requests:
                    with self.subTest(mode=mode, request=request):
                        message = server.tool_start_build(mode, **request)
                        self.assertTrue(
                            "run_only" in message
                            or "fixed local compile gate" in message
                        )

    def test_detached_worker_fails_closed_before_subprocess_work(self) -> None:
        invalid_requests = (
            {"build_only": False},
            {"build_only": True, "debug": True},
            {"build_only": True, "bench_flags": ["--debug"]},
            {"build_only": True, "test_flags": ["[index]"]},
            {"build_only": True, "bench_e2e_config": "config.json"},
            {"build_only": True, "sql_guardrail_request_json": "{}"},
            {
                "build_only": True,
                "postgres_error_safety_review_waiver_json": "{}",
            },
        )
        with mock.patch.object(
            server,
            "_refuse_root_build_identity",
            return_value=None,
        ), mock.patch.object(
            server,
            "_ensure_build_worker_timesharing_policy",
        ), mock.patch.object(
            server,
            "_load_state",
            side_effect=lambda: {
                mode: {"reserved": True}
                for mode in PROFILE_MODES
            },
        ), mock.patch.object(server, "_stream_subprocess") as run:
            for mode in PROFILE_MODES:
                for request in invalid_requests:
                    with self.subTest(mode=mode, request=request):
                        self.assertEqual(
                            server._run_build_worker(mode, **request),
                            1,
                        )
            run.assert_not_called()

    def test_synchronous_worker_fails_before_reserving_state(self) -> None:
        invalid_requests = (
            {"build_only": False},
            {"build_only": True, "debug": True},
            {"build_only": True, "bench_flags": ["--debug"]},
            {"build_only": True, "test_flags": ["[index]"]},
            {"build_only": True, "bench_e2e_config": "config.json"},
            {"build_only": True, "sql_guardrail_request_json": "{}"},
            {
                "build_only": True,
                "postgres_error_safety_review_waiver_json": "{}",
            },
        )
        with mock.patch.object(
            server,
            "_refuse_root_build_identity",
            return_value=None,
        ), mock.patch.object(
            server,
            "_ensure_build_worker_timesharing_policy",
        ), mock.patch.object(server, "_load_state") as load_state:
            for mode in sorted(server.FIXED_BUILD_ONLY_MODES):
                for request in invalid_requests:
                    with self.subTest(mode=mode, request=request):
                        self.assertEqual(
                            server._run_build_inline(mode, **request),
                            2,
                        )
            load_state.assert_not_called()


if __name__ == "__main__":
    unittest.main()
