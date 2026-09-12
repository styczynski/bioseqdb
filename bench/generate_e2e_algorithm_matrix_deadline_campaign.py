#!/usr/bin/env python3
"""Generate the sealed deadline-bounded algorithm comparison campaign.

The campaign deliberately separates algorithm, outer-factor, and query-scale
coverage.  It only writes benchmark JSON and its manifest; it never executes a
benchmark or mutates a results ledger.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import tempfile
from typing import Any

try:
    from bench import generate_e2e_algorithm_matrix_shards as base
except ModuleNotFoundError:  # Direct execution from bench/.
    import generate_e2e_algorithm_matrix_shards as base


ROOT = Path(__file__).resolve().parents[1]
OUTPUT_DIRECTORY = ROOT / "bench/e2e_algorithm_matrix_deadline_v2_shards"
MANIFEST_PATH = ROOT / "bench/bench_e2e_algorithm_matrix_deadline_v2_manifest.json"
CAMPAIGN_ID = "bioseq-e2e-algorithm-matrix-deadline-v2"
CAMPAIGN_DIGEST = (
    "44a85360a5e975ec8a6d70ac0b06486c51d748770c2ada46106d2ccc5db4728a")
SCHEMA = "bioseq.e2e.algorithm-matrix-deadline-campaign"
SCHEMA_VERSION = 2
REPETITIONS = 5
NORMAL_TIMEOUT_SECONDS = 14_400
EXTENDED_TIMEOUT_SECONDS = 172_800
FIXED_SECONDS = 300.0
DEADLINE_SECONDS = 7.0 * 24.0 * 60.0 * 60.0

PARENT_MANIFEST = ROOT / "bench/bench_e2e_algorithm_matrix_deadline_manifest.json"
PARENT_OUTPUT_DIRECTORY = ROOT / "bench/e2e_algorithm_matrix_deadline_shards"
PARENT_CAMPAIGN_ID = "bioseq-e2e-algorithm-matrix-deadline-v1"
PARENT_CAMPAIGN_DIGEST = (
    "3d03b3aea29a5418721ba5899c58a698f568ca10dddf9f00eaab42adf450a3b0")
PARENT_MANIFEST_SHA256 = (
    "73cd6928e7430c46bff2836f0bb0820565e9d5c9b0011939ff4aad9fbe771fa0")
PARENT_SHARD_TREE_SHA256 = (
    "297634d7b22f4ca05bef640282acd09c92e17cd23b544ce624e1c59ba4838270")
PARENT_EXPECTED_SHARDS = 59
PARENT_EXPECTED_RAW_RECORDS = 295

FULL_DATASET = "M2_D4_K15_W10"
FULL_QUERY_COUNT = 16
OUTER_QUERY_COUNT = 2
ANCHOR_QUERY_COUNT = 1024

MINIMIZERS = base.EXPECTED_MINIMIZERS
CHAINING = (
    "scalar_minimap2",
    "simd_minimap2",
    "range_minimum_query",
    "vulkan_tiled_dynamic_programming",
)
BASE_ALIGNERS = ("ksw2", "block_aligner")
INDEX_NAMES = base.EXPECTED_INDEXES

# Accepted wall-clock sums for all five repeats. Q1 is estimate-only input; no
# q1 config or q1 result is a member of this production campaign.
ANCHORS = {
    "q1_fragment": {
        "report": "reporting/bioseq_benchmark_e2e/report_8.json",
        "wall_sum_seconds": 188.545968041,
    },
    "q1_sequence": {
        "report": "reporting/bioseq_benchmark_e2e/report_13.json",
        "wall_sum_seconds": 1118.563596459,
    },
    "q8_fragment_ksw2": {
        "report": "reporting/bioseq_benchmark_e2e/report_9.json",
        "wall_sum_seconds": 1070.047085292,
    },
    "q8_fragment_block": {
        "report": "reporting/bioseq_benchmark_e2e/report_10.json",
        "wall_sum_seconds": 1084.578870334,
    },
    "q8_sequence_ksw2": {
        "report": "reporting/bioseq_benchmark_e2e/report_11.json",
        "wall_sum_seconds": 8946.950796625,
    },
    "q8_sequence_block": {
        "report": "reporting/bioseq_benchmark_e2e/report_12.json",
        "wall_sum_seconds": 8975.231243748,
    },
    "q8_minimap2": {
        "report": "reporting/bioseq_benchmark_e2e/report_5.json",
        "wall_sum_seconds": 212.328001708,
    },
}

SOURCE_BINDINGS = (
    "bench/bench_e2e_algorithm_matrix_campaign.json",
    "bench/e2e/index.cppm",
    "bioseq/backend/pg/configuration/aligner.cppm",
    "bioseq/backend/pipeline/runtime_contract.cppm",
    "bioseq/backend/pipeline/minimap_parameters_contract.cppm",
    "bioseq/backend/sequences/generation/sequence_generator.cppm",
)

REQUIRED_BIOSEQ_METRICS = {
    "tuple_accepted",
    "match_count",
    "correct_match_count",
    "correct_cigar_count",
    "mapq_result_count",
    "pipeline_validation_accepted",
    "stage_engine_matrix_valid",
    "backend_declines",
    "backend_fallbacks",
    "prefilter_activity_count",
    "benchmark_e2e_interval_available",
    "benchmark_e2e_interval_count",
    "benchmark_e2e_cigar_interval_count",
    "benchmark_e2e_mapq_interval_count",
    "benchmark_e2e_interval_finalization_count",
    "downstream_active_wall_available",
    "mapq_execution_available",
    "mapq_executed",
}


class GenerationError(RuntimeError):
    """Raised when source or generated authority is inconsistent."""


def _pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise GenerationError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _load(path: Path) -> tuple[dict[str, Any], bytes]:
    raw = path.read_bytes()
    value = json.loads(raw.decode("utf-8"), object_pairs_hook=_pairs)
    if not isinstance(value, dict):
        raise GenerationError(f"{path} must contain an object")
    return value, raw


def _render(value: Any) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=True, allow_nan=False)
            + "\n").encode("utf-8")


def _canonical_sha(value: Any) -> str:
    raw = json.dumps(
        value, sort_keys=True, separators=(",", ":"),
        ensure_ascii=True, allow_nan=False).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()


def _sha(path_or_raw: Path | bytes) -> str:
    raw = path_or_raw.read_bytes() if isinstance(path_or_raw, Path) else path_or_raw
    return hashlib.sha256(raw).hexdigest()


def _schedule_tree_sha256(schedule: list[dict[str, Any]]) -> str:
    lines = "".join(
        f"{entry['config_sha256']}  {entry['path']}\n"
        for entry in sorted(schedule, key=lambda entry: entry["path"])
    ).encode("utf-8")
    return hashlib.sha256(lines).hexdigest()


def _validated_parent_lineage() -> dict[str, Any]:
    parent, parent_raw = _load(PARENT_MANIFEST)
    if _sha(parent_raw) != PARENT_MANIFEST_SHA256:
        raise GenerationError("parent v1 manifest SHA-256 is not immutable")
    campaign = parent.get("campaign")
    schedule = parent.get("schedule")
    if (parent.get("schema") != SCHEMA
            or parent.get("version") != 1
            or not isinstance(campaign, dict)
            or campaign.get("campaign_id") != PARENT_CAMPAIGN_ID
            or campaign.get("campaign_digest") != PARENT_CAMPAIGN_DIGEST
            or campaign.get("expected_shards") != PARENT_EXPECTED_SHARDS
            or campaign.get("expected_result_cells") != PARENT_EXPECTED_SHARDS
            or campaign.get("expected_raw_records") != PARENT_EXPECTED_RAW_RECORDS
            or not isinstance(schedule, list)
            or len(schedule) != PARENT_EXPECTED_SHARDS):
        raise GenerationError("parent v1 campaign identity/cardinality changed")
    digest_payload = copy.deepcopy(parent)
    digest_payload["campaign"].pop("campaign_digest", None)
    if _canonical_sha(digest_payload) != PARENT_CAMPAIGN_DIGEST:
        raise GenerationError("parent v1 canonical campaign digest changed")
    expected_paths: set[Path] = set()
    for ordinal, entry in enumerate(schedule):
        if (not isinstance(entry, dict)
                or entry.get("ordinal") != ordinal
                or not isinstance(entry.get("path"), str)
                or not isinstance(entry.get("config_sha256"), str)):
            raise GenerationError("parent v1 schedule entry changed shape")
        path = ROOT / entry["path"]
        try:
            path.relative_to(PARENT_OUTPUT_DIRECTORY)
        except ValueError as exc:
            raise GenerationError("parent v1 shard escaped its directory") from exc
        if not path.is_file() or _sha(path) != entry["config_sha256"]:
            raise GenerationError(f"parent v1 shard seal changed: {entry['path']}")
        expected_paths.add(path)
    existing_paths = set(PARENT_OUTPUT_DIRECTORY.glob("*.json"))
    if existing_paths != expected_paths:
        raise GenerationError("parent v1 shard membership changed")
    if _schedule_tree_sha256(schedule) != PARENT_SHARD_TREE_SHA256:
        raise GenerationError("parent v1 shard-tree seal changed")
    return {
        "parent_campaign_id": PARENT_CAMPAIGN_ID,
        "parent_campaign_digest": PARENT_CAMPAIGN_DIGEST,
        "parent_manifest": PARENT_MANIFEST.relative_to(ROOT).as_posix(),
        "parent_manifest_sha256": PARENT_MANIFEST_SHA256,
        "parent_shard_directory": (
            PARENT_OUTPUT_DIRECTORY.relative_to(ROOT).as_posix()),
        "parent_shard_tree_sha256": PARENT_SHARD_TREE_SHA256,
        "parent_expected_shards": PARENT_EXPECTED_SHARDS,
        "parent_expected_raw_records": PARENT_EXPECTED_RAW_RECORDS,
        "parent_artifact_policy": "immutable_exact_parent_authority",
    }


def _walk(value: Any):
    if isinstance(value, dict):
        for key, child in value.items():
            yield key, child
            yield from _walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from _walk(child)


def _report_wall_sum_seconds(report: dict[str, Any]) -> float:
    try:
        tuples = report["benchmarks"]["bench_e2e"]["execution_tuples"]
        cases = tuples[0]["test_cases"]
        regions = next(iter(cases.values()))["all_regions"]
        metric = regions["wall_clock_ns"]
        if metric["samples_count"] != REPETITIONS:
            raise GenerationError("anchor does not contain five wall samples")
        return float(metric["stats"]["sum"]) / 1_000_000_000.0
    except (KeyError, IndexError, StopIteration, TypeError, ValueError) as exc:
        raise GenerationError(f"anchor report has no exact wall sum: {exc}") from exc


def _validated_anchor_bindings() -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for name, specification in ANCHORS.items():
        path = ROOT / specification["report"]
        report, raw = _load(path)
        actual = _report_wall_sum_seconds(report)
        expected = specification["wall_sum_seconds"]
        if not math.isclose(actual, expected, rel_tol=0.0, abs_tol=1e-9):
            raise GenerationError(
                f"{name} wall sum changed: actual={actual} expected={expected}")
        result[name] = {
            "report": specification["report"],
            "report_sha256": _sha(raw),
            "query_count": 1 if name.startswith("q1_") else 8,
            "repetitions": REPETITIONS,
            "wall_sum_seconds": expected,
            "authority_role": (
                "estimate_reference_only" if name.startswith("q1_")
                else "accepted_q8_runtime_anchor"),
        }
    return result


def _linear_estimate(q1: float, q8: float, query_count: int) -> float:
    if query_count < 1:
        raise GenerationError("query count must be positive")
    return q1 + (q8 - q1) * float(query_count - 1) / 7.0


def _outer_multiplier(data: dict[str, Any]) -> float:
    result = float(data["reference_length"]) / 2_000_000_000.0
    if data["alphabet"] == "dna5":
        result *= 1.25
    if (data["kmer_size"], data["window_size"]) == (20, 15):
        result *= 1.10
    return result


def _test_case(
        source: dict[str, Any], dataset: str, query_count: int) -> tuple[str, dict[str, Any]]:
    name = f"{dataset}_Q{query_count}_DEADLINE"
    case = copy.deepcopy(source["test_cases"][dataset])
    case["data"]["fragment_count"] = query_count
    case["description"] = (
        f"DEADLINE_PRODUCTION: {case['data']['reference_length']} bp "
        f"{case['data']['alphabet']} k{case['data']['kmer_size']}/"
        f"w{case['data']['window_size']}, exact fixed-seed q{query_count} prefix")
    return name, case


def _singleton_config(
        source: dict[str, Any], *, dataset: str, query_count: int,
        algorithm: str, parameters: dict[str, Any], tier: str) -> dict[str, Any]:
    config = copy.deepcopy(source)
    case_name, case = _test_case(source, dataset, query_count)
    config["test_cases"] = {case_name: case}
    config["params"] = {algorithm: copy.deepcopy(source["params"][algorithm])}
    benchmark = config["bench_e2e"]
    benchmark["tags"] = [
        "e2e", "algorithm-matrix", "deadline-campaign", tier, f"q{query_count}"]
    benchmark["description"] = (
        f"Deadline campaign singleton {tier}, q{query_count}, {dataset}, {algorithm}.")
    benchmark["matrix"]["for"] = {case_name: REPETITIONS}
    axes = copy.deepcopy(source["bench_e2e"]["matrix"]["over"][algorithm])
    for key in axes:
        if key not in parameters:
            raise GenerationError(f"missing singleton parameter {algorithm}.{key}")
        axes[key] = [parameters[key]]
    benchmark["matrix"]["over"] = {algorithm: axes}
    benchmark["matrix"]["metrics"]["match_count"]["rules"] = []
    metric_names = (
        base.MINIMAP2_BASELINE_METRIC_NAMES
        if algorithm in base.BASELINE_ALGORITHMS
        else base.MIXED_PERSISTED_METRIC_NAMES)
    metrics = benchmark["matrix"]["metrics"]
    benchmark["matrix"]["metrics"] = {
        name: metrics[name] for name in metric_names}
    base._restrict_metric_applicability(config)
    config["regression_rules"] = []
    serialized = json.dumps(config, sort_keys=True)
    if "ksw2_batched" in serialized:
        raise GenerationError("ksw2_batched escaped into a deadline config")
    for key, value in _walk(config):
        if key.lower() in {"prefilter", "prefilter_enabled", "use_prefilter"} \
                and value not in (False, 0, None, "off"):
            raise GenerationError("prefilter must remain absent/default-off")
    if algorithm in base.BIOSEQ_ALGORITHMS:
        declared = set(benchmark["matrix"]["metrics"])
        if not REQUIRED_BIOSEQ_METRICS.issubset(declared):
            raise GenerationError("Bioseq runtime proof counters were dropped")
    return config


def _slug(value: str) -> str:
    return (value.lower().replace("minimizers_per_fragment", "mpf")
            .replace("minimizers_per_sequence", "mps")
            .replace("vulkan_tiled_dynamic_programming", "vulkan_dp")
            .replace("vulkan_minimap_integer", "vulkan_seed")
            .replace("simd_minimap2_reference", "simd_ref")
            .replace("range_minimum_query", "rmq")
            .replace("bounded_nthash_simd", "bounded_nthash"))


def _identity(
        *, tier: str, query_count: int, dataset: str, algorithm: str,
        parameters: dict[str, Any]) -> dict[str, Any]:
    bioseq = algorithm in base.BIOSEQ_ALGORITHMS
    return {
        "campaign_id": CAMPAIGN_ID,
        "tier": tier,
        "query_count": query_count,
        "dataset": dataset,
        "index_mode": (
            base.BIOSEQ_ALGORITHMS.get(algorithm)
            if bioseq else base.BASELINE_ALGORITHMS[algorithm]),
        "engine": "bioseq" if bioseq else "minimap2",
        "bioseq_index_name": parameters.get("minimizer_index"),
        "requested_minimizer_algorithm": parameters.get("minimizer_algorithm"),
        "requested_chaining_algorithm": parameters.get("chaining_algorithm"),
        "requested_base_alignment_algorithm": parameters.get(
            "base_alignment_algorithm"),
    }


def _stable_key(identity: dict[str, Any]) -> str:
    return "bioseq-e2e-deadline-cell-v2:" + json.dumps(
        identity, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def _expected_disposition(identity: dict[str, Any]) -> str | None:
    if identity["engine"] != "bioseq":
        return None
    if (identity["requested_minimizer_algorithm"] == "vulkan_minimap_integer"
            or identity["requested_chaining_algorithm"]
                == "vulkan_tiled_dynamic_programming"):
        return "harness_proven_sql_rejection_or_exact_route_if_runtime_contract_changes"
    return None


def _add_shard(
        outputs: dict[Path, bytes], schedule: list[dict[str, Any]],
        source: dict[str, Any], *, tier: str, query_count: int, dataset: str,
        algorithm: str, parameters: dict[str, Any], timed_seconds: float,
        contingency: float, timeout_seconds: int) -> None:
    identity = _identity(
        tier=tier, query_count=query_count, dataset=dataset,
        algorithm=algorithm, parameters=parameters)
    stable_key = _stable_key(identity)
    ordinal = len(schedule)
    config = _singleton_config(
        source, dataset=dataset, query_count=query_count,
        algorithm=algorithm, parameters=parameters, tier=tier)
    raw = _render(config)
    parts = [
        f"s{ordinal:03d}", f"q{query_count}", tier, dataset,
        identity["index_mode"], identity["engine"],
    ]
    if identity["engine"] == "bioseq":
        parts.extend([
            str(identity["bioseq_index_name"]),
            str(identity["requested_minimizer_algorithm"]),
            str(identity["requested_chaining_algorithm"]),
            str(identity["requested_base_alignment_algorithm"]),
        ])
    path = OUTPUT_DIRECTORY / (
        "bench_e2e_algorithm_matrix_deadline_v2_"
        + "_".join(_slug(part) for part in parts) + ".json")
    estimated = int(math.ceil((timed_seconds + FIXED_SECONDS) * contingency))
    if estimated >= timeout_seconds:
        raise GenerationError(
            f"predicted shard wall {estimated}s reaches timeout {timeout_seconds}s")
    outputs[path] = raw
    schedule.append({
        "ordinal": ordinal,
        "path": path.relative_to(ROOT).as_posix(),
        "config_sha256": _sha(raw),
        "stable_key": stable_key,
        "stable_key_sha256": hashlib.sha256(stable_key.encode()).hexdigest(),
        "identity": identity,
        "repetitions": REPETITIONS,
        "expected_records": REPETITIONS,
        "allowed_statuses": ["accepted", "rejected"]
            if identity["engine"] == "bioseq" else ["accepted"],
        "expected_disposition": _expected_disposition(identity),
        "metric_names": list(config["bench_e2e"]["matrix"]["metrics"]),
        "timed_estimate_seconds": round(timed_seconds, 6),
        "fixed_seconds": FIXED_SECONDS,
        "contingency_multiplier": contingency,
        "estimated_wall_seconds": estimated,
        "runtime_timeout_seconds": timeout_seconds,
    })


def _expected_outputs() -> tuple[dict[Path, bytes], bytes]:
    lineage = _validated_parent_lineage()
    source, source_raw = _load(base.SOURCE_CONFIG)
    base._validate_source(source)
    anchors = _validated_anchor_bindings()
    if tuple(source["params"]["bioseq_algorithm_matrix_no_index"]
             ["minimizer_algorithm"]["enum"]) != MINIMIZERS:
        raise GenerationError("GUC minimizer axis changed")

    fragment_q1 = anchors["q1_fragment"]["wall_sum_seconds"]
    fragment_q8 = max(
        anchors["q8_fragment_ksw2"]["wall_sum_seconds"],
        anchors["q8_fragment_block"]["wall_sum_seconds"])
    sequence_q1 = anchors["q1_sequence"]["wall_sum_seconds"]
    sequence_q8 = max(
        anchors["q8_sequence_ksw2"]["wall_sum_seconds"],
        anchors["q8_sequence_block"]["wall_sum_seconds"])
    fragment_q16 = _linear_estimate(fragment_q1, fragment_q8, FULL_QUERY_COUNT)
    fragment_q2 = _linear_estimate(fragment_q1, fragment_q8, OUTER_QUERY_COUNT)
    sequence_q2 = _linear_estimate(sequence_q1, sequence_q8, OUTER_QUERY_COUNT)

    outputs: dict[Path, bytes] = {}
    schedule: list[dict[str, Any]] = []

    # Tier A: the exact 5 x 4 x 2 algorithm Cartesian on one fastest shared
    # storage route. The two index policies/modes are covered pairwise in tier B.
    for minimizer, chaining, aligner in itertools.product(
            MINIMIZERS, CHAINING, BASE_ALIGNERS):
        _add_shard(
            outputs, schedule, source,
            tier="full_cartesian", query_count=FULL_QUERY_COUNT,
            dataset=FULL_DATASET, algorithm="bioseq_algorithm_matrix_no_index",
            parameters={
                "minimizer_algorithm": minimizer,
                "chaining_algorithm": chaining,
                "base_alignment_algorithm": aligner,
                "minimizer_index": "minimizers_per_fragment",
                "pipeline_mode": "task_parallel",
                "recompute_async_persistence": "off",
            },
            timed_seconds=2.0 * fragment_q16,
            contingency=1.25,
            timeout_seconds=NORMAL_TIMEOUT_SECONDS,
        )

    # Tier B: one supported Bioseq route and its same-mode minimap2 baseline per
    # outer dataset. Persisted MPS lives on M2/D4/k15; every k20 Bioseq route is
    # real no-index because the production persisted format does not admit k20.
    storage_pairs = {
        "M2_D4_K15_W10": ("just_index", "minimizers_per_sequence"),
        "M2_D4_K20_W15": ("no_index", "minimizers_per_sequence"),
        "M2_D5_K15_W10": ("no_index", "minimizers_per_sequence"),
        "M2_D5_K20_W15": ("no_index", "minimizers_per_fragment"),
        "M4_D4_K15_W10": ("no_index", "minimizers_per_fragment"),
        "M4_D4_K20_W15": ("no_index", "minimizers_per_sequence"),
        "M4_D5_K15_W10": ("no_index", "minimizers_per_sequence"),
        "M4_D5_K20_W15": ("no_index", "minimizers_per_fragment"),
    }
    if tuple(storage_pairs) != tuple(base.EXPECTED_DATASETS):
        raise GenerationError("compact storage mapping lost exact dataset order")
    for dataset in base.EXPECTED_DATASETS:
        mode, index_name = storage_pairs[dataset]
        algorithm = f"bioseq_algorithm_matrix_{mode}"
        parameters = {
            "minimizer_algorithm": "scalar",
            "chaining_algorithm": "scalar_minimap2",
            "base_alignment_algorithm": "ksw2",
            "minimizer_index": index_name,
            "pipeline_mode": "task_parallel",
        }
        if mode == "no_index":
            parameters["recompute_async_persistence"] = "off"
        data = source["test_cases"][dataset]["data"]
        policy_anchor = (
            fragment_q2 if index_name == "minimizers_per_fragment"
            else sequence_q2)
        multiplier = _outer_multiplier(data)
        _add_shard(
            outputs, schedule, source,
            tier="compact_outer", query_count=OUTER_QUERY_COUNT,
            dataset=dataset, algorithm=algorithm, parameters=parameters,
            timed_seconds=policy_anchor * multiplier,
            contingency=1.5,
            timeout_seconds=NORMAL_TIMEOUT_SECONDS,
        )
        baseline_algorithm = f"minimap2_{mode}"
        _add_shard(
            outputs, schedule, source,
            tier="compact_outer", query_count=OUTER_QUERY_COUNT,
            dataset=dataset, algorithm=baseline_algorithm,
            parameters={"threads": 0},
            timed_seconds=(
                anchors["q8_minimap2"]["wall_sum_seconds"] * multiplier),
            contingency=1.5,
            timeout_seconds=NORMAL_TIMEOUT_SECONDS,
        )

    # Tier C: query-scale anchors. Keep the v1 persisted fragment route at
    # ordinal 58 and append the newly authorized no-index fragment route at 59.
    for algorithm in ("minimap2_no_index", "minimap2_just_index"):
        _add_shard(
            outputs, schedule, source,
            tier="q1024_anchor", query_count=ANCHOR_QUERY_COUNT,
            dataset=FULL_DATASET, algorithm=algorithm,
            parameters={"threads": 0},
            timed_seconds=4.0 * anchors["q8_minimap2"]["wall_sum_seconds"],
            contingency=1.5,
            timeout_seconds=NORMAL_TIMEOUT_SECONDS,
        )
    fragment_q1024 = _linear_estimate(
        fragment_q1, fragment_q8, ANCHOR_QUERY_COUNT)
    _add_shard(
        outputs, schedule, source,
        tier="q1024_anchor", query_count=ANCHOR_QUERY_COUNT,
        dataset=FULL_DATASET,
        algorithm="bioseq_algorithm_matrix_just_index",
        parameters={
            "minimizer_algorithm": "scalar",
            "chaining_algorithm": "scalar_minimap2",
            "base_alignment_algorithm": "ksw2",
            "minimizer_index": "minimizers_per_fragment",
            "pipeline_mode": "task_parallel",
        },
        timed_seconds=fragment_q1024,
        contingency=1.2,
        timeout_seconds=EXTENDED_TIMEOUT_SECONDS,
    )
    _add_shard(
        outputs, schedule, source,
        tier="q1024_anchor", query_count=ANCHOR_QUERY_COUNT,
        dataset=FULL_DATASET,
        algorithm="bioseq_algorithm_matrix_no_index",
        parameters={
            "minimizer_algorithm": "scalar",
            "chaining_algorithm": "scalar_minimap2",
            "base_alignment_algorithm": "ksw2",
            "minimizer_index": "minimizers_per_fragment",
            "pipeline_mode": "task_parallel",
            "recompute_async_persistence": "off",
        },
        timed_seconds=fragment_q1024,
        contingency=1.2,
        timeout_seconds=EXTENDED_TIMEOUT_SECONDS,
    )

    total_seconds = sum(entry["estimated_wall_seconds"] for entry in schedule)
    expected_shards = (
        len(MINIMIZERS) * len(CHAINING) * len(BASE_ALIGNERS)
        + 2 * len(base.EXPECTED_DATASETS)
        + 4
    )
    if (len(schedule) != expected_shards
            or len(outputs) != expected_shards
            or total_seconds >= DEADLINE_SECONDS):
        raise GenerationError(
            f"campaign cardinality/deadline changed: shards={len(schedule)} "
            f"seconds={total_seconds}")
    if len({_stable_key(entry["identity"]) for entry in schedule}) != len(schedule):
        raise GenerationError("campaign has duplicate cell identity")

    sequence_q1024 = _linear_estimate(
        sequence_q1, sequence_q8, ANCHOR_QUERY_COUNT)
    source_bindings = {
        relative: _sha(ROOT / relative) for relative in SOURCE_BINDINGS}
    manifest: dict[str, Any] = {
        "schema": SCHEMA,
        "version": SCHEMA_VERSION,
        "lineage": lineage,
        "campaign": {
            "campaign_id": CAMPAIGN_ID,
            "status": "sealed_ready_for_sequential_run_only",
            "q1_membership": 0,
            "q1_is_estimate_reference_only": True,
            "repetitions_per_cell": REPETITIONS,
            "expected_shards": len(schedule),
            "expected_result_cells": len(schedule),
            "expected_raw_records": len(schedule) * REPETITIONS,
            "estimated_wall_seconds": total_seconds,
            "estimated_wall_days": round(total_seconds / 86_400.0, 6),
            "deadline_seconds": int(DEADLINE_SECONDS),
            "deadline_days": 7.0,
            "operational_margin_seconds": int(DEADLINE_SECONDS) - total_seconds,
            "campaign_digest_algorithm": (
                "sha256-canonical-json-without-campaign_digest"),
        },
        "source_bindings": source_bindings,
        "calibration_anchors": anchors,
        "runtime_contract": {
            "mode": "benchmark-e2e",
            "run_only_required": True,
            "exact_benchmark_filter": {"benchmarks": ["bench_e2e"]},
            "exact_benchmark_flags": ["--filter", "benchmark=bench_e2e"],
            "local_only": True,
            "prefilter_enabled": False,
            "prefilter_encoding": "absent_or_explicit_default_off",
            "cigar_projection": True,
            "mapq_projection": True,
            "pipeline_mode": "task_parallel",
            "ksw2_batched_excluded": True,
            "runtime_proof_counters_retained": True,
            "normal_timeout_seconds": NORMAL_TIMEOUT_SECONDS,
            "extended_timeout_seconds": EXTENDED_TIMEOUT_SECONDS,
            "extended_timeout_shard_count": sum(
                entry["runtime_timeout_seconds"] == EXTENDED_TIMEOUT_SECONDS
                for entry in schedule),
            "sequential_only": True,
        },
        "coverage": {
            "full_cartesian": {
                "query_count": FULL_QUERY_COUNT,
                "dataset": FULL_DATASET,
                "minimizer_algorithms": list(MINIMIZERS),
                "chaining_algorithms": list(CHAINING),
                "base_alignment_algorithms": list(BASE_ALIGNERS),
                "excluded_base_alignment_algorithms": ["ksw2_batched"],
                "index_mode": "no_index",
                "bioseq_index_name": "minimizers_per_fragment",
                "result_cells": 40,
                "q32_rejected_reason": (
                    "conservative campaign projection leaves insufficient operational "
                    "margin and its slowest shards approach the 4h ordinary guard"),
            },
            "compact_outer": {
                "query_count": OUTER_QUERY_COUNT,
                "datasets": list(base.EXPECTED_DATASETS),
                "target_size_gbp": [2, 4],
                "alphabets": ["dna4", "dna5"],
                "kmer_window": ["k15w10", "k20w15"],
                "index_modes": ["no_index", "just_index"],
                "bioseq_index_names": list(INDEX_NAMES),
                "bioseq_route": ["scalar", "scalar_minimap2", "ksw2"],
                "bioseq_cells": 8,
                "minimap2_cells": 8,
                "pairwise_not_factorial": True,
            },
            "q1024_anchors": {
                "query_count": ANCHOR_QUERY_COUNT,
                "minimap2_modes": ["no_index", "just_index"],
                "bioseq_route": {
                    "index_modes": ["just_index", "no_index"],
                    "bioseq_index_name": "minimizers_per_fragment",
                    "algorithms": ["scalar", "scalar_minimap2", "ksw2"],
                    "timeout_seconds": EXTENDED_TIMEOUT_SECONDS,
                },
                "result_cells": 4,
            },
        },
        "deadline_exclusions": [
            {
                "identity": "q1024_no_index_minimizers_per_sequence_scalar_scalar_ksw2",
                "estimated_five_repeat_seconds": round(sequence_q1024, 6),
                "reason": "measured q1/q8 scaling exceeds the complete campaign deadline",
                "classification": "deadline_infeasible_not_complete",
            },
            {
                "identity": "q1024_just_index_minimizers_per_sequence_scalar_scalar_ksw2",
                "estimated_five_repeat_seconds": round(sequence_q1024, 6),
                "reason": "sequence-policy q1024 estimate is infeasible even before margin",
                "classification": "deadline_infeasible_not_complete",
            },
        ],
        "publication_contract": {
            "classification": "production_all_results_candidate_rows",
            "q1_calibration": False,
            "merge_into_ALL_RESULTS_yaml": (
                "eligible_after_terminal_per_shard_audit"),
            "automatic_merge_during_run_only": False,
            "required_terminal_gate": (
                "five records, accepted or harness-proven rejection, exact parity, "
                "runtime proof counters, fallback comments, and sealed run receipt"),
            "merge_into_Q8_RESULTS_yaml": False,
            "existing_ledgers_unchanged_by_generation": True,
        },
        "deterministic_first_shard": {
            "path": schedule[0]["path"],
            "config_sha256": schedule[0]["config_sha256"],
            "filter": {"benchmarks": ["bench_e2e"]},
            "expected_wall_seconds": schedule[0]["estimated_wall_seconds"],
        },
        "schedule": schedule,
    }
    manifest["campaign"]["campaign_digest"] = CAMPAIGN_DIGEST
    return outputs, _render(manifest)


def _atomic_write(path: Path, raw: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent, prefix=f".{path.name}.", suffix=".tmp")
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(raw)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _check(outputs: dict[Path, bytes], manifest_raw: bytes) -> list[str]:
    errors: list[str] = []
    existing = set(OUTPUT_DIRECTORY.glob("*.json")) \
        if OUTPUT_DIRECTORY.exists() else set()
    if existing != set(outputs):
        errors.append("generated shard membership differs")
    for path, expected in outputs.items():
        if not path.is_file() or path.read_bytes() != expected:
            errors.append(f"stale shard: {path.relative_to(ROOT)}")
    if not MANIFEST_PATH.is_file() or MANIFEST_PATH.read_bytes() != manifest_raw:
        errors.append(f"stale manifest: {MANIFEST_PATH.relative_to(ROOT)}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--write", action="store_true")
    action.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        outputs, manifest_raw = _expected_outputs()
        if args.write:
            OUTPUT_DIRECTORY.mkdir(parents=True, exist_ok=True)
            unexpected = set(OUTPUT_DIRECTORY.glob("*.json")) - set(outputs)
            if unexpected:
                raise GenerationError(
                    "refusing to delete unexpected configs: "
                    + ", ".join(str(path) for path in sorted(unexpected)))
            for path, raw in outputs.items():
                _atomic_write(path, raw)
            _atomic_write(MANIFEST_PATH, manifest_raw)
        errors = _check(outputs, manifest_raw)
        if errors:
            raise GenerationError("; ".join(errors))
        manifest = json.loads(manifest_raw)
        print(
            f"deadline campaign current: {len(outputs)} shards, "
            f"{manifest['campaign']['estimated_wall_days']:.3f} days, "
            f"digest={manifest['campaign']['campaign_digest']}")
        return 0
    except (OSError, UnicodeError, ValueError, GenerationError) as exc:
        print(f"deadline campaign generation failed: {exc}", file=os.sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
