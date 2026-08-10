from __future__ import annotations

import sys
import unittest
from pathlib import Path


BUILD_MCP_DIR = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = BUILD_MCP_DIR.parent
sys.path.insert(0, str(BUILD_MCP_DIR))

import server  # noqa: E402


class ExternalPostgresAcceptanceModeTest(unittest.TestCase):
    def test_isolated_codegen_disables_dependency_ctest_only(self) -> None:
        external_project = (
            REPOSITORY_ROOT / "cmake" / "BioseqCodegen.cmake"
        ).read_text(encoding="utf-8")
        generator_project = (
            REPOSITORY_ROOT / "codegen" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")

        self.assertIn(
            '"-DBUILD_TESTING:BOOL=OFF"',
            external_project,
        )
        self.assertIn(
            'COMMAND "$<TARGET_FILE:bioseq_codegen>" self_test sql',
            generator_project,
        )
        self.assertIn(
            'COMMAND "$<TARGET_FILE:bioseq_codegen>" self_test safety',
            generator_project,
        )

    def test_external_preset_defaults_to_gcc_on_linux(self) -> None:
        self.assertEqual(
            server._external_api_build_preset({}, "linux", "bench-opt"),
            "aws-x64-linux-gcc161",
        )
        self.assertEqual(
            server._external_api_build_preset({}, "linux2", "bench-opt"),
            "aws-x64-linux-gcc161",
        )

    def test_external_preset_defaults_to_core_preset_off_linux(self) -> None:
        self.assertEqual(
            server._external_api_build_preset({}, "darwin", "bench-opt"),
            "bench-opt",
        )
        self.assertEqual(
            server._external_api_build_preset({}, "win32", "bench-opt"),
            "bench-opt",
        )

    def test_external_preset_override_wins_on_every_host(self) -> None:
        environment = {
            "BIOSEQ_EXTERNAL_API_BUILD_PRESET": "custom-external-preset",
        }
        for platform in ("linux", "darwin", "win32"):
            with self.subTest(platform=platform):
                self.assertEqual(
                    server._external_api_build_preset(
                        environment,
                        platform,
                        "bench-opt",
                    ),
                    "custom-external-preset",
                )

    def test_full_mode_builds_package_and_runs_private_acceptance(self) -> None:
        details = server.MODES["external-api"]
        self.assertIn("bioseq_external_integration_tests_exec", details.targets)
        self.assertIn("postgresql_bioseq", details.targets)
        self.assertEqual(
            details.post_build_targets,
            ("bioseq_external_postgres_acceptance",),
        )
        self.assertTrue(details.uses_private_cluster)
        self.assertFalse(details.install_extension)

    def test_build_only_never_starts_private_postgres(self) -> None:
        self.assertEqual(
            server.post_build_targets_for("external-api", True),
            (),
        )
        self.assertEqual(
            server.post_build_targets_for("external-api", False),
            ("bioseq_external_postgres_acceptance",),
        )

    def test_external_feature_gate_remains_fail_closed(self) -> None:
        required = server.MODES["external-api"].required_cache_variables
        self.assertEqual(
            required["BIOSEQ_BUILD_EXTERNAL_API_CLIENTS"],
            "ON",
        )
        self.assertEqual(required["BIOSEQ_BUILD_WEBSITE"], "OFF")


if __name__ == "__main__":
    unittest.main()
