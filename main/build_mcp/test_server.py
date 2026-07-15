"""Focused unit tests for the safe local bench-e2e config override."""

from pathlib import Path
import unittest

import server


class BenchE2eConfigOverrideTests(unittest.TestCase):
    def test_accepts_workspace_relative_json_and_returns_canonical_path(self):
        resolved, error = server._validate_bench_e2e_config_override(
            "bench-e2e", "main/bench/bench_e2e_configuration.json")

        self.assertIsNone(error)
        self.assertEqual(
            resolved,
            str((server.WORKSPACE_ROOT
                 / "main/bench/bench_e2e_configuration.json").resolve()),
        )

    def test_rejects_override_for_every_other_mode(self):
        resolved, error = server._validate_bench_e2e_config_override(
            "bench", "main/bench/bench_e2e_configuration.json")

        self.assertIsNone(resolved)
        self.assertIn("only for mode='bench-e2e'", error)

    def test_rejects_existing_json_outside_workspace(self):
        outside_json = Path("/tmp/bioseq-build-e2e-outside.json")
        outside_json.write_text("{}", encoding="utf-8")
        self.addCleanup(outside_json.unlink, missing_ok=True)

        resolved, error = server._validate_bench_e2e_config_override(
            "bench-e2e", str(outside_json))

        self.assertIsNone(resolved)
        self.assertIn("inside the workspace", error)

    def test_rejects_missing_and_non_json_paths(self):
        missing, missing_error = server._validate_bench_e2e_config_override(
            "bench-e2e", "main/bench/does-not-exist.json")
        non_json, non_json_error = server._validate_bench_e2e_config_override(
            "bench-e2e", "main/build_mcp/server.py")

        self.assertIsNone(missing)
        self.assertIn("existing file", missing_error)
        self.assertIsNone(non_json)
        self.assertIn("must name a .json file", non_json_error)


class UnitIntegrationQualificationTests(unittest.TestCase):
    def test_builder_builds_and_runs_e2e_serializer_contract(self):
        mode = server.MODES["unit+integration"]

        self.assertIn("bioseq_bench_e2e_tests_exec", mode.targets)
        self.assertIn(
            "bioseq_bench_e2e_tests_exec",
            [command[0] for command in mode.commands],
        )

    def test_focused_filter_allows_zero_matches_in_every_binary(self):
        test_flags, error = server._unit_test_filter_to_cli_args(
            {"tags": ["[format5]"]})

        self.assertIsNone(error)
        self.assertEqual(
            test_flags,
            ["--allow-running-no-tests", "[format5]"],
        )
        binary_dir = Path("/tmp/test-opt/bioseq")
        resolved_commands = [
            server._run_command_argv(
                "unit+integration",
                binary_dir,
                command,
                test_flags=test_flags,
            )
            for command in server.MODES["unit+integration"].commands
        ]
        self.assertEqual(len(resolved_commands), 3)
        for command in resolved_commands:
            self.assertEqual(command[-2:], test_flags)


if __name__ == "__main__":
    unittest.main()
