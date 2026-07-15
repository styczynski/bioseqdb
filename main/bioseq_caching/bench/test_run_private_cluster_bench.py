#!/usr/bin/env python3
"""Unit tests for generic cache benchmark correctness and report contracts."""

from __future__ import annotations

import copy
import dataclasses
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
                    "dsa_hot_bytes": {
                        "status": "measured",
                        "value": 1,
                        "unit": "bytes",
                        "source_field": (
                            "bioseq_cache_test.storage_tier_stats."
                            "reserved_hot_bytes"
                        ),
                    },
                    "file_bytes": {
                        "status": "measured",
                        "value": 0,
                        "unit": "bytes",
                        "source_field": (
                            "bioseq_cache_test.storage_tier_stats."
                            "reserved_file_bytes"
                        ),
                    },
                    "row_group_count": {"status": "measured", "value": 1},
                    "prevalidation_bytes": {
                        "status": "measured",
                        "value": 1,
                    },
                    "prevalidation_duration_ns": {
                        "status": "measured",
                        "value": 1,
                    },
                },
            }
        )
        if key == "large_toasted_values":
            cases[-1]["file_hit_temperature"] = {
                "status": "measured",
                "passed": True,
                "first_cold_file_hit": {
                    "process_resources": {
                        "status": "measured",
                        "page_cache_and_physical_io": {
                            "status": "measured",
                            "physical_read_bytes": 4096,
                        },
                    },
                },
                "full_block_prevalidation": {
                    "status": "measured",
                    "passed": True,
                    "bytes": {"status": "measured", "value": 1},
                    "duration_ns": {"status": "measured", "value": 1},
                    "row_group_count": {"status": "measured", "value": 1},
                    "validated_result_checksum": {
                        "status": "measured",
                        "algorithm": "sha256",
                        "reference": "digest",
                        "first_cold_file_hit": "digest",
                        "warm_file_hits": ["digest"],
                        "passed": True,
                    },
                },
            }
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
        "streaming_protocol": {
            "status": "measured",
            "passed": True,
            "minimum_post_first_row_ns": 100,
            "first_row_ns": 10,
            "total_elapsed_ns": 200,
            "post_first_row_ns": 190,
        },
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
    def test_sql_sample_summaries_use_nearest_rank(self) -> None:
        summary = RUNNER._sample_summary([10, 20, 30, 40])

        self.assertEqual(summary["p50_ns"], 20)
        self.assertEqual(summary["p95_ns"], 40)
        self.assertEqual(summary["p99_ns"], 40)
        with self.assertRaisesRegex(
            RUNNER.BenchmarkError, "invalid inputs"
        ):
            RUNNER._sample_summary([True, 20])

    def test_waiter_latency_uses_two_sided_material_delta(self) -> None:
        self.assertTrue(
            RUNNER._latency_delta_within_tolerance(100, 120, 25)
        )
        self.assertTrue(
            RUNNER._latency_delta_within_tolerance(100, 80, 25)
        )
        self.assertFalse(
            RUNNER._latency_delta_within_tolerance(100, 50, 25)
        )

    def test_first_row_protocol_requires_a_delayed_later_row(self) -> None:
        streamed = RUNNER.QueryOutput(
            elapsed_ns=300_000_000,
            first_row_ns=10_000_000,
            completed_at_ns=300_000_000,
            rows=2,
            ordered_sha256="ordered",
            multiset_sha256="multiset",
            stdout_bytes=4,
        )
        self.assertTrue(
            RUNNER._streaming_protocol_probe_passed(
                streamed,
                ["1", "2"],
                minimum_post_first_row_ns=150_000_000,
            )
        )

        buffered = dataclasses.replace(
            streamed, first_row_ns=299_000_000
        )
        self.assertFalse(
            RUNNER._streaming_protocol_probe_passed(
                buffered,
                ["1", "2"],
                minimum_post_first_row_ns=150_000_000,
            )
        )

        boolean_timing = dataclasses.replace(streamed, first_row_ns=True)
        self.assertFalse(
            RUNNER._streaming_protocol_probe_passed(
                boolean_timing,
                ["1", "2"],
                minimum_post_first_row_ns=150_000_000,
            )
        )

    def test_ttl_cleanup_requires_a_usable_complete_generation(self) -> None:
        valid = {
            "ttl_ms": 100,
            "expiry_wait_ns": 100_000_000,
            "correctness": {"passed": True},
            "pre_expiry": {
                "passed": True,
                "complete_entries": 1,
                "stats": {
                    "produced": 1,
                    "completed_hits": 1,
                    "fallbacks": 0,
                },
            },
            "reclamation": {
                "complete_entries": 0,
                "tier": {
                    "dsa_blocks": 0,
                    "file_blocks": 0,
                    "reserved_hot_bytes": 0,
                    "reserved_file_bytes": 0,
                },
                "store": {"file_count": 0, "bytes": 0},
            },
        }
        self.assertTrue(RUNNER._ttl_cleanup_contract_passed(valid))

        no_publication = copy.deepcopy(valid)
        no_publication["pre_expiry"]["complete_entries"] = 0
        no_publication["pre_expiry"]["stats"]["produced"] = 0
        self.assertFalse(
            RUNNER._ttl_cleanup_contract_passed(no_publication)
        )

        boolean_stats = copy.deepcopy(valid)
        boolean_stats["pre_expiry"]["stats"]["produced"] = True
        self.assertFalse(
            RUNNER._ttl_cleanup_contract_passed(boolean_stats)
        )

        boolean_tier = copy.deepcopy(valid)
        boolean_tier["reclamation"]["tier"]["dsa_blocks"] = False
        self.assertFalse(
            RUNNER._ttl_cleanup_contract_passed(boolean_tier)
        )

        boolean_store = copy.deepcopy(valid)
        boolean_store["reclamation"]["store"]["file_count"] = False
        self.assertFalse(
            RUNNER._ttl_cleanup_contract_passed(boolean_store)
        )

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
        self.assertEqual(snapshot["dsa_hot_bytes"]["unit"], "bytes")
        self.assertEqual(snapshot["file_bytes"]["unit"], "bytes")
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

        missing_dsa_value = copy.deepcopy(inventory)
        missing_dsa_value["cases"][0]["runtime_storage"][
            "dsa_hot_bytes"
        ].pop("value")
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "DSA/file"):
            RUNNER._validate_section45_inventory(**missing_dsa_value)

        boolean_dsa_value = copy.deepcopy(inventory)
        boolean_dsa_value["cases"][0]["runtime_storage"][
            "dsa_hot_bytes"
        ]["value"] = True
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "DSA/file"):
            RUNNER._validate_section45_inventory(**boolean_dsa_value)

        negative_file_value = copy.deepcopy(inventory)
        negative_file_value["cases"][0]["runtime_storage"][
            "file_bytes"
        ]["value"] = -1
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "DSA/file"):
            RUNNER._validate_section45_inventory(**negative_file_value)

        wrong_file_unit = copy.deepcopy(inventory)
        wrong_file_unit["cases"][0]["runtime_storage"][
            "file_bytes"
        ]["unit"] = "blocks"
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "DSA/file"):
            RUNNER._validate_section45_inventory(**wrong_file_unit)

        wrong_dsa_source = copy.deepcopy(inventory)
        wrong_dsa_source["cases"][0]["runtime_storage"][
            "dsa_hot_bytes"
        ]["source_field"] = "dsa_blocks"
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "DSA/file"):
            RUNNER._validate_section45_inventory(**wrong_dsa_source)

        boolean_streaming = copy.deepcopy(inventory)
        boolean_streaming["streaming_protocol"]["first_row_ns"] = True
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "streaming"):
            RUNNER._validate_section45_inventory(**boolean_streaming)

        contradictory_streaming = copy.deepcopy(inventory)
        contradictory_streaming["streaming_protocol"][
            "post_first_row_ns"
        ] = 189
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "streaming"):
            RUNNER._validate_section45_inventory(**contradictory_streaming)

        missing_threshold = copy.deepcopy(inventory)
        missing_threshold["thresholds"].pop()
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "threshold"):
            RUNNER._validate_section45_inventory(**missing_threshold)

    def test_section45_inventory_rejects_unproved_thresholds(self) -> None:
        not_applicable = _section45_inventory()
        not_applicable["thresholds"][0].update(
            {"status": "not_applicable", "passed": None}
        )
        with self.assertRaisesRegex(
            RUNNER.BenchmarkError, "mandatory threshold"
        ):
            RUNNER._validate_section45_inventory(**not_applicable)

        missing_passed = _section45_inventory()
        missing_passed["thresholds"][0].pop("passed")
        with self.assertRaisesRegex(
            RUNNER.BenchmarkError, "status/passed evidence"
        ):
            RUNNER._validate_section45_inventory(**missing_passed)

        contradictory = _section45_inventory()
        contradictory["thresholds"][0]["passed"] = False
        with self.assertRaisesRegex(
            RUNNER.BenchmarkError, "status/passed evidence"
        ):
            RUNNER._validate_section45_inventory(**contradictory)

    def test_section45_inventory_rejects_forged_sql_percentiles(self) -> None:
        interpolated = _section45_inventory()
        interpolated["cases"][0]["disabled"]["p95_ns"] = 11
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "percentile"):
            RUNNER._validate_section45_inventory(**interpolated)

        negative = _section45_inventory()
        negative["fanout"][0]["waiter_latency"][
            "raw_samples_ns"
        ][0] = -1
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "raw samples"):
            RUNNER._validate_section45_inventory(**negative)

        forged_throughput = _section45_inventory()
        forged_throughput["cases"][0]["producer"]["throughput"] = {
            "rows_per_second": {
                "raw_samples": [10, 20, 30],
                "p50": 20,
                "p95": 20,
                "p99": 30,
            }
        }
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "percentile"):
            RUNNER._validate_section45_inventory(**forged_throughput)

    def test_section45_inventory_rejects_unavailable_prevalidation(self) -> None:
        unavailable_rows = _section45_inventory()
        unavailable_rows["cases"][0]["runtime_storage"][
            "row_group_count"
        ] = {"status": "unavailable", "value": None}
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "row-group"):
            RUNNER._validate_section45_inventory(**unavailable_rows)

        unavailable_duration = _section45_inventory()
        unavailable_duration["cases"][0]["runtime_storage"][
            "prevalidation_duration_ns"
        ] = {"status": "unavailable", "value": None}
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "duration"):
            RUNNER._validate_section45_inventory(**unavailable_duration)

        forged_file = _section45_inventory()
        next(
            case for case in forged_file["cases"]
            if case["key"] == "large_toasted_values"
        )["file_hit_temperature"] = {
            "status": "measured",
            "passed": True,
            "mandatory_checksum_prepass_included": True,
        }
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "checksum evidence"):
            RUNNER._validate_section45_inventory(**forged_file)

        zero_io = _section45_inventory()
        file_case = next(
            case for case in zero_io["cases"]
            if case["key"] == "large_toasted_values"
        )
        file_case["file_hit_temperature"]["first_cold_file_hit"][
            "process_resources"
        ]["page_cache_and_physical_io"]["physical_read_bytes"] = 0
        with self.assertRaisesRegex(RUNNER.BenchmarkError, "physical I/O"):
            RUNNER._validate_section45_inventory(**zero_io)


if __name__ == "__main__":
    unittest.main()
