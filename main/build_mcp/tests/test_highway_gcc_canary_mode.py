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


MODE = "highway-gcc-canary"
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
        self.assertIn("not the full external-api", self.preset["description"])


class HighwayGccCanaryModeContractTest(unittest.TestCase):
    def test_mode_is_fixed_local_build_only(self) -> None:
        details = server.MODES[MODE]
        self.assertTrue(details.requires_build_only)
        self.assertEqual(details.lock_group, "tests")
        self.assertEqual(details.preset_override, MODE)
        self.assertEqual(details.targets, ["bioseq_highway_gcc_canary"])
        self.assertEqual(details.commands, [])
        self.assertEqual(details.post_build_targets, ())
        self.assertFalse(details.install_extension)
        self.assertEqual(details.required_cache_variables, EXPECTED_CACHE)
        self.assertEqual(server.preset_for(MODE, False), MODE)
        self.assertEqual(server.preset_for(MODE, True), MODE)
        self.assertEqual(
            server.build_dir_for(MODE, False),
            MAIN_DIR / "build" / MODE,
        )
        self.assertIn(MODE, server.FIXED_BUILD_ONLY_MODES)
        self.assertIn(MODE, server.REMOTE_RUN_REJECTED_MODES)
        self.assertNotIn(MODE, server.REMOTE_RUN_BUILD_MODES)

    def test_mode_documents_its_narrow_evidence_boundary(self) -> None:
        docs = server.MODES[MODE].docs
        self.assertIn("paired Homebrew gcc/g++ >= 16.1", docs)
        self.assertIn("libstdc++.modules.json", docs)
        self.assertIn("only the GCC module/attribute/generated-IU boundary", docs)
        self.assertIn("not the full external-api build or production DSO", docs)

    def test_public_entry_rejects_runtime_and_debug_forms(self) -> None:
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
            server,
            "_refuse_root_build_identity",
            return_value=None,
        ), mock.patch.object(
            server,
            "_ensure_build_worker_timesharing_policy",
        ):
            for request in invalid_requests:
                with self.subTest(request=request):
                    message = server.tool_start_build(MODE, **request)
                    self.assertIn("fixed local compile gate", message)
                    self.assertIn("requires build_only=true", message)

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
