#!/usr/bin/env python3
"""Unit tests for generic cache benchmark correctness and report contracts."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from typing import Any
import unittest
from unittest import mock


RUNNER_PATH = Path(__file__).with_name("run_private_cluster_bench.py")
SPEC = importlib.util.spec_from_file_location(
    "bioseq_cache_private_benchmark", RUNNER_PATH
)
assert SPEC is not None and SPEC.loader is not None
RUNNER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)


def _core_fragment() -> dict[str, Any]:
    metrics = {
        "encoded_logical_bytes": 21,
        "encoded_stored_bytes": 42,
        "row_group_count": 1,
        "full_block_prevalidation_bytes": 42,
        "full_block_prevalidation_p50_ns": 10,
        "full_block_prevalidation_p95_ns": 11,
        "full_block_prevalidation_p99_ns": 12,
        "fixed_decode_bytes_per_second": 100,
        "memory_copy_bytes_per_second": 90,
    }
    benchmark_names = (
        "column_encode",
        "column_full_prevalidation",
        "column_fixed_decode",
        "admission_reserve_release",
        "memory_copy_reference",
    )
    return {
        "schema_version": 1,
        "rows": 1,
        "samples": 3,
        "correctness": {
            "passed": True,
            "row_count": 1,
            "expected_ordered_checksum": "abc",
            "actual_ordered_checksum": "abc",
            "column_blob_digest": "def",
            "expected_iteration_guard": 0,
            "decode_guard": 0,
            "memory_copy_guard": 0,
            "admission_final_reserved_bytes": 0,
        },
        "metrics": metrics,
        "benchmarks": [
            {
                "name": name,
                "raw_samples_ns": [10, 11, 12],
                "p50_ns": 11,
                "p95_ns": 12,
                "p99_ns": 12,
                "operations": 1,
            }
            for name in benchmark_names
        ],
    }


def _sample_summary() -> dict[str, Any]:
    return {
        "raw_samples_ns": [10, 11, 12],
        "p50_ns": 11,
        "p95_ns": 12,
        "p99_ns": 12,
    }


def _section45_inventory() -> dict[str, Any]:
    cases = []
    for key in sorted(RUNNER.GENERIC_SQL_CASE_KEYS):
        ordered = key in RUNNER.ORDERED_SQL_CASE_KEYS
        digest_name = "ordered_sha256" if ordered else "multiset_sha256"
        disabled = {
            **_sample_summary(),
            "planning": _sample_summary(),
            "time_to_first_row": {"status": "measured"},
            "throughput": {},
        }
        producer = {
            **_sample_summary(),
            "time_to_first_row": {"status": "measured"},
            "throughput": {},
        }
        completed_hit = {
            **_sample_summary(),
            "planning": _sample_summary(),
            "time_to_first_row": {"status": "measured"},
            "throughput": {},
        }
        cases.append(
            {
                "key": key,
                "ordered": ordered,
                "correctness": {
                    "passed": True,
                    "row_count": 1,
                    digest_name: "digest",
                },
                "disabled": disabled,
                "producer": producer,
                "completed_hit": completed_hit,
                "runtime_storage": {
                    "dsa_hot_bytes": {"status": "measured", "value": 1},
                    "file_bytes": {"status": "measured", "value": 0},
                    "row_group_count": {"status": "measured", "value": 1},
                    "prevalidation_bytes": {
                        "status": "measured",
                        "value": 1,
                    },
                },
            }
        )
    fanout = [
        {
            "clients": clients,
            "correctness": {"passed": True},
            "waiter_latency": _sample_summary(),
            "wait_duration": {"status": "bounded_measurement"},
            "child_execution_count": {"status": "measured", "value": 1},
            "cache_behavior": {"passed": True},
        }
        for clients in (2, 8, 32)
    ]
    invalidation = [
        {
            "key": f"{operation}_invalidation",
            "operation_latency": _sample_summary(),
            "correctness": {"passed": True},
        }
        for operation in ("insert", "update", "delete")
    ]
    thresholds = [
        {
            "name": name,
            "status": "pass",
            "passed": True,
            "actual": 1,
            "limit": 1,
            "detail": "evidence",
        }
        for name in sorted(RUNNER.SECTION45_THRESHOLD_NAMES)
    ]
    return {
        "core": {"metrics": {"encoded_stored_bytes": 1}},
        "cases": cases,
        "generic_provider": {
            "overlapping_predicate_reuse": {
                "reuse": {
                    "predicate_runtime": {
                        "late_fetch_rows": {"status": "measured", "value": 0},
                        "late_fetch_bytes": {"status": "measured", "value": 0},
                        "selection_ratio": {"status": "measured", "value": 1},
                    }
                }
            }
        },
        "fanout": fanout,
        "invalidation": invalidation,
        "thresholds": thresholds,
        "resource_usage": {
            "status": "measured",
            "page_cache_and_physical_io": {"status": "measured"},
        },
    }


class BenchmarkCorrectnessContractTest(unittest.TestCase):
    def test_sql_case_matrix_requires_every_generic_result_shape(self) -> None:
        cases_path = Path(__file__).with_name("sql_cases.json")
        document = RUNNER._read_cases(cases_path)
        self.assertIn("ordered_result", {
            case["key"] for case in document["cases"]
        })

        incomplete = copy.deepcopy(document)
        incomplete["cases"] = [
            case
            for case in incomplete["cases"]
            if case["key"] != "ordered_result"
        ]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "cases.json"
            path.write_text(json.dumps(incomplete), encoding="utf-8")
            with self.assertRaisesRegex(RUNNER.BenchmarkError, "ordered_result"):
                RUNNER._read_cases(path)

        wrong_ordering = copy.deepcopy(document)
        next(
            case for case in wrong_ordering["cases"]
            if case["key"] == "ordered_result"
        )["ordered"] = False
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "cases.json"
            path.write_text(json.dumps(wrong_ordering), encoding="utf-8")
            with self.assertRaisesRegex(RUNNER.BenchmarkError, "ordered-checksum"):
                RUNNER._read_cases(path)

    def test_ordered_and_multiset_digests_have_distinct_contracts(self) -> None:
        first = RUNNER._digest_output("a\nb\n", 10, 4, 10)
        reordered = RUNNER._digest_output("b\na\n", 11, 5, 11)

        self.assertEqual(first.rows, 2)
        self.assertNotEqual(first.ordered_sha256, reordered.ordered_sha256)
        self.assertEqual(first.multiset_sha256, reordered.multiset_sha256)
        self.assertFalse(RUNNER._same_result(first, reordered, True))
        self.assertTrue(RUNNER._same_result(first, reordered, False))

        duplicates = RUNNER._digest_output("a\na\nb\n", 12, 4, 12)
        different_multiplicity = RUNNER._digest_output("a\nb\nb\n", 13, 5, 13)
        self.assertNotEqual(
            duplicates.multiset_sha256,
            different_multiplicity.multiset_sha256,
        )

    def test_empty_result_has_zero_rows_and_no_first_row_metric(self) -> None:
        output = RUNNER._digest_output("", 10, None, 10)

        self.assertEqual(output.rows, 0)
        summary = RUNNER._first_row_summary([output])
        self.assertEqual(summary["status"], "not_applicable")
        self.assertEqual(summary["raw_samples_ns"], [])

    def test_core_fragment_requires_matching_checksum_and_all_series(self) -> None:
        valid = _core_fragment()
        self.assertIs(RUNNER._validate_core_benchmark_fragment(valid), valid)

        mismatch = copy.deepcopy(valid)
        mismatch["correctness"]["actual_ordered_checksum"] = "wrong"
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "checksum"):
            RUNNER._validate_core_benchmark_fragment(mismatch)

        missing_series = copy.deepcopy(valid)
        missing_series["benchmarks"].pop()
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "mandatory"):
            RUNNER._validate_core_benchmark_fragment(missing_series)

        forged_percentile = copy.deepcopy(valid)
        forged_percentile["benchmarks"][0]["p50_ns"] = 10
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "percentile"):
            RUNNER._validate_core_benchmark_fragment(forged_percentile)

        skipped_copy = copy.deepcopy(valid)
        skipped_copy["correctness"]["memory_copy_guard"] = 1
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "checksum"):
            RUNNER._validate_core_benchmark_fragment(skipped_copy)

        negative_sample = copy.deepcopy(valid)
        negative_sample["benchmarks"][0]["raw_samples_ns"][0] = -1
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "raw/p50/p95/p99"):
            RUNNER._validate_core_benchmark_fragment(negative_sample)

        duplicate_series = copy.deepcopy(valid)
        duplicate_series["benchmarks"][-1]["name"] = "column_encode"
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "duplicate"):
            RUNNER._validate_core_benchmark_fragment(duplicate_series)

    def test_runtime_snapshot_reports_available_and_blocked_metrics(self) -> None:
        entries = (
            '[{"logical_bytes":10,"stored_bytes":8,'
            '"prevalidation_bytes":8,"row_group_count":2},'
            '{"logical_bytes":20,"stored_bytes":16,'
            '"prevalidation_bytes":16,"row_group_count":3}]\n'
        )
        with mock.patch.object(RUNNER, "_run_sql", return_value=(entries, 1)):
            snapshot = RUNNER._entry_runtime_snapshot(object())

        self.assertEqual(
            snapshot["prevalidation_bytes"]["status"], "measured"
        )
        self.assertEqual(snapshot["prevalidation_bytes"]["value"], 24)
        self.assertEqual(snapshot["row_group_count"]["value"], 5)
        self.assertEqual(snapshot["dsa_hot_bytes"]["status"], "unavailable")

    def test_runtime_storage_uses_byte_ledger_not_block_counts(self) -> None:
        admin = {
            "dsa_hot_bytes": {"status": "unavailable"},
            "file_bytes": {"status": "unavailable"},
        }
        tier = {
            "force_file_tier": False,
            "dsa_blocks": 3,
            "file_blocks": 2,
            "reserved_hot_bytes": 1234,
            "reserved_file_bytes": 5678,
        }
        with mock.patch.object(
            RUNNER, "_entry_runtime_snapshot", return_value=admin
        ), mock.patch.object(
            RUNNER, "_test_storage_tier_snapshot", return_value=tier
        ):
            snapshot = RUNNER._runtime_storage_snapshot(object())

        self.assertEqual(snapshot["dsa_hot_bytes"]["value"], 1234)
        self.assertEqual(snapshot["file_bytes"]["value"], 5678)
        self.assertNotEqual(snapshot["dsa_hot_bytes"]["value"], 3)

    def test_provider_phase_reports_exact_late_fetch_bytes(self) -> None:
        output = RUNNER.QueryOutput(
            elapsed_ns=100,
            first_row_ns=50,
            completed_at_ns=200,
            rows=1,
            ordered_sha256="ordered",
            multiset_sha256="multiset",
            stdout_bytes=12,
        )
        cache_before = {
            name: 0
            for name in (
                "produced",
                "completed_hits",
                "inflight_waits",
                "reuse_hits",
                "fallbacks",
                "invalidations",
                "evictions",
                "logical_bytes",
                "stored_bytes",
            )
        }
        provider_before = {
            name: 0 for name in RUNNER.PROVIDER_COUNTER_NAMES
        }
        provider_after = dict(provider_before)
        provider_after.update(
            {
                "late_fetch_references": 2,
                "late_fetch_values": 3,
                "late_fetch_bytes": 17,
            }
        )
        with mock.patch.object(
            RUNNER, "_storage_snapshot", return_value={}
        ), mock.patch.object(
            RUNNER, "_filesystem_policy_snapshot", return_value={}
        ):
            report = RUNNER._provider_phase_report(
                object(),
                reference=output,
                outputs=[output],
                ordered=True,
                cache_before=cache_before,
                cache_after=cache_before,
                provider_before=provider_before,
                provider_after=provider_after,
                input_rows=4,
            )

        self.assertEqual(
            report["predicate_runtime"]["late_fetch_bytes"],
            {
                "status": "measured",
                "value": 17,
                "detail": (
                    "Sum of datumGetSize for every successfully emitted non-null "
                    "late-fetch value; null values and occurrence/status envelopes "
                    "contribute zero bytes."
                ),
            },
        )

    def test_section45_inventory_rejects_silent_metric_loss(self) -> None:
        inventory = _section45_inventory()
        coverage = RUNNER._validate_section45_inventory(**inventory)
        self.assertEqual(coverage["status"], "complete")
        self.assertEqual(coverage["fanout_clients"], [2, 8, 32])

        missing_dsa = copy.deepcopy(inventory)
        missing_dsa["cases"][0]["runtime_storage"].pop("dsa_hot_bytes")
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "DSA/file"):
            RUNNER._validate_section45_inventory(**missing_dsa)

        missing_threshold = copy.deepcopy(inventory)
        missing_threshold["thresholds"].pop()
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "threshold"):
            RUNNER._validate_section45_inventory(**missing_threshold)


if __name__ == "__main__":
    unittest.main()
