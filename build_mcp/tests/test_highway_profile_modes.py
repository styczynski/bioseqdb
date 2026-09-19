from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from unittest import mock


BUILD_MCP_DIR = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = BUILD_MCP_DIR.parent
sys.path.insert(0, str(BUILD_MCP_DIR))

import build_presets  # noqa: E402
import build_worker  # noqa: E402
import protocol  # noqa: E402
import process_state  # noqa: E402
import remote  # noqa: E402


PROFILE_MODES = {
    "highway-scheduler-all-alphabets-uint64": (
        "test-optimized-all-alphabets-uint64",
        "all_alphabets_uint64",
    ),
    "highway-scheduler-all-coordinates": (
        "test-optimized-all-coordinates",
        "all_coordinates",
    ),
    "highway-scheduler-full-specialization": (
        "test-optimized-full-specialization",
        "full_specialization",
    ),
}
REMOVED_MODE_KEYS = frozenset({
    "unit+integration",
    "bench",
    "bench-fast",
    "bench-io",
    "bench-e2e",
    "cache-bench",
    "cache-bioseq-bench",
})
RUNTIME_PROFILE_MODE = build_presets.ALL_ALPHABETS_UNIT_INTEGRATION_MODE


class HighwayProfilePresetContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        payload = json.loads((REPOSITORY_ROOT / "CMakePresets.json").read_text())
        cls.presets = {
            preset["name"]: preset
            for preset in payload["configurePresets"]
        }

    def test_profile_presets_are_isolated_from_the_everyday_tree(self) -> None:
        self.assertEqual(
            self.presets["test-opt"]["cacheVariables"][
                "BIOSEQ_TEMPLATE_FLATTEN_PROFILE"],
            "dna4_uint64",
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
                        "BIOSEQ_TEMPLATE_FLATTEN_PROFILE"],
                    profile,
                )


class HighwayProfileModeContractTest(unittest.TestCase):
    def test_protocol_registers_each_loaded_tool_once(self) -> None:
        names = [
            definition["name"]
            for definition in protocol.TOOL_DEFINITIONS
        ]
        self.assertEqual(len(names), 18)
        self.assertEqual(len(names), len(set(names)))

    def test_remote_run_schema_and_runtime_close_operation_shapes(self) -> None:
        definitions = {
            definition["name"]: definition
            for definition in protocol.TOOL_DEFINITIONS
        }
        for tool_name in (
            "start_build",
            "inspect",
            "remote-status",
            "remote-debug",
            "remote-status-ps",
            "remote-manage",
            "remote-ssh",
            "remote-snapshot",
            "remote-git",
        ):
            self.assertFalse(
                definitions[tool_name]["inputSchema"][
                    "additionalProperties"]
            )
        subagent_schema = definitions["subagent"]["inputSchema"]
        self.assertEqual(len(subagent_schema["oneOf"]), 8)
        for shape in subagent_schema["oneOf"]:
            self.assertFalse(shape["additionalProperties"])
        create_shape = next(
            shape for shape in subagent_schema["oneOf"]
            if shape["title"] == "create"
        )
        expected_tiers = [
            "contractor-intern", "intern", "contractor", "mid", "senior",
            "specialist",
        ]
        self.assertEqual(
            create_shape["properties"]["reasoning_effort"]["enum"],
            expected_tiers,
        )
        self.assertEqual(
            create_shape["properties"]["highest_spawn_tier"]["enum"],
            [None, *expected_tiers],
        )
        self.assertIn(
            "separate Cursor account quota",
            create_shape["properties"]["reasoning_effort"]["description"],
        )
        repurpose_shape = next(
            shape for shape in subagent_schema["oneOf"]
            if shape["title"] == "repurpose"
        )
        self.assertEqual(
            repurpose_shape["properties"]["highest_spawn_tier"]["enum"],
            [None, *expected_tiers],
        )
        remote_schema = definitions["remote-run"]["inputSchema"]
        for shape in remote_schema["oneOf"]:
            self.assertFalse(shape["additionalProperties"])
        with self.assertRaisesRegex(
                RuntimeError, "unknown argument.*function_name"):
            protocol._reject_unknown_tool_arguments(
                "remote-run",
                {
                    "operation": "build",
                    "mode": "cache-unit",
                    "function_name": "cross-shape argument",
                },
            )

    def test_subagent_public_dispatch_preserves_runtime_error_envelope(self) -> None:
        expected = json.dumps({
            "ok": False,
            "command": "status",
            "error_code": "INVALID_ARGUMENT",
        })
        arguments = {"command": "status", "agent_id": 1}
        with mock.patch.object(
            protocol, "tool_subagent", return_value=expected,
        ) as dispatched:
            response = protocol._handle_tools_call({
                "name": "subagent",
                "arguments": arguments,
            })
        dispatched.assert_called_once_with(arguments)
        self.assertEqual(response["content"][0]["text"], expected)

    def test_removed_mode_keys_have_no_alias_or_entrypoint(self) -> None:
        local_schema = next(
            definition for definition in protocol.TOOL_DEFINITIONS
            if definition["name"] == "start_build"
        )["inputSchema"]
        remote_schema = next(
            definition for definition in protocol.TOOL_DEFINITIONS
            if definition["name"] == "remote-run"
        )["inputSchema"]
        remote_build_shape = next(
            shape for shape in remote_schema["oneOf"]
            if shape.get("title") == "build"
        )
        local_modes = set(local_schema["properties"]["mode"]["enum"])
        remote_modes = set(
            remote_build_shape["properties"]["mode"]["enum"]
        )
        self.assertTrue(REMOVED_MODE_KEYS.isdisjoint(build_presets.MODES))
        self.assertTrue(REMOVED_MODE_KEYS.isdisjoint(build_presets.VALID_MODES))
        self.assertTrue(REMOVED_MODE_KEYS.isdisjoint(local_modes))
        self.assertTrue(REMOVED_MODE_KEYS.isdisjoint(remote_modes))

        with mock.patch.object(
            build_worker, "_refuse_root_build_identity", return_value=None
        ), mock.patch.object(
            build_worker, "_ensure_build_worker_timesharing_policy"
        ), mock.patch.object(
            remote, "_current_local_branch", return_value="main"
        ):
            for mode in REMOVED_MODE_KEYS:
                with self.subTest(mode=mode):
                    self.assertIn(
                        "Invalid mode",
                        build_worker.tool_start_build(mode, build_only=True),
                    )
                    self.assertIn(
                        "Invalid mode",
                        remote.tool_remote_run({
                            "operation": "build",
                            "mode": mode,
                        }),
                    )

    def test_modes_pin_the_production_extension_compile_gate(self) -> None:
        for mode, (preset_name, profile) in PROFILE_MODES.items():
            with self.subTest(mode=mode):
                details = build_presets.MODES[mode]
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
                        "BIOSEQ_TEMPLATE_FLATTEN_PROFILE": profile,
                        "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
                        "BIOSEQ_BUILD_WEBSITE": "OFF",
                    },
                )
                self.assertEqual(build_presets.preset_for(mode, False), preset_name)
                self.assertEqual(build_presets.preset_for(mode, True), preset_name)
                self.assertEqual(
                    build_presets.build_dir_for(mode, False),
                    REPOSITORY_ROOT / "build" / preset_name,
                )

        runtime = build_presets.MODES[RUNTIME_PROFILE_MODE]
        ordinary = build_presets.MODES["unit-integration"]
        self.assertFalse(runtime.requires_build_only)
        self.assertEqual(runtime.targets, ordinary.targets)
        self.assertEqual(runtime.commands, ordinary.commands)
        self.assertIsNot(runtime.targets, ordinary.targets)
        self.assertIsNot(runtime.commands, ordinary.commands)
        self.assertTrue(runtime.build_targets_together)
        self.assertTrue(runtime.install_extension)
        self.assertEqual(runtime.lock_group, "tests")
        self.assertEqual(runtime.binary_subdir, ordinary.binary_subdir)
        self.assertEqual(runtime.preset_family, "test")
        self.assertEqual(
            runtime.required_cache_variables,
            {**ordinary.required_cache_variables,
             "BIOSEQ_TEMPLATE_FLATTEN_PROFILE": "all_alphabets_uint64"},
        )
        self.assertEqual(
            build_presets.preset_for(RUNTIME_PROFILE_MODE, False),
            "test-optimized-all-alphabets-uint64",
        )
        self.assertEqual(
            build_presets.build_dir_for(RUNTIME_PROFILE_MODE, False),
            REPOSITORY_ROOT / "build" / "test-optimized-all-alphabets-uint64",
        )
        self.assertEqual(
            build_worker._build_target_groups_for(RUNTIME_PROFILE_MODE),
            (tuple(ordinary.targets),),
        )

    def test_modes_are_local_only_and_documented_as_build_only(self) -> None:
        documentation = build_presets.get_modes_short_documentation()
        local_schema = next(
            definition for definition in protocol.TOOL_DEFINITIONS
            if definition["name"] == "start_build"
        )["inputSchema"]
        remote_schema = next(
            definition for definition in protocol.TOOL_DEFINITIONS
            if definition["name"] == "remote-run"
        )["inputSchema"]
        remote_build_shape = next(
            shape for shape in remote_schema["oneOf"]
            if shape.get("title") == "build"
        )

        for mode in PROFILE_MODES:
            with self.subTest(mode=mode):
                self.assertIn(mode, build_presets.FIXED_BUILD_ONLY_MODES)
                self.assertIn(mode, build_presets.REMOTE_RUN_REJECTED_MODES)
                self.assertNotIn(mode, build_presets.REMOTE_RUN_BUILD_MODES)
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
        self.assertNotIn(RUNTIME_PROFILE_MODE, build_presets.FIXED_BUILD_ONLY_MODES)
        self.assertIn(RUNTIME_PROFILE_MODE, build_presets.REMOTE_RUN_REJECTED_MODES)
        self.assertNotIn(RUNTIME_PROFILE_MODE, build_presets.REMOTE_RUN_BUILD_MODES)
        self.assertIn(RUNTIME_PROFILE_MODE, local_schema["properties"]["mode"]["enum"])
        self.assertNotIn(
            RUNTIME_PROFILE_MODE, remote_build_shape["properties"]["mode"]["enum"])
        self.assertIn(f"'{RUNTIME_PROFILE_MODE}'", documentation)
        runtime_guard = next(
            clause for clause in local_schema["allOf"]
            if clause.get("if", {}).get("properties", {}).get("mode", {}).get("const")
            == RUNTIME_PROFILE_MODE
        )
        self.assertEqual(runtime_guard["then"]["properties"]["debug"], {"const": False})
        self.assertIn("local build-only only", documentation)
        self.assertEqual(
            local_schema["properties"]["build_timeout_sec"]["maximum"],
            3300,
        )
        for field in (
            "fresh",
            "build_timeout_sec",
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
                build_presets.ModeDefinition(**(base | override))


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
        )
        with mock.patch.object(
            build_worker,
            "_refuse_root_build_identity",
            return_value=None,
        ), mock.patch.object(
            build_worker,
            "_ensure_build_worker_timesharing_policy",
        ):
            for mode in PROFILE_MODES:
                for request in invalid_requests:
                    with self.subTest(mode=mode, request=request):
                        message = build_worker.tool_start_build(mode, **request)
                        self.assertIn("fixed local compile gate", message)
                        self.assertIn("requires build_only=true", message)
            self.assertIn(
                "rejects debug=true",
                build_worker.tool_start_build(
                    RUNTIME_PROFILE_MODE, build_only=True, debug=True),
            )

    def test_profiles_reject_baseline_and_receipt_runtime_controls(self) -> None:
        invalid_requests = (
            {"build_only": True, "run_only": True},
            {
                "build_only": True,
                "reuse_build_receipt": "br_invalid",
            },
        )
        with mock.patch.object(
            build_worker,
            "_refuse_root_build_identity",
            return_value=None,
        ), mock.patch.object(
            build_worker,
            "_ensure_build_worker_timesharing_policy",
        ):
            for mode in PROFILE_MODES:
                for request in invalid_requests:
                    with self.subTest(mode=mode, request=request):
                        message = build_worker.tool_start_build(mode, **request)
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
        )
        with mock.patch.object(
            build_worker,
            "_refuse_root_build_identity",
            return_value=None,
        ), mock.patch.object(
            build_worker,
            "_ensure_build_worker_timesharing_policy",
        ), mock.patch.object(
            build_worker,
            "_load_state",
            side_effect=lambda: {
                mode: {"reserved": True}
                for mode in (*PROFILE_MODES, RUNTIME_PROFILE_MODE)
            },
        ), mock.patch.object(
            build_worker, "_stream_subprocess",
        ) as run, mock.patch.object(build_worker, "_log") as log:
            for mode in PROFILE_MODES:
                for request in invalid_requests:
                    with self.subTest(mode=mode, request=request):
                        self.assertEqual(
                            build_worker._run_build_worker(mode, **request),
                            1,
                        )
            log.reset_mock()
            self.assertEqual(
                build_worker._run_build_worker(
                    RUNTIME_PROFILE_MODE, build_only=True, debug=True),
                1,
            )
            log.assert_called_once_with(
                f"{RUNTIME_PROFILE_MODE} uses a fixed optimized preset and rejects debug=true")
            run.assert_not_called()

    def test_synchronous_worker_fails_before_reserving_state(self) -> None:
        invalid_requests = (
            {"build_only": False},
            {"build_only": True, "debug": True},
            {"build_only": True, "bench_flags": ["--debug"]},
            {"build_only": True, "test_flags": ["[index]"]},
            {"build_only": True, "bench_e2e_config": "config.json"},
        )
        with mock.patch.object(
            build_worker,
            "_refuse_root_build_identity",
            return_value=None,
        ), mock.patch.object(
            build_worker,
            "_ensure_build_worker_timesharing_policy",
        ), mock.patch.object(process_state, "_load_state") as load_state:
            for mode in sorted(build_presets.FIXED_BUILD_ONLY_MODES):
                for request in invalid_requests:
                    with self.subTest(mode=mode, request=request):
                        self.assertEqual(
                            build_worker._run_build_inline(mode, **request),
                            2,
                        )
            self.assertEqual(
                build_worker._run_build_inline(
                    RUNTIME_PROFILE_MODE, build_only=True, debug=True),
                2,
            )
            load_state.assert_not_called()


if __name__ == "__main__":
    unittest.main()
