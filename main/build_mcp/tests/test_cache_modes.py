from __future__ import annotations

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
                            else:
                                expected = server._default_cache_preset(
                                    core_preset, combined=True)

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
    def test_build_only_suppresses_every_post_build_target(self) -> None:
        expected = {
            "cache-integration": (
                "test_bioseq_cache_regress",
                "test_bioseq_cache_isolation",
                "test_bioseq_cache_tap",
            ),
            "cache-bench": ("run_bioseq_cache_bench",),
            "cache-bioseq-integration": (
                "test_bioseq_cache_bioseq_private",),
            "cache-bioseq-bench": (
                "run_bioseq_cache_bioseq_bench",),
        }
        for mode, targets in expected.items():
            with self.subTest(mode=mode):
                self.assertEqual(
                    server.post_build_targets_for(mode, False), targets)
                self.assertEqual(server.post_build_targets_for(mode, True), ())

    def test_existing_install_modes_remain_unchanged(self) -> None:
        install_modes = {
            name for name, details in server.MODES.items()
            if details.install_extension
        }
        self.assertEqual(
            install_modes, {"unit+integration", "bench-e2e"})
        for name, details in server.MODES.items():
            if name.startswith("cache-"):
                self.assertFalse(details.install_extension, name)
                if details.uses_private_cluster:
                    self.assertTrue(details.post_build_targets, name)

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

    def test_cache_post_target_bench_rejects_cli_filters(self) -> None:
        result = server.tool_start_build(
            "cache-bench", bench_filter={"tags": ["cache"]})
        self.assertIn("require a benchmark CLI", result)
        self.assertIn("cache-bench", result)

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


if __name__ == "__main__":
    unittest.main()
