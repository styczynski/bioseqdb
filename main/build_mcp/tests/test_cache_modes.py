from __future__ import annotations

import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


BUILD_MCP_DIR = Path(__file__).resolve().parents[1]
MAIN_DIR = BUILD_MCP_DIR.parent
sys.path.insert(0, str(BUILD_MCP_DIR))

import inspect_tool  # noqa: E402
import server  # noqa: E402


LOCAL = ("bench-opt", "bench-opt-debug")
AWS_CLANG = ("aws-x64-linux", "aws-x64-linux-debug")
AWS_GCC = ("aws-x64-linux-gcc161", "aws-x64-linux-gcc161-debug")


class CachePresetContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        payload = json.loads((MAIN_DIR / "CMakePresets.json").read_text())
        cls.presets = {
            preset["name"]: preset
            for preset in payload["configurePresets"]
        }

    def test_inherited_cache_presets_preserve_toolchain_families(self) -> None:
        expected = {
            "cache-opt": ("bench-opt", False, False),
            "cache-opt-debug": ("bench-opt-debug", False, False),
            "cache-bioseq-opt": ("bench-opt", True, False),
            "cache-bioseq-opt-debug": ("bench-opt-debug", True, False),
            "aws-x64-linux-cache": ("aws-x64-linux", False, True),
            "aws-x64-linux-cache-debug": (
                "aws-x64-linux-debug", False, True),
            "aws-x64-linux-cache-bioseq": (
                "aws-x64-linux", True, True),
            "aws-x64-linux-cache-bioseq-debug": (
                "aws-x64-linux-debug", True, True),
            "aws-x64-linux-gcc161-cache": (
                "aws-x64-linux-gcc161", False, True),
            "aws-x64-linux-gcc161-cache-debug": (
                "aws-x64-linux-gcc161-debug", False, True),
            "aws-x64-linux-gcc161-cache-bioseq": (
                "aws-x64-linux-gcc161", True, True),
            "aws-x64-linux-gcc161-cache-bioseq-debug": (
                "aws-x64-linux-gcc161-debug", True, True),
        }

        for name, (parent, combined, _aws) in expected.items():
            with self.subTest(name=name):
                preset = self.presets[name]
                variables = preset["cacheVariables"]
                self.assertEqual(preset["inherits"], parent)
                self.assertEqual(
                    preset["binaryDir"], "${sourceDir}/build/${presetName}")
                self.assertEqual(
                    variables["BIOSEQ_BUILD_CORE"],
                    "ON" if combined else "OFF",
                )
                self.assertEqual(variables["BIOSEQ_BUILD_CACHE"], "ON")
                self.assertEqual(variables["BIOSEQ_BUILD_PAPERS"], "OFF")
                self.assertEqual(variables["BIOSEQ_CACHE_BUILD_TESTS"], "ON")
                self.assertEqual(variables["BIOSEQ_CACHE_TESTING"], "ON")
                if combined:
                    self.assertNotIn("VCPKG_MANIFEST_DIR", variables)
                else:
                    self.assertEqual(
                        variables["VCPKG_MANIFEST_DIR"],
                        "${sourceDir}/bioseq_caching",
                    )


class CacheModeResolutionTest(unittest.TestCase):
    def _patch_toolchain(self, fast: str, debug: str):
        return mock.patch.multiple(
            server,
            DEFAULT_BUILD_PRESET=fast,
            DEBUG_BUILD_PRESET=debug,
            TEST_BUILD_PRESET=server._default_test_preset(fast),
            TEST_DEBUG_BUILD_PRESET=server._default_test_preset(debug),
            CACHE_BUILD_PRESET_OVERRIDE=None,
            CACHE_DEBUG_BUILD_PRESET_OVERRIDE=None,
            CACHE_BIOSEQ_BUILD_PRESET_OVERRIDE=None,
            CACHE_BIOSEQ_DEBUG_BUILD_PRESET_OVERRIDE=None,
        )

    def test_all_modes_resolve_for_local_aws_clang_and_aws_gcc(self) -> None:
        for fast, debug_preset in (LOCAL, AWS_CLANG, AWS_GCC):
            with self.subTest(toolchain=fast), self._patch_toolchain(
                    fast, debug_preset):
                for mode, details in server.MODES.items():
                    for debug in (False, True):
                        with self.subTest(mode=mode, debug=debug):
                            core_preset = debug_preset if debug else fast
                            if details.preset_override:
                                expected = details.preset_override
                            elif details.preset_family == "core":
                                expected = core_preset
                            elif details.preset_family == "test":
                                expected = server._default_test_preset(core_preset)
                            elif details.preset_family == "cache":
                                expected = server._default_cache_preset(
                                    core_preset, combined=False)
                            elif details.preset_family == "cache-bioseq":
                                expected = server._default_cache_preset(
                                    core_preset, combined=True)
                            elif details.preset_family == "website":
                                expected = (
                                    server.WEBSITE_DEBUG_BUILD_PRESET
                                    if debug else server.WEBSITE_BUILD_PRESET)
                            else:
                                self.fail(
                                    "unhandled preset family "
                                    f"{details.preset_family!r}")

                            self.assertEqual(
                                server.preset_for(mode, debug), expected)
                            self.assertEqual(
                                server.build_dir_for(mode, debug),
                                server.PROJECT_ROOT / "build" / expected,
                            )
                            self.assertEqual(
                                server.bin_dir_for(mode, debug),
                                server.PROJECT_ROOT / "build" / expected
                                / details.binary_subdir,
                            )

    def test_cache_specific_environment_overrides_win(self) -> None:
        with mock.patch.multiple(
            server,
            CACHE_BUILD_PRESET_OVERRIDE="custom-cache-fast",
            CACHE_DEBUG_BUILD_PRESET_OVERRIDE="custom-cache-debug",
            CACHE_BIOSEQ_BUILD_PRESET_OVERRIDE="custom-combined-fast",
            CACHE_BIOSEQ_DEBUG_BUILD_PRESET_OVERRIDE="custom-combined-debug",
        ):
            self.assertEqual(
                server.preset_for("cache-unit", False), "custom-cache-fast")
            self.assertEqual(
                server.preset_for("cache-unit", True), "custom-cache-debug")
            self.assertEqual(
                server.preset_for("cache-bioseq-integration", False),
                "custom-combined-fast",
            )
            self.assertEqual(
                server.preset_for("cache-bioseq-integration", True),
                "custom-combined-debug",
            )

    def test_custom_preset_fallback_keeps_debug_suffix_last(self) -> None:
        self.assertEqual(
            server._default_cache_preset("toolchain-debug", combined=False),
            "toolchain-cache-debug",
        )
        self.assertEqual(
            server._default_cache_preset("toolchain-debug", combined=True),
            "toolchain-cache-bioseq-debug",
        )


class CacheModeRoutingTest(unittest.TestCase):
    def test_legacy_python_benchmark_inventory_is_complete_and_checked(
            self) -> None:
        inventory_path = (
            MAIN_DIR
            / "bioseq_caching"
            / "bench"
            / "legacy_python_behavior_inventory.tsv"
        )
        with inventory_path.open(encoding="utf-8", newline="") as stream:
            rows = list(csv.DictReader(stream, delimiter="\t"))
        self.assertGreaterEqual(len(rows), 100)
        self.assertEqual(
            set(rows[0]),
            {
                "legacy_asset_sha256",
                "legacy_symbol",
                "legacy_behavior",
                "disposition",
                "native_evidence",
            },
        )
        expected_hashes = {
            "b7f8378f3f3cdfa4a0ef88b20ba34273587f1a7ddced698ce7ef9cc8c19e659b",
            "2f255c8f83d7782d3c1aaf8d7d4025eeb8b5c35ff1b4ce50b855b0b9b3627585",
            "1d3045a0b41a5b42f851a3db9fe0f7518aef2f8d6325da648a92a096e714b472",
        }
        self.assertEqual(
            {row["legacy_asset_sha256"] for row in rows},
            expected_hashes,
        )
        identities = [
            (row["legacy_asset_sha256"], row["legacy_symbol"])
            for row in rows
        ]
        self.assertEqual(len(identities), len(set(identities)))
        for row in rows:
            with self.subTest(symbol=row["legacy_symbol"]):
                self.assertTrue(row["legacy_behavior"])
                self.assertRegex(
                    row["disposition"],
                    r"^(native|shared|retired)_[a-z0-9_]+$",
                )
                evidence_paths = row["native_evidence"].split(";")
                self.assertTrue(evidence_paths)
                for relative_path in evidence_paths:
                    self.assertTrue(
                        (MAIN_DIR.parent / relative_path).is_file(),
                        relative_path,
                    )

    def test_native_benchmark_declares_v1_report_and_2gbp_contract(
            self) -> None:
        source = (
            MAIN_DIR
            / "bioseq_caching"
            / "bench"
            / "cache_benchmark.cppm"
        ).read_text(encoding="utf-8")
        for required in (
            "bioseq-cache-benchmark-v1",
            "metadata",
            "scenario",
            "fixture",
            "correctness_digest",
            "samples",
            "summary",
            "p50",
            "p95",
            "cache_counter_deltas",
            "logical_bytes",
            "stored_bytes",
            "peak_rss_bytes",
            "peak_temp_bytes",
            "2'000'000'000ULL",
            "PQsetSingleRowMode first PGRES_SINGLE_TUPLE arrival",
        ):
            with self.subTest(required=required):
                self.assertIn(required, source)

    def test_cache_cmake_generates_artifact_manifest_from_target_properties(
            self) -> None:
        cmake = (
            MAIN_DIR
            / "bioseq_caching"
            / "bench"
            / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        self.assertIn("cache_native_artifacts.json", cmake)
        self.assertIn("BIOSEQ_EXTENSION_CONTROL_FILE", cmake)
        self.assertIn("BIOSEQ_EXTENSION_INSTALL_SQL", cmake)
        self.assertIn("BIOSEQ_EXTENSION_DSO", cmake)
        self.assertNotIn("run_private_cluster_bench.py", cmake)

    def test_cache_runtime_modes_have_no_legacy_post_build_targets(
            self) -> None:
        for mode in (
            "cache-integration",
            "cache-bench",
            "cache-bioseq-integration",
            "cache-bioseq-bench",
        ):
            with self.subTest(mode=mode):
                self.assertEqual(server.post_build_targets_for(mode, False), ())
                self.assertEqual(server.post_build_targets_for(mode, True), ())

    def test_existing_install_modes_remain_unchanged(self) -> None:
        install_modes = {
            name for name, details in server.MODES.items()
            if details.install_extension
        }
        self.assertEqual(
            install_modes, {"unit+integration", "bench-e2e"})
        for name in install_modes:
            with self.subTest(mode=name):
                self.assertIn(
                    "postgresql_bioseq_cache", server.MODES[name].targets)
                self.assertIn("postgresql_bioseq", server.MODES[name].targets)
        for name, details in server.MODES.items():
            if name.startswith("cache-"):
                self.assertFalse(details.install_extension, name)
                if details.uses_private_cluster:
                    self.assertTrue(details.commands, name)

    def test_private_cluster_mode_cannot_install_shared_extension(self) -> None:
        with self.assertRaisesRegex(ValueError, "private-cluster modes"):
            server.ModeDefinition(
                lock_group="invalid",
                expected_duration_min=1.0,
                build_only_expected_duration_min=1.0,
                label="invalid",
                is_verb_plular=False,
                targets=[],
                commands=[],
                docs="invalid",
                install_extension=True,
                uses_private_cluster=True,
            )

    def test_native_cache_bench_rejects_unimplemented_cli_filters(self) -> None:
        result = server.tool_start_build(
            "cache-bench", bench_filter={"tags": ["cache"]})
        self.assertIn("require a benchmark CLI", result)
        self.assertIn("cache-bench", result)

    def test_native_cache_mode_commands_and_timeouts(self) -> None:
        expected = {
            "cache-integration": (
                "bioseq_cache_integration_tests",
                8.0,
                "480",
                None,
            ),
            "cache-bench": (
                "bioseq_cache_bench",
                20.0,
                None,
                "standalone",
            ),
            "cache-bioseq-integration": (
                "bioseq_cache_bioseq_integration_tests",
                120.0,
                "6300",
                None,
            ),
            "cache-bioseq-bench": (
                "bioseq_cache_bioseq_bench",
                180.0,
                None,
                "bioseq-2gbp",
            ),
        }
        for mode, (executable, timeout, case_timeout, scenario) in (
                expected.items()):
            with self.subTest(mode=mode):
                details = server.MODES[mode]
                self.assertEqual(details.expected_duration_min, timeout)
                self.assertEqual(len(details.commands), 1)
                command = details.commands[0]
                self.assertEqual(command[0], executable)
                self.assertIn("--postgres-bin-dir", command)
                self.assertIn("{postgres_bin_dir}", command)
                self.assertIn("--cache-control", command)
                self.assertIn("--cache-install-sql", command)
                self.assertIn("--cache-dso", command)
                self.assertIn("--report", command)
                self.assertIn("{report}", command)
                self.assertTrue(details.reporting_subdir_name)
                if case_timeout is not None:
                    timeout_index = command.index(
                        "--case-timeout-seconds")
                    self.assertEqual(command[timeout_index + 1], case_timeout)
                if scenario is not None:
                    scenario_index = command.index("--scenario")
                    self.assertEqual(command[scenario_index + 1], scenario)
                    self.assertIn("--samples", command)
                    self.assertIn("--warmup-samples", command)
                if "bioseq" in mode:
                    self.assertIn("--bioseq-control", command)
                    self.assertIn("--bioseq-install-sql", command)
                    self.assertIn("--bioseq-dso", command)

    def test_cache_modes_contain_no_script_runner_invocations(self) -> None:
        forbidden = (".py", ".pl", ".pm", "python", "perl")
        for mode, details in server.MODES.items():
            if not mode.startswith("cache-"):
                continue
            for command in details.commands:
                rendered = " ".join(command).lower()
                with self.subTest(mode=mode, command=rendered):
                    self.assertFalse(
                        any(token in rendered for token in forbidden))

    def test_native_artifact_placeholders_resolve_from_generated_manifest(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            binary_dir = Path(tmp)
            manifest = {
                key: str(binary_dir / key)
                for key in server._CACHE_NATIVE_ARTIFACT_KEYS
            }
            for key, value in manifest.items():
                path = Path(value)
                if key == "postgres_bin_dir":
                    path.mkdir()
                else:
                    path.touch()
            (binary_dir / "cache_native_artifacts.json").write_text(
                json.dumps(manifest), encoding="utf-8")
            report = binary_dir / "reports" / "report_0.json"
            command = server.MODES["cache-bioseq-bench"].commands[0]
            resolved = server._run_command_argv(
                "cache-bioseq-bench",
                binary_dir,
                command,
                report_path=report,
            )
        self.assertEqual(
            resolved[0], str(binary_dir / "bioseq_cache_bioseq_bench"))
        self.assertIn(str(binary_dir / "postgres_bin_dir"), resolved)
        self.assertIn(str(binary_dir / "cache_control"), resolved)
        self.assertIn(str(binary_dir / "bioseq_dso"), resolved)
        self.assertIn(str(report), resolved)
        self.assertNotIn("{report}", resolved)

    def test_native_artifact_resolution_rejects_missing_combined_path(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            binary_dir = Path(tmp)
            manifest = {
                key: str(binary_dir / key)
                for key in server._CACHE_NATIVE_ARTIFACT_KEYS
            }
            manifest["bioseq_dso"] = ""
            for key, value in manifest.items():
                if not value:
                    continue
                path = Path(value)
                if key == "postgres_bin_dir":
                    path.mkdir()
                else:
                    path.touch()
            (binary_dir / "cache_native_artifacts.json").write_text(
                json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(
                    RuntimeError, "bioseq_dso.*unavailable"):
                server._run_command_argv(
                    "cache-bioseq-bench",
                    binary_dir,
                    server.MODES["cache-bioseq-bench"].commands[0],
                    report_path=binary_dir / "report_0.json",
                )

    def test_report_path_allocator_preserves_prior_native_reports(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            reporting_dir = Path(tmp)
            (reporting_dir / "report_0.json").write_text(
                "{}", encoding="utf-8")
            (reporting_dir / "report_debug_9.jsonl").write_text(
                "{}", encoding="utf-8")
            self.assertEqual(
                server._next_report_path(reporting_dir),
                reporting_dir / "report_1.json",
            )

    def test_mcp_schemas_expose_cache_modes_and_inspect_targets(self) -> None:
        definitions = {
            definition["name"]: definition
            for definition in server.TOOL_DEFINITIONS
        }
        modes = definitions["start_build"]["inputSchema"]["properties"][
            "mode"]["enum"]
        inspect_targets = definitions["inspect"]["inputSchema"][
            "properties"]["target"]["enum"]
        remote_shapes = definitions["remote-run"]["inputSchema"]["oneOf"]
        remote_modes = remote_shapes[0]["properties"]["mode"]["enum"]
        remote_targets = remote_shapes[1]["properties"]["target"]["enum"]
        for mode in (
            "cache-unit",
            "cache-integration",
            "cache-bench",
            "cache-bioseq-integration",
            "cache-bioseq-bench",
        ):
            self.assertIn(mode, modes)
            self.assertIn(mode, remote_modes)
        self.assertEqual(inspect_targets, sorted(server.INSPECT_TARGETS))
        self.assertEqual(remote_targets, sorted(server.INSPECT_TARGETS))

    def test_inspect_missing_artifact_uses_cache_subdirectory_and_candidates(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            result = inspect_tool.run_inspect(
                function_name="bioseq_cache_version",
                target="postgresql_bioseq_cache",
                build_dir=Path(tmp),
                binary_subdir="bioseq_caching",
                artifact_names=("bioseq_cache.dylib", "bioseq_cache.so"),
                shared_target_names=("bioseq_cache_core",),
            )
        self.assertIn("error", result)
        self.assertIn("bioseq_caching/bioseq_cache.dylib", result["error"])
        self.assertIn("bioseq_caching/bioseq_cache.so", result["error"])
        self.assertEqual(result["binary_subdir"], "bioseq_caching")

    def test_postgresql_bioseq_inspect_mapping_uses_bench_e2e_dso_candidates(
            self) -> None:
        definition = server.INSPECT_TARGETS["postgresql_bioseq"]
        self.assertEqual(definition.mode, "bench-e2e")
        self.assertEqual(
            definition.artifact_names,
            ("bioseq.dylib", "bioseq.so", "bioseq.dll"),
        )
        self.assertEqual(
            definition.shared_target_names,
            ("bioseq_modules_nopg",),
        )

        with tempfile.TemporaryDirectory() as tmp:
            result = inspect_tool.run_inspect(
                function_name="PreparedShapeV5FragmentQueryReader",
                target="postgresql_bioseq",
                build_dir=Path(tmp),
                binary_subdir="",
                artifact_names=definition.artifact_names,
                shared_target_names=definition.shared_target_names,
            )
        self.assertIn("error", result)
        self.assertIn("bioseq.dylib", result["error"])
        self.assertIn("bioseq.so", result["error"])
        self.assertIn("bioseq.dll", result["error"])


if __name__ == "__main__":
    unittest.main()
