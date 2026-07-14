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


if __name__ == "__main__":
    unittest.main()
