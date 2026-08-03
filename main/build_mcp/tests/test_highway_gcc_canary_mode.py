from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path


BUILD_MCP_DIR = Path(__file__).resolve().parents[1]
MAIN_DIR = BUILD_MCP_DIR.parent
sys.path.insert(0, str(BUILD_MCP_DIR))

import server  # noqa: E402


MODE = "highway-gcc-canary"
NESTED_BINARY_SUBDIR = (
    "codegen/namespace_materialization/tests/module_canary/"
    "highway-gcc-canary/nested/bin"
)
HIGHWAY_CANARY_MAIN = (
    "codegen/namespace_materialization/tests/module_canary/"
    "highway_canary/main.cpp"
)
EXPECTED_CACHE = {
    "BIOSEQ_HIGHWAY_GCC_CANARY": "ON",
    "BIOSEQ_BUILD_CORE": "ON",
    "BIOSEQ_BUILD_CACHE": "OFF",
    "BIOSEQ_BUILD_PAPERS": "OFF",
    "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
    "BIOSEQ_BUILD_WEBSITE": "OFF",
    "BUILD_TESTING": "OFF",
}


class HighwayGccCanaryPresetContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        payload = json.loads((MAIN_DIR / "CMakePresets.json").read_text())
        cls.preset = next(
            preset for preset in payload["configurePresets"]
            if preset["name"] == MODE
        )

    def test_preset_is_an_isolated_clang_host_tree(self) -> None:
        self.assertEqual(self.preset["inherits"], "bench-opt")
        self.assertEqual(
            self.preset["binaryDir"],
            "${sourceDir}/build/${presetName}",
        )
        self.assertEqual(
            self.preset["environment"],
            {"CMAKE_NAMED_ENV_TYPE": "H"},
        )
        self.assertEqual(self.preset["cacheVariables"], EXPECTED_CACHE)
        self.assertIn("no-vcpkg", self.preset["description"])
        self.assertIn("build_only=false", self.preset["description"])
        self.assertIn("selected target IDs", self.preset["description"])
        self.assertIn("not the full external-api", self.preset["description"])


class HighwayGccCanaryModeContractTest(unittest.TestCase):
    def test_mode_owns_compile_only_and_runtime_receipts(self) -> None:
        details = server.MODES[MODE]
        self.assertFalse(details.requires_build_only)
        self.assertEqual(details.lock_group, "tests")
        self.assertEqual(details.preset_override, MODE)
        self.assertEqual(details.targets, ["bioseq_highway_gcc_canary"])
        self.assertEqual(
            details.commands,
            [["bioseq_highway_gcc_module_canary"]],
        )
        self.assertEqual(details.binary_subdir, NESTED_BINARY_SUBDIR)
        self.assertEqual(details.post_build_targets, ())
        self.assertFalse(details.install_extension)
        self.assertEqual(details.required_cache_variables, EXPECTED_CACHE)
        self.assertEqual(server.preset_for(MODE, False), MODE)
        self.assertEqual(server.preset_for(MODE, True), MODE)
        self.assertEqual(
            server.build_dir_for(MODE, False),
            MAIN_DIR / "build" / MODE,
        )
        self.assertEqual(
            server.bin_dir_for(MODE, False),
            MAIN_DIR / "build" / MODE / NESTED_BINARY_SUBDIR,
        )
        self.assertNotIn(MODE, server.FIXED_BUILD_ONLY_MODES)
        self.assertIn(MODE, server.REMOTE_RUN_REJECTED_MODES)
        self.assertNotIn(MODE, server.REMOTE_RUN_BUILD_MODES)

    def test_mode_documents_its_narrow_evidence_boundary(self) -> None:
        docs = server.MODES[MODE].docs
        self.assertIn("paired Homebrew gcc/g++ >= 16.1", docs)
        self.assertIn("libstdc++.modules.json", docs)
        self.assertIn("sole HWY_EXPORT/HWY_DYNAMIC_DISPATCH", docs)
        self.assertIn("third compile-only attributed target probe", docs)
        self.assertIn("sealed Highway/libstdc++ headers", docs)
        self.assertIn("audited GCC dyld runtimes", docs)
        self.assertIn("build_only=true", docs)
        self.assertIn("build_only=false", docs)
        self.assertIn("executes neither binary", docs)
        self.assertIn("executes the imported-binder canary", docs)
        self.assertIn("both selected target IDs and names", docs)
        self.assertIn(
            "GCC Highway module/attribute/dispatch boundary",
            docs,
        )
        self.assertIn("Neither form proves the full external-api", docs)

    def test_runtime_output_records_numeric_target_ids(self) -> None:
        source = (MAIN_DIR / HIGHWAY_CANARY_MAIN).read_text()
        self.assertIn("first=%s(0x%llx)", source)
        self.assertIn("second=%s(0x%llx)", source)
        self.assertIn(
            "static_cast<unsigned long long>(result.first_target)",
            source,
        )
        self.assertIn(
            "static_cast<unsigned long long>(result.second_target)",
            source,
        )

    def test_local_schema_includes_mode_but_remote_schema_does_not(self) -> None:
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
        self.assertIn(MODE, local_schema["properties"]["mode"]["enum"])
        self.assertNotIn(
            MODE,
            remote_build_shape["properties"]["mode"]["enum"],
        )


if __name__ == "__main__":
    unittest.main()
