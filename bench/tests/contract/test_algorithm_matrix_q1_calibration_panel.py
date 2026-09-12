#!/usr/bin/env python3
"""Focused contract tests for the sealed five-cell q1 calibration panel."""

from __future__ import annotations

import io
import json
from pathlib import Path
import sys
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "build_mcp"))

from bench import generate_e2e_algorithm_matrix_q1_calibration_panel as generator
import build_worker


class Q1CalibrationPanelContractTests(unittest.TestCase):
    def test_generated_authority_is_current_and_exactly_five_by_five(self):
        proof, manifest = generator._build_documents()
        self.assertEqual(
            (ROOT / "bench/bench_e2e_algorithm_matrix_q1_calibration_prefix_proof.json").read_bytes(),
            generator._canonical_bytes(proof),
        )
        self.assertEqual(
            (ROOT / "bench/bench_e2e_algorithm_matrix_q1_calibration_manifest.json").read_bytes(),
            generator._canonical_bytes(manifest),
        )
        self.assertEqual(manifest["panel_contract"]["result_cells"], 5)
        self.assertEqual(manifest["panel_contract"]["expected_raw_records"], 25)
        self.assertEqual(len(manifest["cells"]), 5)
        self.assertEqual([cell["repetitions"] for cell in manifest["cells"]], [5] * 5)
        self.assertEqual(proof["validation_scope"]["pair_count"], 5)
        self.assertEqual(proof["validation_scope"]["q1_records"], 25)

    def test_all_five_are_exact_q8_to_q1_transformations(self):
        for cell in generator.CELLS:
            q1, q8 = generator._validate_pair(cell)
            self.assertEqual(generator._normalized_q1(q1, q8), q8)

    def test_measured_fragment_ksw2_bytes_and_evidence_are_preserved(self):
        _, manifest = generator._build_documents()
        measured = manifest["cells"][1]
        self.assertEqual(
            measured["config_sha256"],
            "2590ad34d3efe5633c3ac79773e633d2bb50de18690a36a885eac2eda531d613",
        )
        self.assertEqual(
            measured["measured_evidence"],
            {
                "preserved": True,
                "complete": True,
                "record_count": 5,
                "report_path": "reporting/bioseq_benchmark_e2e/report_8.json",
                "report_sha256": (
                    "5fc7759f9e38de1c6b541831684b4cf00bd855432965e177bb58858e4c28156f"),
                "raw_path": "reporting/bioseq_benchmark_e2e/report_8.raw.jsonl",
                "raw_sha256": (
                    "dc2fddbc9f8897be0e8909278afa496a70af0bd48a1203718436a4554fdb00b2"),
            },
        )

    def test_worker_accepts_only_the_five_manifest_owned_paths(self):
        for expected_id, path in zip(
                [cell["id"] for cell in generator.CELLS],
                build_worker._ALGORITHM_MATRIX_Q1_CALIBRATION_CONFIGS,
                strict=True):
            authority, error = build_worker._algorithm_matrix_campaign_authority_status(path)
            self.assertIsNone(error)
            self.assertIsNotNone(authority)
            self.assertEqual(authority["name"], "q1")
            self.assertEqual(authority["execution_role"], "q1_calibration_singleton")
            self.assertEqual(authority["selected_cell"]["id"], expected_id)
            self.assertTrue(authority["nonpublication"])

        absolute = str(ROOT / build_worker._ALGORITHM_MATRIX_Q1_CALIBRATION_CONFIGS[0])
        authority, error = build_worker._algorithm_matrix_campaign_authority_status(absolute)
        self.assertIsNone(authority)
        self.assertIn("exact manifest-owned", error)
        authority, error = build_worker._algorithm_matrix_campaign_authority_status(
            "bench/e2e_algorithm_matrix_q1_calibration_shards/impostor.json")
        self.assertIsNone(authority)
        self.assertIsNone(error)

    def test_runtime_contract_has_no_prefilter_persistence_or_batched_ksw2(self):
        _, manifest = generator._build_documents()
        self.assertFalse(manifest["runtime_contract"]["prefilter"])
        self.assertEqual(manifest["runtime_contract"]["pipeline_mode"], "task_parallel")
        self.assertEqual(
            manifest["runtime_contract"]["recompute_async_persistence"], "off")
        self.assertFalse(manifest["runtime_contract"]["ksw2_batched_permitted"])
        for cell in manifest["cells"]:
            config = json.loads((ROOT / cell["path"]).read_text(encoding="utf-8"))
            data = config["test_cases"][generator.TEST_CASE_Q1]["data"]
            self.assertTrue(data["cigar_projection"])
            self.assertTrue(data["mapq_projection"])
            self.assertNotIn("ksw2_batched", json.dumps(config, sort_keys=True))

    def test_darwin_q1_priority_gate_accepts_zero_and_fails_nonzero(self):
        config = build_worker._ALGORITHM_MATRIX_Q1_CALIBRATION_CONFIGS[3]
        output = io.StringIO()
        with (mock.patch.object(build_worker.host_platform, "system", return_value="Darwin"),
              mock.patch.object(build_worker.os, "setpriority") as setpriority,
              mock.patch.object(build_worker.os, "getpriority", return_value=0)):
            build_worker._ensure_darwin_algorithm_matrix_launch_nice(
                "benchmark-e2e", config, output)
        setpriority.assert_called_once_with(build_worker.os.PRIO_PROCESS, 0, 0)
        self.assertIn("worker_NI=0 verified", output.getvalue())

        with (mock.patch.object(build_worker.host_platform, "system", return_value="Darwin"),
              mock.patch.object(build_worker.os, "setpriority"),
              mock.patch.object(build_worker.os, "getpriority", return_value=1)):
            with self.assertRaisesRegex(RuntimeError, "requires verified worker NI=0"):
                build_worker._ensure_darwin_algorithm_matrix_launch_nice(
                    "benchmark-e2e", config, io.StringIO())

    def test_q1_merge_is_explicitly_nonpublication(self):
        output = io.StringIO()
        build_worker._merge_algorithm_matrix_all_results(
            mode="benchmark-e2e",
            report_path=None,
            bench_e2e_config=build_worker._ALGORITHM_MATRIX_Q1_CALIBRATION_CONFIGS[0],
            environment={},
            log_handle=output,
            deadline=0.0,
        )
        self.assertIn("nonpublication evidence", output.getvalue())
        self.assertIn("merge skipped", output.getvalue())

    def test_cpp_identity_gate_uses_closed_schema2_manifest_membership(self):
        source = (ROOT / "bench/e2e/index.cppm").read_text(encoding="utf-8")
        self.assertIn(
            "canonical_q1_calibration_e2e_config_identity", source)
        self.assertIn(
            '"bench_e2e_algorithm_matrix_q1_calibration_manifest.json"', source)
        self.assertIn('manifest.at("cells").size() != 5', source)
        self.assertIn("unique_paths.size() == 5 && matches == 1", source)
        self.assertNotIn(
            "return identity.starts_with(q1_directory)", source)


if __name__ == "__main__":
    unittest.main()
