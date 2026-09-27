#!/usr/bin/env python3
"""Generate and verify an E2E algorithm-matrix shard campaign.

The canonical full campaign JSON remains the sealed source of the axes.  The
default invocation deterministically reproduces its 88 q1024 serial configs
and ownership contract byte-for-byte.  Explicit CLI options can derive a
separate query-count projection with collision-free output identity and
bounded no-index shards.  This tool never runs a benchmark.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import tempfile
from typing import Any


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SOURCE_CONFIG = REPOSITORY_ROOT / "bench/bench_e2e_algorithm_matrix_campaign.json"
SHARD_DIRECTORY = REPOSITORY_ROOT / "bench/e2e_algorithm_matrix_shards"
CONTRACT_PATH = REPOSITORY_ROOT / "bench/bench_e2e_algorithm_matrix_campaign_contract.json"
CANONICAL_SHARD_DIRECTORY = SHARD_DIRECTORY
CANONICAL_CONTRACT_PATH = CONTRACT_PATH
SINGLETON_PARTITION_PATH = (
    REPOSITORY_ROOT
    / "bench/bench_e2e_algorithm_matrix_q1024_singleton_partition_manifest.json"
)
SINGLETON_PARTITION_SCHEMA = (
    "bioseq.e2e.algorithm-matrix-q1024-singleton-partition"
)
SINGLETON_PARTITION_VERSION = 1
SINGLETON_FILENAME_PREFIX = (
    "bench_e2e_algorithm_matrix_campaign_shard_singleton"
)
OBSERVED_SINGLETON_PARTITION_SCHEMA = (
    "bioseq.e2e.algorithm-matrix-observed-rung-singleton-partition"
)
OBSERVED_SINGLETON_PARTITION_VERSION = 1
Q8_RESCUE_MANIFEST_PATH = (
    REPOSITORY_ROOT
    / "bench/bench_e2e_algorithm_matrix_q8_ordinal1_rescue_manifest.json"
)
HISTORICAL_Q8_CONTRACT_PATH = (
    REPOSITORY_ROOT
    / "bench/bench_e2e_algorithm_matrix_q8_campaign_manifest.json"
)
HISTORICAL_Q8_CONFIG_PATH = (
    REPOSITORY_ROOT / "bench/bench_e2e_algorithm_matrix_q8_campaign.json"
)
HISTORICAL_Q8_SHARD_DIRECTORY = (
    REPOSITORY_ROOT / "bench/e2e_algorithm_matrix_q8_shards"
)
HISTORICAL_DEADLINE_V2_MANIFEST_PATH = (
    REPOSITORY_ROOT
    / "bench/bench_e2e_algorithm_matrix_deadline_v2_manifest.json"
)
HISTORICAL_DEADLINE_V2_SHARD_DIRECTORY = (
    REPOSITORY_ROOT / "bench/e2e_algorithm_matrix_deadline_v2_shards"
)
CAMPAIGN_ID = "bioseq-e2e-algorithm-matrix-local-v1"
CANONICAL_CAMPAIGN_ID = CAMPAIGN_ID
OUTPUT_CONFIG = SOURCE_CONFIG
PROOF_PATH: Path | None = None
SHARD_FILENAME_PREFIX = "bench_e2e_algorithm_matrix_campaign"
QUERY_FRAGMENT_COUNT = 1024
NO_INDEX_CELLS_PER_SHARD = 80
RUNTIME_HARD_TIMEOUT_SECONDS = 14_400
EXPECTED_CAMPAIGN_DIGEST = (
    "011fbb1ef33a214ab80d3568c2341e8d0f8311f82e21a5955bdcb93b3c63b979")
ACTIVE_EXPECTED_CAMPAIGN_DIGEST: str | None = EXPECTED_CAMPAIGN_DIGEST
CAMPAIGN_SCHEMA = "bioseq.e2e.algorithm-matrix-campaign"
CAMPAIGN_VERSION = 1
OBSERVED_RUNG_CAMPAIGN_VERSION = 2
LEGACY_RESULT_STATUS_CONTRACT = "legacy_expected_rejections_v1"
OBSERVED_RESULT_STATUS_CONTRACT = "observed_status_v1"
RESULT_STATUS_CONTRACT = LEGACY_RESULT_STATUS_CONTRACT
EXECUTION_CONTRACT_ID: str | None = None
RUNG_QUERY_COUNTS = (2, 8, 16, 32, 64, 128, 256, 512, 1024)
REPETITIONS_PER_CELL = 5
EXPECTED_RESULT_CELLS = 1296
EXPECTED_SHARDS = 88
EXPECTED_ALL_REJECTED_SHARDS = 64
EXPECTED_ALL_REJECTED_CELLS = 512
EXPECTED_ALL_REJECTED_RECORDS = 2560
EXPECTED_MIXED_PERSISTED_SHARDS = 16
EXPECTED_CANONICAL_QUERY_COUNT = 1024
BIOSEQ_ALGORITHMS = {
    "bioseq_algorithm_matrix_no_index": "no_index",
    "bioseq_algorithm_matrix_just_index": "just_index",
}
BASELINE_ALGORITHMS = {
    "minimap2_no_index": "no_index",
    "minimap2_just_index": "just_index",
}
STABLE_KEY_FIELDS = (
    "campaign_id",
    "dataset",
    "alphabet",
    "target_size_gbp",
    "kmer_size",
    "window_size",
    "index_mode",
    "engine",
    "bioseq_index_name",
    "requested_minimizer_algorithm",
    "requested_chaining_algorithm",
    "requested_base_alignment_algorithm",
)
EXPECTED_DATASETS = (
    "M2_D4_K15_W10",
    "M2_D4_K20_W15",
    "M2_D5_K15_W10",
    "M2_D5_K20_W15",
    "M4_D4_K15_W10",
    "M4_D4_K20_W15",
    "M4_D5_K15_W10",
    "M4_D5_K20_W15",
)
EXPECTED_MINIMIZERS = (
    "scalar",
    "simd_minimap2_reference",
    "two_stack_nthash",
    "vulkan_minimap_integer",
    "bounded_nthash_simd",
)
EXPECTED_INDEXES = (
    "minimizers_per_fragment",
    "minimizers_per_sequence",
)
FULL_DECLARED_METRIC_NAMES = (
    "wall_clock_ns",
    "region_total_ns",
    "tuple_accepted",
    "match_count",
    "expected_match_count",
    "correct_match_count",
    "exact_primary_count_accepted",
    "correct_cigar_count",
    "cigar_replay_count",
    "mapq_result_count",
    "pipeline_validation_accepted",
    "stage_engine_matrix_valid",
    "backend_declines",
    "backend_fallbacks",
    "prefilter_activity_count",
    "measured_region_ns_manual",
    "cigar_or_mapq_interval_union_ns",
    "outer_e2e_minus_cigar_or_mapq_union_ns",
    "benchmark_e2e_interval_schema_revision",
    "benchmark_e2e_interval_available",
    "benchmark_e2e_interval_count",
    "benchmark_e2e_cigar_interval_count",
    "benchmark_e2e_mapq_interval_count",
    "benchmark_e2e_interval_finalization_count",
    "minimap2_receipt_available",
    "minimap2_cigar_interval_count",
    "minimap2_mapq_interval_count",
    "downstream_active_wall_ns",
    "downstream_active_wall_available",
    "downstream_work_ns",
    "mapq_execution_available",
    "mapq_executed",
)
# A canonical all-rejected shard emits only these status and correctness-envelope
# diagnostics.  This covers exact untimed setup rejections and exact measured-SQL
# rejections alike; no accepted-only interval/timing/receipt key is declared merely
# to placate the framework.
ALL_REJECTED_METRIC_NAMES = FULL_DECLARED_METRIC_NAMES[:15]
MINIMAP2_RECEIPT_METRIC_NAMES = (
    "minimap2_receipt_available",
    "minimap2_cigar_interval_count",
    "minimap2_mapq_interval_count",
)
MIXED_PERSISTED_METRIC_NAMES = tuple(
    name for name in FULL_DECLARED_METRIC_NAMES
    if name not in MINIMAP2_RECEIPT_METRIC_NAMES
)
ALL_CAMPAIGN_ALGORITHMS = tuple(BIOSEQ_ALGORITHMS) + tuple(BASELINE_ALGORITHMS)
BIOSEQ_CAMPAIGN_ALGORITHMS = tuple(BIOSEQ_ALGORITHMS)
MINIMAP2_CAMPAIGN_ALGORITHMS = tuple(BASELINE_ALGORITHMS)
BIOSEQ_ONLY_EVALUATION_METRICS = {
    "benchmark_e2e_interval_schema_revision",
    "benchmark_e2e_interval_available",
    "benchmark_e2e_interval_count",
    "benchmark_e2e_cigar_interval_count",
    "benchmark_e2e_mapq_interval_count",
    "benchmark_e2e_interval_finalization_count",
    "downstream_active_wall_ns",
    "downstream_active_wall_available",
    "downstream_work_ns",
    "mapq_execution_available",
    "mapq_executed",
}
MINIMAP2_BASELINE_METRIC_NAMES = tuple(
    name for name in FULL_DECLARED_METRIC_NAMES
    if name not in BIOSEQ_ONLY_EVALUATION_METRICS
)


class GenerationError(RuntimeError):
    """Raised when the source campaign or generated tree is inconsistent."""


def _observed_rung_execution_contract_id(query_count: int) -> str:
    """Return the collision-free execution identity for one observed rung."""
    return f"bioseq-e2e-algorithm-matrix-local-q{query_count}-observed-v1"


def _observes_result_status() -> bool:
    """Whether generated entries must derive terminal status from their reports."""
    return RESULT_STATUS_CONTRACT == OBSERVED_RESULT_STATUS_CONTRACT


def _allowed_cell_statuses(engine: str) -> list[str]:
    """Return publishable aggregate statuses for one engine in this campaign."""
    if engine == "minimap2" or _observes_result_status():
        return ["accepted"]
    if engine == "bioseq":
        return ["accepted", "rejected"]
    raise GenerationError(f"unknown campaign engine {engine!r}")


def _publication_status_contract() -> dict[str, Any]:
    """Return the closed status schema for the selected campaign identity."""
    if _observes_result_status():
        return {
            "status_source": "observed",
            "minimap2": ["accepted"],
            "bioseq": ["accepted"],
            "rejected_dispositions": ["rejected_setup", "rejected_sql"],
            "nonpublishable_statuses": ["rejected", "invalid", "pending"],
        }
    return {
        "minimap2": ["accepted"],
        "bioseq": ["accepted", "rejected"],
        "rejected_dispositions": ["rejected_setup", "rejected_sql"],
        "nonpublishable_statuses": ["invalid", "pending"],
    }


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise GenerationError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _read_json(path: Path) -> tuple[dict[str, Any], bytes]:
    try:
        raw = path.read_bytes()
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=_strict_object)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise GenerationError(f"cannot read strict JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise GenerationError(f"{path} must contain one JSON object")
    return value, raw


def _canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=True,
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def _render(value: Any) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=True, allow_nan=False)
            + "\n").encode("utf-8")


def _sha256(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _repository_output_path(value: str, label: str) -> Path:
    path = Path(value)
    if (not value or path.is_absolute() or path.as_posix() != value
            or any(part in {"", ".", ".."} for part in path.parts)):
        raise GenerationError(
            f"{label} must be one canonical repository-relative path")
    return REPOSITORY_ROOT / path


def _derive_campaign_config(
        canonical: dict[str, Any]) -> tuple[dict[str, Any], bytes]:
    if (OUTPUT_CONFIG == SOURCE_CONFIG
            and QUERY_FRAGMENT_COUNT == EXPECTED_CANONICAL_QUERY_COUNT
            and CAMPAIGN_ID == CANONICAL_CAMPAIGN_ID):
        _, raw = _read_json(SOURCE_CONFIG)
        return canonical, raw

    projected = copy.deepcopy(canonical)
    for dataset in EXPECTED_DATASETS:
        test_case = projected["test_cases"][dataset]
        data = test_case["data"]
        if data.get("fragment_count") != EXPECTED_CANONICAL_QUERY_COUNT:
            raise GenerationError(
                f"{dataset} canonical fragment_count is not 1024")
        data["fragment_count"] = QUERY_FRAGMENT_COUNT
        test_case["description"] = (
            f"{data['reference_length'] // 1_000_000_000} Gbp "
            f"{data['alphabet']}, k{data['kmer_size']}/w{data['window_size']}, "
            f"{QUERY_FRAGMENT_COUNT} deterministic fixed-seed-prefix queries")
    projected["bench_e2e"]["tags"] = [
        "e2e", "algorithm-matrix", f"q{QUERY_FRAGMENT_COUNT}-campaign"]
    projected["bench_e2e"]["description"] = (
        f"Five-repeat q{QUERY_FRAGMENT_COUNT} fixed-seed prefix of the sealed "
        "2/4 Gbp dna4/dna5 matrix with CIGAR+MAPQ, both k/w values, both "
        "persisted index policies, cold persisted/transient modes, and "
        "minimap2 parity baselines.")
    return projected, _render(projected)


def _validate_query_projection(
        canonical: dict[str, Any], projected: dict[str, Any]) -> None:
    if QUERY_FRAGMENT_COUNT < 1:
        raise GenerationError("fragment_count must be positive")
    for dataset in EXPECTED_DATASETS:
        canonical_data = copy.deepcopy(
            canonical["test_cases"][dataset]["data"])
        projected_data = copy.deepcopy(
            projected["test_cases"][dataset]["data"])
        if canonical_data.pop("fragment_count", None) \
                != EXPECTED_CANONICAL_QUERY_COUNT:
            raise GenerationError(
                f"{dataset} canonical fragment_count changed")
        if projected_data.pop("fragment_count", None) != QUERY_FRAGMENT_COUNT:
            raise GenerationError(
                f"{dataset} projected fragment_count changed")
        if projected_data != canonical_data:
            raise GenerationError(
                f"{dataset} query-generation data changed beyond fragment_count")


def _stable_key(identity: dict[str, Any]) -> str:
    if tuple(identity) != STABLE_KEY_FIELDS:
        raise GenerationError("stable identity field order changed")
    return "bioseq-e2e-cell-v2:" + json.dumps(
        list(identity.values()), ensure_ascii=True, separators=(",", ":"))


def _expanded_tuples(config: dict[str, Any]) -> list[tuple[str, dict[str, Any]]]:
    params = config["params"]
    over = config["bench_e2e"]["matrix"]["over"]
    result: list[tuple[str, dict[str, Any]]] = []
    # The C++ framework owns std::map containers for both levels, so its
    # expansion order is lexicographic even when JSON member order differs.
    for algorithm in sorted(over):
        declared = over[algorithm]
        partial: list[dict[str, Any]] = [{}]
        for parameter in sorted(declared):
            next_partial: list[dict[str, Any]] = []
            for accumulated in partial:
                for value in declared[parameter]:
                    expanded = dict(accumulated)
                    expanded[parameter] = value
                    next_partial.append(expanded)
            partial = next_partial
        schema = params[algorithm]
        for expanded in partial:
            for parameter, declaration in schema.items():
                if parameter not in expanded:
                    if "default" not in declaration:
                        raise GenerationError(
                            f"{algorithm}.{parameter} has no value or default")
                    expanded[parameter] = declaration["default"]
            result.append((algorithm, dict(sorted(expanded.items()))))
    return result


def _config_cells(config: dict[str, Any]) -> list[dict[str, Any]]:
    matrix = config["bench_e2e"]["matrix"]
    tuples = _expanded_tuples(config)
    cells: list[dict[str, Any]] = []
    for test_case_index, (test_case, repeat_count) in enumerate(matrix["for"].items()):
        if repeat_count != REPETITIONS_PER_CELL:
            raise GenerationError(
                f"{test_case} requests {repeat_count}, expected {REPETITIONS_PER_CELL}")
        data = config["test_cases"][test_case]["data"]
        target_size_bp = data["reference_length"]
        if (isinstance(target_size_bp, bool) or not isinstance(target_size_bp, int)
                or target_size_bp <= 0 or target_size_bp % 1_000_000_000 != 0):
            raise GenerationError(f"{test_case} has a non-canonical target size")
        for tuple_index, (algorithm, parameters) in enumerate(tuples):
            if algorithm in BIOSEQ_ALGORITHMS:
                engine = "bioseq"
                mode = BIOSEQ_ALGORITHMS[algorithm]
                index_name = parameters["minimizer_index"]
                minimizer = parameters["minimizer_algorithm"]
                chaining = parameters["chaining_algorithm"]
                alignment = parameters["base_alignment_algorithm"]
                allowed_statuses = _allowed_cell_statuses(engine)
            elif algorithm in BASELINE_ALGORITHMS:
                engine = "minimap2"
                mode = BASELINE_ALGORITHMS[algorithm]
                index_name = None
                minimizer = None
                chaining = None
                alignment = None
                allowed_statuses = _allowed_cell_statuses(engine)
            else:
                raise GenerationError(f"unexpected campaign algorithm {algorithm!r}")
            identity = {
                "campaign_id": CAMPAIGN_ID,
                "dataset": data["size_label"],
                "alphabet": data["alphabet"],
                "target_size_gbp": target_size_bp // 1_000_000_000,
                "kmer_size": data["kmer_size"],
                "window_size": data["window_size"],
                "index_mode": mode,
                "engine": engine,
                "bioseq_index_name": index_name,
                "requested_minimizer_algorithm": minimizer,
                "requested_chaining_algorithm": chaining,
                "requested_base_alignment_algorithm": alignment,
            }
            cells.append({
                "stable_key": _stable_key(identity),
                "identity": identity,
                "test_case": test_case,
                "test_case_index": test_case_index,
                "algorithm": algorithm,
                "tuple_index": tuple_index,
                "params": parameters,
                "allowed_statuses": allowed_statuses,
            })
    keys = [cell["stable_key"] for cell in cells]
    if len(keys) != len(set(keys)):
        raise GenerationError("one config expands the same stable key more than once")
    return cells


def _canonical_cell_rejection_disposition(cell: dict[str, Any]) -> str | None:
    """Return a legacy campaign-proven rejection for one persisted cell."""
    if _observes_result_status():
        return None
    identity = cell["identity"]
    if (identity["engine"] != "bioseq"
            or identity["index_mode"] != "just_index"):
        return None

    profile = (identity["kmer_size"], identity["window_size"])
    if profile == (20, 15):
        # Every persisted publisher is specialized to K15/W10 and rejects this
        # profile before allocating or publishing a generation.
        return "rejected_setup"
    if profile != (15, 10):
        return None
    if identity["alphabet"] == "dna5":
        # Grouped-SoA has no canonical DNA5 profile; bounded ntHash preserves
        # its more specific unsupported_alphabet setup diagnostic.
        return "rejected_setup"
    if (identity["alphabet"] == "dna4"
            and identity["requested_minimizer_algorithm"]
                == "vulkan_minimap_integer"):
        # The canonical projection requests MAPQ. Runtime therefore rejects
        # every Vulkan-selected tuple at the complete-all-GPU selector, including
        # Vulkan/Vulkan/Block; the later persisted-CIGAR gate is unreachable.
        return "rejected_sql"
    return None


def _expected_all_records_disposition(config: dict[str, Any]) -> str | None:
    """Return one exact disposition only when every config-owned cell shares it."""
    cells = _config_cells(config)
    if not cells:
        return None
    dispositions = {
        _canonical_cell_rejection_disposition(cell) for cell in cells
    }
    if None in dispositions or len(dispositions) != 1:
        return None
    return dispositions.pop()


def _nonblocking_rule_names(config: dict[str, Any]) -> list[str]:
    rules = config["bench_e2e"]["matrix"]["metrics"]["match_count"]["rules"]
    names: list[str] = []
    for rule in rules:
        if (not isinstance(rule, list) or len(rule) != 4
                or not isinstance(rule[0], str)
                or not isinstance(rule[2], dict)
                or rule[2].get("blocking") is not False):
            raise GenerationError(
                "campaign match-count rules must be explicit nonblocking diagnostics")
        names.append(rule[0])
    if len(names) != len(set(names)):
        raise GenerationError("campaign match-count rule names must be unique")
    return names


def _validate_metric_applicability(config: dict[str, Any]) -> None:
    """Require exact per-accepted-cell applicability for campaign evaluations."""
    metrics = config["bench_e2e"]["matrix"]["metrics"]
    for name, specification in metrics.items():
        if specification.get("type") != "evaluation":
            if "applicable_algorithms" in specification:
                raise GenerationError(
                    f"non-evaluation metric {name!r} declares applicability")
            continue
        expected = (
            MINIMAP2_CAMPAIGN_ALGORITHMS
            if name in MINIMAP2_RECEIPT_METRIC_NAMES
            else BIOSEQ_CAMPAIGN_ALGORITHMS
            if name in BIOSEQ_ONLY_EVALUATION_METRICS
            else ALL_CAMPAIGN_ALGORITHMS
        )
        if tuple(specification.get("applicable_algorithms", ())) != expected:
            raise GenerationError(
                f"evaluation metric {name!r} has incorrect cell applicability")


def _restrict_metric_applicability(config: dict[str, Any]) -> None:
    """Intersect source applicability with one generated shard's rows."""
    algorithms = set(config["bench_e2e"]["matrix"]["over"])
    for name, specification in config["bench_e2e"]["matrix"]["metrics"].items():
        applicable = specification.get("applicable_algorithms")
        if applicable is None:
            continue
        selected = [algorithm for algorithm in applicable if algorithm in algorithms]
        if not selected:
            raise GenerationError(
                f"retained metric {name!r} applies to no row in generated shard")
        specification["applicable_algorithms"] = selected


def _validate_source(config: dict[str, Any]) -> None:
    if tuple(config.get("test_cases", {})) != EXPECTED_DATASETS:
        raise GenerationError("full campaign must contain the canonical eight datasets")
    benchmark = config.get("bench_e2e")
    if not isinstance(benchmark, dict):
        raise GenerationError("full campaign is missing bench_e2e")
    if benchmark.get("fast") is not False:
        raise GenerationError("campaign must remain a non-fast benchmark")
    if benchmark.get("concurrency") != {"bench_lock": True, "concurrency": False}:
        raise GenerationError("campaign must remain serial behind the bench lock")
    if benchmark.get("iteration_order") != {"mode": "interleaved_seeded", "seed": 333}:
        raise GenerationError("campaign must retain the canonical seeded schedule")
    matrix = benchmark.get("matrix", {})
    if tuple(matrix.get("for", {})) != EXPECTED_DATASETS \
            or set(matrix["for"].values()) != {REPETITIONS_PER_CELL}:
        raise GenerationError("campaign repeat ownership changed")
    over = matrix.get("over", {})
    for algorithm in BIOSEQ_ALGORITHMS:
        axes = over.get(algorithm, {})
        if tuple(axes.get("minimizer_algorithm", ())) != EXPECTED_MINIMIZERS:
            raise GenerationError(f"{algorithm} minimizer axis changed")
        if tuple(axes.get("minimizer_index", ())) != EXPECTED_INDEXES:
            raise GenerationError(f"{algorithm} persisted-index axis changed")
    if set(over) != set(BIOSEQ_ALGORITHMS) | set(BASELINE_ALGORITHMS):
        raise GenerationError("campaign algorithm membership changed")
    if config.get("regression_rules") != []:
        raise GenerationError("campaign regression rules must remain empty")
    if tuple(matrix.get("metrics", {})) != FULL_DECLARED_METRIC_NAMES:
        raise GenerationError("full campaign metric declarations changed")
    _validate_metric_applicability(config)
    if any(
        config["test_cases"][dataset]["data"].get("cigar_projection") is not True
        or config["test_cases"][dataset]["data"].get("mapq_projection") is not True
        for dataset in EXPECTED_DATASETS
    ):
        raise GenerationError(
            "canonical rejection semantics require CIGAR+MAPQ projection")
    if len(_nonblocking_rule_names(config)) != 2 * len(EXPECTED_DATASETS):
        raise GenerationError("full campaign must own 16 nonblocking parity rules")
    if len(_config_cells(config)) != EXPECTED_RESULT_CELLS:
        raise GenerationError("full campaign no longer expands to 1296 cells")


def _base_shard(source: dict[str, Any], dataset: str) -> dict[str, Any]:
    shard = copy.deepcopy(source)
    shard["test_cases"] = {dataset: copy.deepcopy(source["test_cases"][dataset])}
    benchmark = shard["bench_e2e"]
    benchmark["tags"] = ["e2e", "algorithm-matrix", "campaign-shard"]
    if QUERY_FRAGMENT_COUNT != EXPECTED_CANONICAL_QUERY_COUNT:
        benchmark["tags"].append(f"q{QUERY_FRAGMENT_COUNT}")
    benchmark["matrix"]["for"] = {dataset: REPETITIONS_PER_CELL}
    shard["regression_rules"] = []
    return shard


def _no_index_baseline_shard(source: dict[str, Any], dataset: str) -> dict[str, Any]:
    shard = _base_shard(source, dataset)
    included = (
        "bioseq_algorithm_matrix_no_index",
        "minimap2_no_index",
        "minimap2_just_index",
    )
    shard["params"] = {
        key: copy.deepcopy(source["params"][key]) for key in included
    }
    shard["bench_e2e"]["description"] = (
        f"Canonical serial {dataset} cold no-index matrix plus both minimap2 baselines."
    )
    shard["bench_e2e"]["matrix"]["over"] = {
        key: copy.deepcopy(source["bench_e2e"]["matrix"]["over"][key])
        for key in included
    }
    source_rules = source["bench_e2e"]["matrix"]["metrics"]["match_count"]["rules"]
    selected_rules = [
        copy.deepcopy(rule) for rule in source_rules
        if rule[1].get("test_case") == dataset
        and rule[1].get("key") == "bioseq_algorithm_matrix_no_index"
    ]
    if len(selected_rules) != 1:
        raise GenerationError(f"{dataset} has no exact no-index parity rule")
    shard["bench_e2e"]["matrix"]["metrics"]["match_count"]["rules"] = selected_rules
    _restrict_metric_applicability(shard)
    return shard


def _no_index_micro_shard(
        source: dict[str, Any],
        dataset: str,
        minimizer: str,
        chaining: str) -> dict[str, Any]:
    shard = _base_shard(source, dataset)
    algorithm = "bioseq_algorithm_matrix_no_index"
    shard["params"] = {algorithm: copy.deepcopy(source["params"][algorithm])}
    axes = copy.deepcopy(source["bench_e2e"]["matrix"]["over"][algorithm])
    axes["minimizer_algorithm"] = [minimizer]
    axes["chaining_algorithm"] = [chaining]
    shard["bench_e2e"]["description"] = (
        f"q{QUERY_FRAGMENT_COUNT} serial {dataset} no-index micro-shard for "
        f"{minimizer}/{chaining}; both base aligners and index policies.")
    shard["bench_e2e"]["matrix"]["over"] = {algorithm: axes}
    shard["bench_e2e"]["matrix"]["metrics"]["match_count"]["rules"] = []
    metrics = shard["bench_e2e"]["matrix"]["metrics"]
    shard["bench_e2e"]["matrix"]["metrics"] = {
        name: metrics[name] for name in MIXED_PERSISTED_METRIC_NAMES
    }
    _restrict_metric_applicability(shard)
    if len(_config_cells(shard)) > NO_INDEX_CELLS_PER_SHARD:
        raise GenerationError("no-index micro-shard exceeds configured cell cap")
    return shard


def _minimap2_baseline_shard(
        source: dict[str, Any], dataset: str, algorithm: str) -> dict[str, Any]:
    if algorithm not in BASELINE_ALGORITHMS:
        raise GenerationError(f"unexpected minimap2 baseline {algorithm!r}")
    shard = _base_shard(source, dataset)
    shard["params"] = {algorithm: copy.deepcopy(source["params"][algorithm])}
    shard["bench_e2e"]["description"] = (
        f"q{QUERY_FRAGMENT_COUNT} serial isolated {dataset} {algorithm} baseline.")
    shard["bench_e2e"]["matrix"]["over"] = {
        algorithm: copy.deepcopy(
            source["bench_e2e"]["matrix"]["over"][algorithm])
    }
    shard["bench_e2e"]["matrix"]["metrics"]["match_count"]["rules"] = []
    metrics = shard["bench_e2e"]["matrix"]["metrics"]
    retained = tuple(
        name for name in FULL_DECLARED_METRIC_NAMES
        if name not in BIOSEQ_ONLY_EVALUATION_METRICS)
    shard["bench_e2e"]["matrix"]["metrics"] = {
        name: metrics[name] for name in retained
    }
    _restrict_metric_applicability(shard)
    if len(_config_cells(shard)) != 1:
        raise GenerationError("isolated minimap2 shard must own exactly one cell")
    return shard


def _persisted_shard(
        source: dict[str, Any],
        dataset: str,
        minimizer: str,
        index_name: str) -> dict[str, Any]:
    shard = _base_shard(source, dataset)
    algorithm = "bioseq_algorithm_matrix_just_index"
    shard["params"] = {algorithm: copy.deepcopy(source["params"][algorithm])}
    axes = copy.deepcopy(source["bench_e2e"]["matrix"]["over"][algorithm])
    axes["minimizer_algorithm"] = [minimizer]
    axes["minimizer_index"] = [index_name]
    shard["bench_e2e"]["description"] = (
        f"Canonical serial {dataset} persisted-index shard for "
        f"{minimizer}/{index_name}."
    )
    shard["bench_e2e"]["matrix"]["over"] = {algorithm: axes}
    # Its corresponding minimap2 baseline is owned exactly once by the
    # dataset's no-index+baseline shard and joined later by the merger.
    shard["bench_e2e"]["matrix"]["metrics"]["match_count"]["rules"] = []
    if _expected_all_records_disposition(shard) is not None:
        metrics = shard["bench_e2e"]["matrix"]["metrics"]
        shard["bench_e2e"]["matrix"]["metrics"] = {
            name: metrics[name] for name in ALL_REJECTED_METRIC_NAMES
        }
    else:
        metrics = shard["bench_e2e"]["matrix"]["metrics"]
        shard["bench_e2e"]["matrix"]["metrics"] = {
            name: metrics[name] for name in MIXED_PERSISTED_METRIC_NAMES
        }
    _restrict_metric_applicability(shard)
    return shard


def _slug(value: str) -> str:
    return value.lower().replace("minimizers_per_", "mp")


def _config_entry(
        path: Path,
        raw: bytes,
        config: dict[str, Any],
        *,
        role: str,
        shard_kind: str | None) -> dict[str, Any]:
    cells = _config_cells(config)
    keys = sorted(cell["stable_key"] for cell in cells)
    expected_disposition = _expected_all_records_disposition(config)
    return {
        "path": str(path.relative_to(REPOSITORY_ROOT)),
        "config_sha256": _sha256(raw),
        "role": role,
        "shard_kind": shard_kind,
        "expected_result_cells": len(cells),
        "expected_raw_records": len(cells) * REPETITIONS_PER_CELL,
        "expected_stable_keys_sha256": _sha256("\n".join(keys).encode("utf-8")),
        "allowed_nonblocking_rule_names": _nonblocking_rule_names(config),
        "declared_metric_names": list(
            config["bench_e2e"]["matrix"]["metrics"]),
        "expected_all_records_disposition": expected_disposition,
        "cells": cells,
    }


def _singleton_config(
        parent: dict[str, Any], canonical_cell: dict[str, Any], *,
        observed_query_count: int | None = None) -> dict[str, Any]:
    """Narrow one canonical parent cell without changing its runtime contract."""
    child = copy.deepcopy(parent)
    test_case = canonical_cell["test_case"]
    algorithm = canonical_cell["algorithm"]
    child["test_cases"] = {
        test_case: copy.deepcopy(parent["test_cases"][test_case])
    }
    child["params"] = {
        algorithm: copy.deepcopy(parent["params"][algorithm])
    }
    parent_axes = parent["bench_e2e"]["matrix"]["over"][algorithm]
    child_axes: dict[str, list[Any]] = {}
    for parameter in parent_axes:
        if parameter not in canonical_cell["params"]:
            raise GenerationError(
                f"canonical cell omits declared axis {algorithm}.{parameter}")
        value = canonical_cell["params"][parameter]
        if value not in parent_axes[parameter]:
            raise GenerationError(
                f"canonical cell value escaped {algorithm}.{parameter}")
        child_axes[parameter] = [value]
    benchmark = child["bench_e2e"]
    stable_key_sha256 = hashlib.sha256(
        canonical_cell["stable_key"].encode("utf-8")).hexdigest()
    if observed_query_count is None:
        benchmark["tags"] = [
            "e2e", "algorithm-matrix", "campaign-singleton", "q1024"]
        benchmark["description"] = (
            "Sealed q1024 singleton execution descriptor for canonical cell "
            f"{stable_key_sha256}."
        )
    else:
        if observed_query_count not in RUNG_QUERY_COUNTS:
            raise GenerationError(
                "observed singleton query count escaped the exact q ladder")
        benchmark["tags"] = [
            "e2e", "algorithm-matrix", "campaign-singleton",
            f"q{observed_query_count}-observed",
        ]
        benchmark["description"] = (
            f"Sealed observed q{observed_query_count} singleton execution "
            f"descriptor for canonical cell {stable_key_sha256}."
        )
    benchmark["matrix"]["for"] = {test_case: REPETITIONS_PER_CELL}
    benchmark["matrix"]["over"] = {algorithm: child_axes}
    disposition = _canonical_cell_rejection_disposition(canonical_cell)
    if observed_query_count is None:
        benchmark["matrix"]["metrics"]["match_count"]["rules"] = []
        metric_names = (
            ALL_REJECTED_METRIC_NAMES
            if disposition is not None else
            MINIMAP2_BASELINE_METRIC_NAMES
            if canonical_cell["identity"]["engine"] == "minimap2" else
            MIXED_PERSISTED_METRIC_NAMES
        )
        metrics = benchmark["matrix"]["metrics"]
        benchmark["matrix"]["metrics"] = {
            name: metrics[name] for name in metric_names
        }
    else:
        expected_metric_names = (
            MINIMAP2_BASELINE_METRIC_NAMES
            if canonical_cell["identity"]["engine"] == "minimap2" else
            MIXED_PERSISTED_METRIC_NAMES
        )
        test_data = child["test_cases"][test_case]["data"]
        if (disposition is not None
                or canonical_cell.get("allowed_statuses") != ["accepted"]
                or list(benchmark["matrix"]["metrics"])
                    != list(expected_metric_names)
                or test_data.get("fragment_count") != observed_query_count
                or test_data.get("cigar_projection") is not True
                or test_data.get("mapq_projection") is not True
                or test_data.get("paired_minimap2_oracle", False) is not False
                or (canonical_cell["identity"]["engine"] == "bioseq"
                    and canonical_cell["params"].get("pipeline_mode")
                        != "task_parallel")
                or canonical_cell["params"].get(
                    "base_alignment_algorithm") == "ksw2_batched"):
            raise GenerationError(
                "observed singleton changed accepted-only status, metrics, "
                "projection, or execution policy")
    child["regression_rules"] = []
    _restrict_metric_applicability(child)
    execution_cells = _config_cells(child)
    if len(execution_cells) != 1:
        raise GenerationError("singleton config did not expand to exactly one cell")
    execution_cell = execution_cells[0]
    if (execution_cell["stable_key"] != canonical_cell["stable_key"]
            or execution_cell["identity"] != canonical_cell["identity"]
            or execution_cell["test_case_index"] != 0
            or execution_cell["tuple_index"] != 0):
        raise GenerationError(
            "singleton execution descriptor changed canonical ownership")
    return child


def _observed_singleton_locations(
        query_count: int) -> tuple[Path, Path, str, str]:
    """Return collision-free additive singleton locations for one q rung."""
    if query_count not in RUNG_QUERY_COUNTS:
        raise GenerationError("observed singleton query count is outside the ladder")
    directory = (
        REPOSITORY_ROOT
        / f"bench/e2e_algorithm_matrix_q{query_count}_observed_singletons")
    manifest = (
        REPOSITORY_ROOT
        / f"bench/bench_e2e_algorithm_matrix_q{query_count}_"
          "observed_singleton_partition_manifest.json")
    prefix = f"bench_e2e_algorithm_matrix_q{query_count}_observed_singleton"
    pattern = (
        rf"bench/e2e_algorithm_matrix_q{query_count}_observed_singletons/"
        rf"{prefix}_[0-9]{{4}}_[0-9a-f]{{16}}\.json")
    return directory, manifest, prefix, pattern


def _expected_observed_singleton_partition_outputs(
        canonical_generated: dict[Path, bytes],
        canonical_contract: dict[str, Any],
        canonical_contract_raw: bytes,
        canonical_config_raw: bytes,
) -> tuple[dict[Path, bytes], bytes, Path, Path]:
    """Return the exact accepted-only singleton partition for one observed rung."""
    campaign = canonical_contract.get("campaign")
    source = canonical_contract.get("source")
    rung = canonical_contract.get("rung")
    ownership = canonical_contract.get("shard_ownership")
    entries = canonical_contract.get("configs")
    query_count = QUERY_FRAGMENT_COUNT
    expected_execution_id = _observed_rung_execution_contract_id(query_count)
    expected_status = _publication_status_contract()
    if (not _observes_result_status()
            or query_count not in RUNG_QUERY_COUNTS
            or not isinstance(campaign, dict)
            or campaign.get("campaign_id") != CANONICAL_CAMPAIGN_ID
            or campaign.get("campaign_digest")
                != ACTIVE_EXPECTED_CAMPAIGN_DIGEST
            or campaign.get("execution_contract_id") != expected_execution_id
            or campaign.get("result_status_contract")
                != OBSERVED_RESULT_STATUS_CONTRACT
            or campaign.get("publication_status_contract") != expected_status
            or campaign.get("query_count") != query_count
            or campaign.get("repetitions_per_cell") != REPETITIONS_PER_CELL
            or campaign.get("expected_result_cells") != EXPECTED_RESULT_CELLS
            or campaign.get("expected_raw_records")
                != EXPECTED_RESULT_CELLS * REPETITIONS_PER_CELL
            or campaign.get("required_shard_configs") != 256
            or not isinstance(source, dict)
            or source.get("full_config")
                != str(OUTPUT_CONFIG.relative_to(REPOSITORY_ROOT))
            or source.get("full_config_sha256")
                != _sha256(canonical_config_raw)
            or not isinstance(rung, dict)
            or rung.get("query_count") != query_count
            or not isinstance(ownership, dict)
            or ownership.get("stable_keys_sha256")
                != "5103d36ed70d929f9055b4712b490c521ac08d00e44b36465e536f11010afa5e"
            or not isinstance(entries, list)
            or len(entries) != 257):
        raise GenerationError(
            "observed singleton partition requires one exact accepted-only rung")
    parent_entries = entries[1:]
    if (any(not isinstance(entry, dict) or entry.get("role") != "shard"
            or entry.get("expected_all_records_disposition") is not None
            for entry in parent_entries)
            or ownership.get("shard_paths")
                != [entry.get("path") for entry in parent_entries]):
        raise GenerationError("observed singleton canonical parent order changed")

    directory, manifest_path, prefix, pattern = (
        _observed_singleton_locations(query_count))
    generated: dict[Path, bytes] = {}
    children: list[dict[str, Any]] = []
    parents: list[dict[str, Any]] = []
    all_keys: list[str] = []
    metric_distribution: dict[int, int] = {}
    expected_disposition_distribution = {
        "timed_bioseq": 1280,
        "minimap2_baseline": 16,
    }
    disposition_distribution = {
        key: 0 for key in expected_disposition_distribution}
    ordinal = 0
    for parent_ordinal, parent_entry in enumerate(parent_entries):
        parent_relative = parent_entry["path"]
        parent_path = REPOSITORY_ROOT / parent_relative
        parent_raw = canonical_generated.get(parent_path)
        if parent_raw is None:
            raise GenerationError(
                f"observed generator omitted parent {parent_relative}")
        if (_sha256(parent_raw) != parent_entry.get("config_sha256")
                or parent_entry.get("expected_result_cells")
                    != len(parent_entry.get("cells", []))
                or parent_entry.get("expected_all_records_disposition") is not None):
            raise GenerationError(
                f"observed parent evidence changed for {parent_relative}")
        parent_config = json.loads(
            parent_raw.decode("utf-8"), object_pairs_hook=_strict_object)
        recomputed_parent_cells = _config_cells(parent_config)
        if recomputed_parent_cells != parent_entry["cells"]:
            raise GenerationError(
                f"observed parent descriptors changed for {parent_relative}")
        child_ordinals: list[int] = []
        parent_keys: list[str] = []
        for canonical_cell in parent_entry["cells"]:
            child = _singleton_config(
                parent_config, canonical_cell,
                observed_query_count=query_count)
            raw = _render(child)
            stable_key = canonical_cell["stable_key"]
            stable_key_sha256 = _sha256(stable_key.encode("utf-8"))
            path = directory / (
                f"{prefix}_{ordinal:04d}_{stable_key_sha256[:16]}.json")
            if path in generated:
                raise GenerationError("observed singleton output path collision")
            execution_cell = _config_cells(child)[0]
            metric_names = list(child["bench_e2e"]["matrix"]["metrics"])
            expected_metrics = (
                MINIMAP2_BASELINE_METRIC_NAMES
                if canonical_cell["identity"]["engine"] == "minimap2" else
                MIXED_PERSISTED_METRIC_NAMES)
            if (metric_names != list(expected_metrics)
                    or metric_names != parent_entry["declared_metric_names"]
                    or canonical_cell.get("allowed_statuses") != ["accepted"]):
                raise GenerationError(
                    "observed singleton measurement/status declaration changed")
            metric_distribution[len(metric_names)] = (
                metric_distribution.get(len(metric_names), 0) + 1)
            bucket = (
                "minimap2_baseline"
                if canonical_cell["identity"]["engine"] == "minimap2"
                else "timed_bioseq")
            disposition_distribution[bucket] += 1
            generated[path] = raw
            child_ordinals.append(ordinal)
            parent_keys.append(stable_key)
            all_keys.append(stable_key)
            children.append({
                "ordinal": ordinal,
                "path": str(path.relative_to(REPOSITORY_ROOT)),
                "config_sha256": _sha256(raw),
                "parent_path": parent_relative,
                "parent_config_sha256": parent_entry["config_sha256"],
                "parent_expected_stable_keys_sha256": parent_entry[
                    "expected_stable_keys_sha256"],
                "parent_test_case_index": canonical_cell["test_case_index"],
                "parent_tuple_index": canonical_cell["tuple_index"],
                "stable_key": stable_key,
                "stable_key_sha256": stable_key_sha256,
                "expected_stable_keys_sha256": stable_key_sha256,
                "expected_result_cells": 1,
                "expected_raw_records": REPETITIONS_PER_CELL,
                "repetitions_per_cell": REPETITIONS_PER_CELL,
                "declared_metric_names": metric_names,
                "expected_all_records_disposition": None,
                "canonical_cell": copy.deepcopy(canonical_cell),
                "execution_cell": execution_cell,
            })
            ordinal += 1
        parent_key_digest = _sha256(
            "\n".join(sorted(parent_keys)).encode("utf-8"))
        if parent_key_digest != parent_entry["expected_stable_keys_sha256"]:
            raise GenerationError(
                f"observed singleton changed parent union for {parent_relative}")
        parents.append({
            "ordinal": parent_ordinal,
            "path": parent_relative,
            "config_sha256": parent_entry["config_sha256"],
            "expected_result_cells": parent_entry["expected_result_cells"],
            "expected_raw_records": parent_entry["expected_raw_records"],
            "expected_stable_keys_sha256": parent_key_digest,
            "child_ordinals": child_ordinals,
        })

    stable_keys_sha256 = _sha256(
        "\n".join(sorted(all_keys)).encode("utf-8"))
    if (ordinal != EXPECTED_RESULT_CELLS
            or len(generated) != EXPECTED_RESULT_CELLS
            or len(set(all_keys)) != EXPECTED_RESULT_CELLS
            or stable_keys_sha256 != ownership["stable_keys_sha256"]
            or metric_distribution != {21: 16, 29: 1280}
            or disposition_distribution != expected_disposition_distribution):
        raise GenerationError(
            "observed singleton cardinality, metric, or accepted-only "
            "ownership changed")

    first_launch = children[0]["path"]
    manifest: dict[str, Any] = {
        "schema": OBSERVED_SINGLETON_PARTITION_SCHEMA,
        "version": OBSERVED_SINGLETON_PARTITION_VERSION,
        "campaign_binding": {
            "campaign_id": CANONICAL_CAMPAIGN_ID,
            "campaign_digest": campaign["campaign_digest"],
            "execution_contract_id": expected_execution_id,
            "result_status_contract": OBSERVED_RESULT_STATUS_CONTRACT,
            "query_count": query_count,
            "canonical_manifest": str(
                CONTRACT_PATH.relative_to(REPOSITORY_ROOT)),
            "canonical_manifest_sha256": _sha256(canonical_contract_raw),
            "canonical_config": source["full_config"],
            "canonical_config_sha256": source["full_config_sha256"],
            "canonical_parent_config_count": 256,
            "canonical_result_cells": EXPECTED_RESULT_CELLS,
            "canonical_raw_records": (
                EXPECTED_RESULT_CELLS * REPETITIONS_PER_CELL),
            "canonical_stable_keys_sha256": stable_keys_sha256,
            "fixture_projection_sha256": rung["fixture_projection_sha256"],
            "predecessor_execution_contract_id": rung[
                "predecessor_execution_contract_id"],
        },
        "partition_contract": {
            "child_config_count": EXPECTED_RESULT_CELLS,
            "cells_per_child": 1,
            "records_per_child": REPETITIONS_PER_CELL,
            "repetitions_per_cell": REPETITIONS_PER_CELL,
            "repeat_indices": list(range(REPETITIONS_PER_CELL)),
            "normalization_axes": ["test_case_index", "tuple_index"],
            "canonical_ownership_preserved": True,
            "parent_unions_exact": True,
            "global_union_exact": True,
            "stable_keys_sha256": stable_keys_sha256,
            "metric_count_distribution": {
                str(key): value
                for key, value in sorted(metric_distribution.items())
            },
            "cell_disposition_distribution": disposition_distribution,
            "accepted_only": True,
            "prefilter_enabled": False,
            "pipeline_mode": "task_parallel",
            "cold_no_index_persistence": "off",
            "cigar_projection": True,
            "mapq_projection": True,
            "paired_minimap2_oracle": False,
            "base_alignment_algorithms": ["ksw2", "block_aligner"],
            "excluded_base_alignment_algorithms": ["ksw2_batched"],
            "cross_rung_membership_permitted": False,
            "reserved_filename_regex": pattern,
            "deterministic_first_launch": first_launch,
        },
        "parents": parents,
        "children": children,
    }
    manifest["partition_contract"]["partition_digest_algorithm"] = (
        "sha256-canonical-json-without-partition_digest")
    digest_payload = copy.deepcopy(manifest)
    manifest["partition_contract"]["partition_digest"] = _sha256(
        _canonical_bytes(digest_payload))
    return generated, _render(manifest), directory, manifest_path


def _expected_singleton_partition_outputs(
        canonical_generated: dict[Path, bytes],
        canonical_contract: dict[str, Any],
        canonical_contract_raw: bytes,
) -> tuple[dict[Path, bytes], bytes]:
    """Return the exact 1,296 interruption-safe q1024 child configs."""
    campaign = canonical_contract.get("campaign")
    ownership = canonical_contract.get("shard_ownership")
    entries = canonical_contract.get("configs")
    if (not isinstance(campaign, dict)
            or campaign.get("campaign_id") != CANONICAL_CAMPAIGN_ID
            or campaign.get("campaign_digest") != EXPECTED_CAMPAIGN_DIGEST
            or not isinstance(ownership, dict)
            or ownership.get("stable_keys_sha256")
                != "5103d36ed70d929f9055b4712b490c521ac08d00e44b36465e536f11010afa5e"
            or not isinstance(entries, list)
            or len(entries) != EXPECTED_SHARDS + 1):
        raise GenerationError(
            "singleton partition requires the exact canonical q1024 contract")
    parent_entries = entries[1:]
    if (any(not isinstance(entry, dict) or entry.get("role") != "shard"
            for entry in parent_entries)
            or ownership.get("shard_paths")
                != [entry.get("path") for entry in parent_entries]):
        raise GenerationError("canonical q1024 parent order changed")

    generated: dict[Path, bytes] = {}
    children: list[dict[str, Any]] = []
    parents: list[dict[str, Any]] = []
    all_keys: list[str] = []
    metric_distribution: dict[int, int] = {}
    disposition_distribution = {
        "timed_bioseq": 0,
        "minimap2_baseline": 0,
        "rejected_setup": 0,
        "rejected_sql": 0,
    }
    ordinal = 0
    for parent_ordinal, parent_entry in enumerate(parent_entries):
        parent_relative = parent_entry["path"]
        parent_path = REPOSITORY_ROOT / parent_relative
        parent_raw = canonical_generated.get(parent_path)
        if parent_raw is None:
            raise GenerationError(
                f"canonical generator omitted parent {parent_relative}")
        if (_sha256(parent_raw) != parent_entry.get("config_sha256")
                or parent_entry.get("expected_result_cells")
                    != len(parent_entry.get("cells", []))):
            raise GenerationError(
                f"canonical parent evidence changed for {parent_relative}")
        parent_config = json.loads(
            parent_raw.decode("utf-8"), object_pairs_hook=_strict_object)
        recomputed_parent_cells = _config_cells(parent_config)
        if recomputed_parent_cells != parent_entry["cells"]:
            raise GenerationError(
                f"canonical parent descriptors changed for {parent_relative}")
        child_ordinals: list[int] = []
        parent_keys: list[str] = []
        for canonical_cell in parent_entry["cells"]:
            child = _singleton_config(parent_config, canonical_cell)
            raw = _render(child)
            stable_key = canonical_cell["stable_key"]
            stable_key_sha256 = _sha256(stable_key.encode("utf-8"))
            path = SHARD_DIRECTORY / (
                f"{SINGLETON_FILENAME_PREFIX}_{ordinal:04d}_"
                f"{stable_key_sha256[:16]}.json")
            if path in generated:
                raise GenerationError("singleton output path collision")
            execution_cell = _config_cells(child)[0]
            disposition = _canonical_cell_rejection_disposition(canonical_cell)
            metric_names = list(child["bench_e2e"]["matrix"]["metrics"])
            metric_distribution[len(metric_names)] = (
                metric_distribution.get(len(metric_names), 0) + 1)
            if disposition is None:
                bucket = (
                    "minimap2_baseline"
                    if canonical_cell["identity"]["engine"] == "minimap2"
                    else "timed_bioseq")
            else:
                bucket = disposition
            disposition_distribution[bucket] += 1
            generated[path] = raw
            child_ordinals.append(ordinal)
            parent_keys.append(stable_key)
            all_keys.append(stable_key)
            children.append({
                "ordinal": ordinal,
                "path": str(path.relative_to(REPOSITORY_ROOT)),
                "config_sha256": _sha256(raw),
                "parent_path": parent_relative,
                "parent_config_sha256": parent_entry["config_sha256"],
                "parent_expected_stable_keys_sha256": parent_entry[
                    "expected_stable_keys_sha256"],
                "parent_test_case_index": canonical_cell["test_case_index"],
                "parent_tuple_index": canonical_cell["tuple_index"],
                "stable_key": stable_key,
                "stable_key_sha256": stable_key_sha256,
                "expected_stable_keys_sha256": _sha256(
                    stable_key.encode("utf-8")),
                "expected_result_cells": 1,
                "expected_raw_records": REPETITIONS_PER_CELL,
                "repetitions_per_cell": REPETITIONS_PER_CELL,
                "declared_metric_names": metric_names,
                "expected_all_records_disposition": disposition,
                "canonical_cell": copy.deepcopy(canonical_cell),
                "execution_cell": execution_cell,
            })
            ordinal += 1
        parent_key_digest = _sha256(
            "\n".join(sorted(parent_keys)).encode("utf-8"))
        if parent_key_digest != parent_entry["expected_stable_keys_sha256"]:
            raise GenerationError(
                f"singleton union changed parent ownership for {parent_relative}")
        parents.append({
            "ordinal": parent_ordinal,
            "path": parent_relative,
            "config_sha256": parent_entry["config_sha256"],
            "expected_result_cells": parent_entry["expected_result_cells"],
            "expected_raw_records": parent_entry["expected_raw_records"],
            "expected_stable_keys_sha256": parent_key_digest,
            "child_ordinals": child_ordinals,
        })

    stable_keys_sha256 = _sha256(
        "\n".join(sorted(all_keys)).encode("utf-8"))
    first_launch = (
        "bench/e2e_algorithm_matrix_shards/"
        "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
        "0081_423ec685980909f5.json")
    if (ordinal != EXPECTED_RESULT_CELLS
            or len(generated) != EXPECTED_RESULT_CELLS
            or len(set(all_keys)) != EXPECTED_RESULT_CELLS
            or stable_keys_sha256 != ownership["stable_keys_sha256"]
            or metric_distribution != {15: 512, 21: 16, 29: 768}
            or disposition_distribution != {
                "timed_bioseq": 768,
                "minimap2_baseline": 16,
                "rejected_setup": 480,
                "rejected_sql": 32,
            }
            or children[81]["path"] != first_launch
            or children[81]["parent_path"] != parent_entries[0]["path"]
            or children[81]["parent_tuple_index"] != 81
            or children[81]["canonical_cell"]["algorithm"]
                != "minimap2_no_index"):
        raise GenerationError(
            "q1024 singleton cardinality, metric, disposition, or launch order changed")

    manifest: dict[str, Any] = {
        "schema": SINGLETON_PARTITION_SCHEMA,
        "version": SINGLETON_PARTITION_VERSION,
        "campaign_binding": {
            "campaign_id": CANONICAL_CAMPAIGN_ID,
            "campaign_digest": EXPECTED_CAMPAIGN_DIGEST,
            "canonical_manifest": str(
                CANONICAL_CONTRACT_PATH.relative_to(REPOSITORY_ROOT)),
            "canonical_manifest_sha256": _sha256(canonical_contract_raw),
            "canonical_parent_config_count": EXPECTED_SHARDS,
            "canonical_result_cells": EXPECTED_RESULT_CELLS,
            "canonical_raw_records": EXPECTED_RESULT_CELLS
                * REPETITIONS_PER_CELL,
            "canonical_stable_keys_sha256": stable_keys_sha256,
        },
        "partition_contract": {
            "child_config_count": EXPECTED_RESULT_CELLS,
            "cells_per_child": 1,
            "records_per_child": REPETITIONS_PER_CELL,
            "repetitions_per_cell": REPETITIONS_PER_CELL,
            "repeat_indices": list(range(REPETITIONS_PER_CELL)),
            "normalization_axes": ["test_case_index", "tuple_index"],
            "canonical_ownership_preserved": True,
            "parent_unions_exact": True,
            "global_union_exact": True,
            "stable_keys_sha256": stable_keys_sha256,
            "metric_count_distribution": {
                str(key): value for key, value in sorted(metric_distribution.items())
            },
            "cell_disposition_distribution": disposition_distribution,
            "prefilter_enabled": False,
            "pipeline_mode": "task_parallel",
            "cold_no_index_persistence": "off",
            "cigar_projection": True,
            "mapq_projection": True,
            "base_alignment_algorithms": ["ksw2", "block_aligner"],
            "excluded_base_alignment_algorithms": ["ksw2_batched"],
            "q1_or_q8_membership_permitted": False,
            "reserved_filename_regex": (
                r"bench/e2e_algorithm_matrix_shards/"
                r"bench_e2e_algorithm_matrix_campaign_shard_singleton_"
                r"[0-9]{4}_[0-9a-f]{16}\.json"),
            "deterministic_first_launch": first_launch,
        },
        "parents": parents,
        "children": children,
    }
    manifest["partition_contract"]["partition_digest_algorithm"] = (
        "sha256-canonical-json-without-partition_digest")
    digest_payload = copy.deepcopy(manifest)
    manifest["partition_contract"]["partition_digest"] = _sha256(
        _canonical_bytes(digest_payload))
    return generated, _render(manifest)


def _source_evidence(path: Path, snippets: tuple[str, ...]) -> dict[str, Any]:
    raw = path.read_bytes()
    text = raw.decode("utf-8")
    locations: list[dict[str, Any]] = []
    for snippet in snippets:
        offset = text.find(snippet)
        if offset < 0:
            raise GenerationError(
                f"query-prefix proof snippet disappeared from {path}")
        locations.append({
            "line": text.count("\n", 0, offset) + 1,
            "statement": snippet,
        })
    return {
        "path": str(path.relative_to(REPOSITORY_ROOT)),
        "sha256": _sha256(raw),
        "evidence": locations,
    }


def _query_prefix_proof(
        canonical_source: dict[str, Any],
        source: dict[str, Any],
        source_raw: bytes,
        contract: dict[str, Any],
        contract_raw: bytes,
        full_entry: dict[str, Any],
        shard_entries: list[dict[str, Any]]) -> dict[str, Any]:
    if QUERY_FRAGMENT_COUNT != 8 or NO_INDEX_CELLS_PER_SHARD != 4:
        raise GenerationError(
            "a query-prefix proof is currently defined only for q8/four-cell shards")
    if (canonical_source["params"] != source["params"]
            or canonical_source["bench_e2e"]["matrix"]
                != source["bench_e2e"]["matrix"]
            or canonical_source["regression_rules"] != source["regression_rules"]):
        raise GenerationError(
            "q8 matrix, metrics, repeat schedule, or parameter schemas changed")

    canonical_contract, canonical_contract_raw = _read_json(
        CANONICAL_CONTRACT_PATH)
    canonical_campaign = canonical_contract["campaign"]
    if (canonical_campaign.get("campaign_id") != CANONICAL_CAMPAIGN_ID
            or canonical_campaign.get("campaign_digest")
                != EXPECTED_CAMPAIGN_DIGEST):
        raise GenerationError("canonical q1024 contract identity changed")
    if CAMPAIGN_ID == CANONICAL_CAMPAIGN_ID:
        raise GenerationError("q8 campaign identity must not collide with q1024")

    benchmark_evidence = _source_evidence(
        REPOSITORY_ROOT / "bench/e2e/index.cppm",
        (
            'gp.fragment_count         = static_cast<std::size_t>(jint(data, "fragment_count", 1));',
            '.generate_sequences_pairs(gp.fragment_count);',
        ))
    generator_evidence = _source_evidence(
        REPOSITORY_ROOT /
            "bioseq/backend/sequences/generation/sequence_generator.cppm",
        (
            "return generate_fragment_pairs(generate_reference(), 0, count);",
            "const std::size_t fragment_index = first + pair_index;",
            "seed_stream(params_.seed, k_reference_stream_space + fragment_index);",
        ))
    sealed_matrix_evidence = _source_evidence(
        REPOSITORY_ROOT / "bench/aggregate_e2e_algorithm_matrix.py",
        (
            '"prefilter_enabled": False,',
            '"explicitly_excluded_base_alignment_algorithms": ["ksw2_batched"],',
        ))

    dataset_proofs: list[dict[str, Any]] = []
    for dataset in EXPECTED_DATASETS:
        canonical_data = copy.deepcopy(
            canonical_source["test_cases"][dataset]["data"])
        projected_data = copy.deepcopy(source["test_cases"][dataset]["data"])
        canonical_count = canonical_data.pop("fragment_count")
        projected_count = projected_data.pop("fragment_count")
        if canonical_data != projected_data:
            raise GenerationError(
                f"{dataset} prefix proof found non-count data drift")
        dataset_proofs.append({
            "test_case": dataset,
            "alphabet": projected_data["alphabet"],
            "reference_length": projected_data["reference_length"],
            "seed": projected_data["seed"],
            "q1024_fragment_count": canonical_count,
            "q8_fragment_count": projected_count,
            "generation_data_without_fragment_count_sha256":
                _sha256(_canonical_bytes(projected_data)),
            "q8_fragment_indices": list(range(8)),
            "relation": "exact ordered prefix of q1024 indices 0..7",
        })

    no_index_entries = [
        entry for entry in shard_entries
        if entry["shard_kind"] == "no_index_micro"]
    baseline_entries = [
        entry for entry in shard_entries
        if entry["shard_kind"] == "minimap2_baseline"]
    persisted_entries = [
        entry for entry in shard_entries
        if entry["shard_kind"] == "persisted_index"]
    rescue_manifest, rescue_manifest_raw = _read_json(Q8_RESCUE_MANIFEST_PATH)
    if (rescue_manifest.get("schema")
            != "bioseq.e2e.algorithm-matrix-q8-rescue"
            or rescue_manifest.get("version") != 2
            or rescue_manifest.get("ordinal") != 1):
        raise GenerationError("q8 rescue execution authority is not sealed schema 2")
    return {
        "schema": "bioseq.e2e.algorithm-matrix-q8-prefix-proof",
        "version": 2,
        "verdict": "proven_exact_fixed_seed_ordered_prefix",
        "campaign_binding": {
            "campaign_id": CAMPAIGN_ID,
            "campaign_digest": contract["campaign"]["campaign_digest"],
            "manifest": str(CONTRACT_PATH.relative_to(REPOSITORY_ROOT)),
            "manifest_sha256": _sha256(contract_raw),
            "config": str(OUTPUT_CONFIG.relative_to(REPOSITORY_ROOT)),
            "config_sha256": _sha256(source_raw),
        },
        "q1024_binding": {
            "campaign_id": canonical_campaign["campaign_id"],
            "campaign_digest": canonical_campaign["campaign_digest"],
            "manifest": str(
                CANONICAL_CONTRACT_PATH.relative_to(REPOSITORY_ROOT)),
            "manifest_sha256": _sha256(canonical_contract_raw),
            "config": str(SOURCE_CONFIG.relative_to(REPOSITORY_ROOT)),
            "config_sha256": _sha256(SOURCE_CONFIG.read_bytes()),
        },
        "query_generation_contract": {
            "fragment_count_is_query_count": True,
            "reference_generation_is_independent_of_fragment_count": True,
            "fragment_stream_key":
                "seed_stream(seed, k_reference_stream_space + fragment_index)",
            "first_fragment_index": 0,
            "q8_fragment_indices": list(range(8)),
            "q8_is_subset_of_q1024": True,
            "q8_is_ordered_prefix_of_q1024": True,
            "parallel_schedule_cannot_change_per_index_streams": True,
            "benchmark_loader": benchmark_evidence,
            "sequence_generator": generator_evidence,
            "datasets": dataset_proofs,
        },
        "matrix_and_repeat_contract": {
            "result_cells": EXPECTED_RESULT_CELLS,
            "records": EXPECTED_RESULT_CELLS * REPETITIONS_PER_CELL,
            "repetitions_per_cell": REPETITIONS_PER_CELL,
            "repeat_indices": list(range(REPETITIONS_PER_CELL)),
            "iteration_order": source["bench_e2e"]["iteration_order"],
            "full_matrix_and_metric_declarations_byte_equivalent": True,
            "non_campaign_cell_identity_fields_preserved": True,
            "ksw2_batched_excluded": True,
            "cigar_projection": True,
            "mapq_projection": True,
            "prefilter_enabled": False,
            "sealed_matrix_evidence": sealed_matrix_evidence,
        },
        "timeout_and_shard_contract": {
            "runtime_hard_timeout_seconds": RUNTIME_HARD_TIMEOUT_SECONDS,
            "required_shards": len(shard_entries),
            "no_index_micro_shards": len(no_index_entries),
            "maximum_bioseq_cells_per_no_index_shard": max(
                entry["expected_result_cells"] for entry in no_index_entries),
            "isolated_minimap2_baseline_shards": len(baseline_entries),
            "maximum_cells_per_minimap2_baseline_shard": max(
                entry["expected_result_cells"] for entry in baseline_entries),
            "persisted_index_shards": len(persisted_entries),
            "maximum_cells_per_persisted_index_shard": max(
                entry["expected_result_cells"] for entry in persisted_entries),
        },
        "ownership_and_resume_contract": {
            "exact_nonoverlapping": True,
            "all_cells_appear_exactly_once": True,
            "stable_key_schema": contract["campaign"]["stable_key_schema"],
            "stable_key_fields": contract["campaign"]["stable_key_fields"],
            "stable_keys_sha256":
                contract["shard_ownership"]["stable_keys_sha256"],
            "monolithic_stable_keys_sha256":
                full_entry["expected_stable_keys_sha256"],
            "q1024_stable_keys_sha256":
                canonical_contract["shard_ownership"]["stable_keys_sha256"],
            "campaign_namespace_is_collision_free": True,
            "q8_is_eligible_for_q1024_automatic_merge": False,
            "aggregation_authority":
                "q8 reports must be resumed and aggregated only against this q8 manifest",
        },
        "rescue_execution_authority": {
            "schema": "bioseq.e2e.algorithm-matrix-q8-rescue",
            "version": 2,
            "ordinal": 1,
            "manifest": str(
                Q8_RESCUE_MANIFEST_PATH.relative_to(REPOSITORY_ROOT)),
            "manifest_sha256": _sha256(rescue_manifest_raw),
            "canonical_parent_union_cells": 4,
            "canonical_parent_union_records": 20,
        },
    }


def _rung_fixture_projection_sha256(source: dict[str, Any]) -> str:
    """Bind the compact fixed-seed fixture prefix without listing its ordinals."""
    datasets: list[dict[str, Any]] = []
    for dataset in EXPECTED_DATASETS:
        data = copy.deepcopy(source["test_cases"][dataset]["data"])
        if data.pop("fragment_count", None) != QUERY_FRAGMENT_COUNT:
            raise GenerationError(
                f"{dataset} does not own the observed rung query count")
        datasets.append({"test_case": dataset, "data": data})
    projection = {
        "query_ordinal_interval": {
            "begin": 0,
            "end_exclusive": QUERY_FRAGMENT_COUNT,
        },
        "datasets": datasets,
    }
    return _sha256(_canonical_bytes(projection))


def _canonical_q1024_binding(canonical_source_raw: bytes) -> dict[str, Any]:
    """Revalidate and return the canonical manifest-to-config binding."""
    canonical_contract, canonical_contract_raw = _read_json(
        CANONICAL_CONTRACT_PATH)
    canonical_campaign = canonical_contract.get("campaign")
    if (canonical_contract.get("schema") != CAMPAIGN_SCHEMA
            or canonical_contract.get("version") != CAMPAIGN_VERSION
            or not isinstance(canonical_campaign, dict)
            or canonical_campaign.get("campaign_id") != CANONICAL_CAMPAIGN_ID
            or canonical_campaign.get("campaign_digest")
                != EXPECTED_CAMPAIGN_DIGEST
            or canonical_campaign.get("campaign_digest_algorithm")
                != "sha256-canonical-json-without-campaign_digest"):
        raise GenerationError("canonical q1024 campaign binding changed")
    digest_payload = copy.deepcopy(canonical_contract)
    del digest_payload["campaign"]["campaign_digest"]
    if _sha256(_canonical_bytes(digest_payload)) != EXPECTED_CAMPAIGN_DIGEST:
        raise GenerationError("canonical q1024 campaign digest does not recompute")
    canonical_source_binding = canonical_contract.get("source")
    expected_config_path = str(SOURCE_CONFIG.relative_to(REPOSITORY_ROOT))
    expected_config_sha256 = _sha256(canonical_source_raw)
    if (not isinstance(canonical_source_binding, dict)
            or canonical_source_binding.get("full_config")
                != expected_config_path
            or canonical_source_binding.get("full_config_sha256")
                != expected_config_sha256):
        raise GenerationError(
            "canonical q1024 manifest no longer binds its source config")
    return {
        "campaign_id": CANONICAL_CAMPAIGN_ID,
        "campaign_digest": EXPECTED_CAMPAIGN_DIGEST,
        "manifest": str(
            CANONICAL_CONTRACT_PATH.relative_to(REPOSITORY_ROOT)),
        "manifest_sha256": _sha256(canonical_contract_raw),
        "config": expected_config_path,
        "config_sha256": expected_config_sha256,
    }


def _observed_rung_contract(
        canonical_source: dict[str, Any], canonical_source_raw: bytes,
        source: dict[str, Any]) -> dict[str, Any]:
    """Describe one exact accepted-only fixed-seed prefix of canonical q1024."""
    if not _observes_result_status():
        raise GenerationError("observed rung contract selected for a legacy campaign")
    if QUERY_FRAGMENT_COUNT not in RUNG_QUERY_COUNTS:
        raise GenerationError("observed campaign query count is outside the exact q ladder")
    expected_execution_id = _observed_rung_execution_contract_id(
        QUERY_FRAGMENT_COUNT)
    if (CAMPAIGN_ID != CANONICAL_CAMPAIGN_ID
            or EXECUTION_CONTRACT_ID != expected_execution_id):
        raise GenerationError(
            "observed execution identity must bind its exact q rung without "
            "changing logical tuple identity: "
            f"expected={expected_execution_id}")
    if (canonical_source["params"] != source["params"]
            or canonical_source["bench_e2e"]["matrix"]
                != source["bench_e2e"]["matrix"]
            or canonical_source["regression_rules"] != source["regression_rules"]):
        raise GenerationError(
            "observed rung changed an algorithm axis, metric, repeat, or rule")

    rung_index = RUNG_QUERY_COUNTS.index(QUERY_FRAGMENT_COUNT)
    predecessor = (
        RUNG_QUERY_COUNTS[rung_index - 1] if rung_index != 0 else None)
    predecessor_execution_id = (
        _observed_rung_execution_contract_id(predecessor)
        if predecessor is not None else None)
    return {
        "schema": "bioseq.e2e.algorithm-matrix-observed-rung",
        "version": 1,
        "query_count": QUERY_FRAGMENT_COUNT,
        "ladder_query_counts": list(RUNG_QUERY_COUNTS),
        "predecessor_query_count": predecessor,
        "predecessor_execution_contract_id": predecessor_execution_id,
        "query_ordinal_interval": {
            "begin": 0,
            "end_exclusive": QUERY_FRAGMENT_COUNT,
        },
        "projection": "exact_fixed_seed_ordered_prefix_of_q1024",
        "fixture_projection_sha256": _rung_fixture_projection_sha256(source),
        "matrix_contract": {
            "all_axes_preserved": True,
            "repetitions_per_cell": REPETITIONS_PER_CELL,
            "prefilter_enabled": False,
            "base_alignment_algorithms": ["ksw2", "block_aligner"],
            "excluded_base_alignment_algorithms": ["ksw2_batched"],
        },
        "canonical_campaign_binding": _canonical_q1024_binding(
            canonical_source_raw),
    }


def _expected_outputs() -> tuple[dict[Path, bytes], bytes, bytes, bytes | None]:
    canonical_source, canonical_source_raw = _read_json(SOURCE_CONFIG)
    _validate_source(canonical_source)
    source, source_raw = _derive_campaign_config(canonical_source)
    _validate_query_projection(canonical_source, source)
    _validate_source(source)
    generated: dict[Path, bytes] = {}
    generated_configs: list[tuple[Path, bytes, dict[str, Any], str]] = []

    if NO_INDEX_CELLS_PER_SHARD == 80:
        for dataset in EXPECTED_DATASETS:
            config = _no_index_baseline_shard(source, dataset)
            path = SHARD_DIRECTORY / (
                f"{SHARD_FILENAME_PREFIX}_shard_{_slug(dataset)}_"
                "no_index_baselines.json")
            raw = _render(config)
            generated[path] = raw
            generated_configs.append(
                (path, raw, config, "no_index_baselines"))
    elif NO_INDEX_CELLS_PER_SHARD == 4:
        for dataset, minimizer, chaining in itertools.product(
                EXPECTED_DATASETS, EXPECTED_MINIMIZERS,
                ("scalar_minimap2", "simd_minimap2", "range_minimum_query",
                 "vulkan_tiled_dynamic_programming")):
            config = _no_index_micro_shard(
                source, dataset, minimizer, chaining)
            path = SHARD_DIRECTORY / (
                f"{SHARD_FILENAME_PREFIX}_shard_{_slug(dataset)}_no_index_"
                f"{_slug(minimizer)}_{_slug(chaining)}.json")
            raw = _render(config)
            generated[path] = raw
            generated_configs.append(
                (path, raw, config, "no_index_micro"))
        for dataset, algorithm in itertools.product(
                EXPECTED_DATASETS, BASELINE_ALGORITHMS):
            config = _minimap2_baseline_shard(source, dataset, algorithm)
            path = SHARD_DIRECTORY / (
                f"{SHARD_FILENAME_PREFIX}_shard_{_slug(dataset)}_"
                f"isolated_{_slug(algorithm)}.json")
            raw = _render(config)
            generated[path] = raw
            generated_configs.append(
                (path, raw, config, "minimap2_baseline"))
    else:
        raise GenerationError(
            "no-index cells-per-shard must be exactly 4 or 80")

    for dataset, minimizer, index_name in itertools.product(
            EXPECTED_DATASETS, EXPECTED_MINIMIZERS, EXPECTED_INDEXES):
        config = _persisted_shard(source, dataset, minimizer, index_name)
        path = SHARD_DIRECTORY / (
            f"{SHARD_FILENAME_PREFIX}_shard_"
            f"{_slug(dataset)}_just_index_{_slug(minimizer)}_{_slug(index_name)}.json")
        raw = _render(config)
        generated[path] = raw
        generated_configs.append((path, raw, config, "persisted_index"))

    expected_shards = 88 if NO_INDEX_CELLS_PER_SHARD == 80 else 256
    if len(generated) != expected_shards:
        raise GenerationError(
            f"generator produced {len(generated)} shards, expected {expected_shards}")

    full_entry = _config_entry(
        OUTPUT_CONFIG, source_raw, source, role="monolithic", shard_kind=None)
    shard_entries = [
        _config_entry(path, raw, config, role="shard", shard_kind=kind)
        for path, raw, config, kind in generated_configs
    ]
    all_rejected_entries = [
        entry for entry in shard_entries
        if entry["expected_all_records_disposition"] is not None
    ]
    setup_rejected_entries = [
        entry for entry in all_rejected_entries
        if entry["expected_all_records_disposition"] == "rejected_setup"
    ]
    sql_rejected_entries = [
        entry for entry in all_rejected_entries
        if entry["expected_all_records_disposition"] == "rejected_sql"
    ]
    mixed_persisted_entries = [
        entry for entry in shard_entries
        if entry["shard_kind"] == "persisted_index"
        and entry["expected_all_records_disposition"] is None
    ]
    profile_rejected_shards = sum(
        all((cell["identity"]["kmer_size"], cell["identity"]["window_size"])
            == (20, 15) for cell in entry["cells"])
        for entry in all_rejected_entries
    )
    dna5_rejected_shards = sum(
        all(cell["identity"]["alphabet"] == "dna5"
            and (cell["identity"]["kmer_size"], cell["identity"]["window_size"])
                == (15, 10) for cell in entry["cells"])
        for entry in all_rejected_entries
    )
    dna4_vulkan_rejected_shards = sum(
        all(cell["identity"]["alphabet"] == "dna4"
            and (cell["identity"]["kmer_size"], cell["identity"]["window_size"])
                == (15, 10)
            and cell["identity"]["requested_minimizer_algorithm"]
                == "vulkan_minimap_integer"
            for cell in entry["cells"])
        for entry in all_rejected_entries
    )
    if _observes_result_status():
        if (all_rejected_entries or setup_rejected_entries
                or sql_rejected_entries or profile_rejected_shards
                or dna5_rejected_shards or dna4_vulkan_rejected_shards
                or len(mixed_persisted_entries) != 80):
            raise GenerationError(
                "observed-status campaign retained a predicted rejection or "
                "lost one of its 80 full-metric persisted shards")
    elif (len(all_rejected_entries) != EXPECTED_ALL_REJECTED_SHARDS
            or len(setup_rejected_entries) != 60
            or len(sql_rejected_entries) != 4
            or len(mixed_persisted_entries)
                != EXPECTED_MIXED_PERSISTED_SHARDS
            or (profile_rejected_shards, dna5_rejected_shards,
                dna4_vulkan_rejected_shards) != (40, 20, 4)
            or sum(entry["expected_result_cells"] for entry in all_rejected_entries)
                != EXPECTED_ALL_REJECTED_CELLS
            or sum(entry["expected_raw_records"] for entry in all_rejected_entries)
                != EXPECTED_ALL_REJECTED_RECORDS):
        raise GenerationError(
            "all-rejected shard semantics are not the exact 40/20/4, "
            "64-shard, 512-cell, 2560-record campaign closure")
    metric_distribution: dict[int, int] = {}
    for entry in [full_entry, *shard_entries]:
        metric_count = len(entry["declared_metric_names"])
        metric_distribution[metric_count] = (
            metric_distribution.get(metric_count, 0) + 1)
    if _observes_result_status():
        expected_metric_distribution = (
            {29: 80, 32: 9}
            if NO_INDEX_CELLS_PER_SHARD == 80
            else {21: 16, 29: 240, 32: 1}
        )
    else:
        expected_metric_distribution = (
            {15: 64, 29: 16, 32: 9}
            if NO_INDEX_CELLS_PER_SHARD == 80
            else {15: 64, 21: 16, 29: 176, 32: 1}
        )
    if metric_distribution != expected_metric_distribution:
        raise GenerationError(
            "campaign metric applicability distribution changed: "
            f"actual={metric_distribution} expected={expected_metric_distribution}")
    owner: dict[str, str] = {}
    for entry in shard_entries:
        for cell in entry["cells"]:
            key = cell["stable_key"]
            if key in owner:
                raise GenerationError(
                    f"stable key is owned by both {owner[key]} and {entry['path']}")
            owner[key] = entry["path"]
    full_keys = {cell["stable_key"] for cell in full_entry["cells"]}
    if set(owner) != full_keys or len(owner) != EXPECTED_RESULT_CELLS:
        raise GenerationError(
            f"{expected_shards}-shard ownership is not the exact full campaign set")

    if NO_INDEX_CELLS_PER_SHARD == 4:
        no_index_entries = [
            entry for entry in shard_entries
            if entry["shard_kind"] == "no_index_micro"]
        baseline_entries = [
            entry for entry in shard_entries
            if entry["shard_kind"] == "minimap2_baseline"]
        if (len(no_index_entries) != 160
                or any(entry["expected_result_cells"] != 4
                       for entry in no_index_entries)
                or len(baseline_entries) != 16
                or any(entry["expected_result_cells"] != 1
                       for entry in baseline_entries)):
            raise GenerationError(
                "micro-shard topology must be 160x4 no-index plus 16x1 baselines"
                if _observes_result_status() else
                "q8 timeout topology must be 160x4 no-index plus 16x1 baselines")

    campaign_contract: dict[str, Any] = {
        "campaign_id": CAMPAIGN_ID,
        "campaign_digest_algorithm": "sha256-canonical-json-without-campaign_digest",
        "stable_key_schema": "bioseq-e2e-cell-v2",
        "stable_key_fields": list(STABLE_KEY_FIELDS),
        "repetitions_per_cell": REPETITIONS_PER_CELL,
        "expected_result_cells": EXPECTED_RESULT_CELLS,
        "expected_raw_records": EXPECTED_RESULT_CELLS * REPETITIONS_PER_CELL,
        "required_shard_configs": expected_shards,
        "publication_status_contract": _publication_status_contract(),
    }
    if _observes_result_status():
        campaign_contract["query_count"] = QUERY_FRAGMENT_COUNT
        campaign_contract["result_status_contract"] = RESULT_STATUS_CONTRACT
        campaign_contract["execution_contract_id"] = EXECUTION_CONTRACT_ID

    if _observes_result_status():
        resume_semantics = (
            f"Each observed q{QUERY_FRAGMENT_COUNT} shard owns all five repeats "
            "for its exact cells under the distinct rung execution-contract "
            "identity. "
            "Reports are idempotent by sealed evidence digest; missing disjoint "
            "shards remain pending and nonaccepted outcomes are nonpublishable.")
    elif NO_INDEX_CELLS_PER_SHARD == 80:
        resume_semantics = (
            "Each shard owns every repeat for its exact cells. Reports are "
            "idempotent by sealed evidence digest; missing disjoint shards "
            "remain pending. Dataset baseline shards may be merged before or "
            "after persisted shards.")
    else:
        resume_semantics = (
            "Each q8 shard owns every repeat for its exact cells under the q8 "
            "campaign identity. Reports are idempotent by sealed evidence digest; "
            "missing disjoint micro-shards remain pending. Isolated minimap2 "
            "baselines may be merged before or after Bioseq shards.")

    contract: dict[str, Any] = {
        "schema": CAMPAIGN_SCHEMA,
        "version": (OBSERVED_RUNG_CAMPAIGN_VERSION
                    if _observes_result_status() else CAMPAIGN_VERSION),
        "campaign": campaign_contract,
        "source": {
            "full_config": str(OUTPUT_CONFIG.relative_to(REPOSITORY_ROOT)),
            "full_config_sha256": _sha256(source_raw),
            "generator": str(Path(__file__).resolve().relative_to(REPOSITORY_ROOT)),
        },
        "configs": [full_entry, *shard_entries],
        "shard_ownership": {
            "exact_nonoverlapping": True,
            "shard_paths": [entry["path"] for entry in shard_entries],
            "stable_keys_sha256": _sha256("\n".join(sorted(owner)).encode("utf-8")),
            "resume_semantics": resume_semantics,
        },
    }
    if _observes_result_status():
        contract["rung"] = _observed_rung_contract(
            canonical_source, canonical_source_raw, source)
    digest_payload = copy.deepcopy(contract)
    campaign_digest = _sha256(_canonical_bytes(digest_payload))
    if (ACTIVE_EXPECTED_CAMPAIGN_DIGEST is not None
            and campaign_digest != ACTIVE_EXPECTED_CAMPAIGN_DIGEST):
        raise GenerationError(
            "generated campaign semantics differ from the audited canonical digest; "
            "a deliberate campaign revision must mint and review a new identity/digest; "
            f"generated={campaign_digest} pinned={ACTIVE_EXPECTED_CAMPAIGN_DIGEST}")
    contract["campaign"]["campaign_digest"] = campaign_digest
    contract_raw = _render(contract)
    proof_raw = _render(_query_prefix_proof(
        canonical_source, source, source_raw, contract, contract_raw,
        full_entry, shard_entries)) if PROOF_PATH is not None else None
    return generated, contract_raw, source_raw, proof_raw


def _atomic_write(path: Path, raw: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(raw)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def _check_exact_tree(
        generated: dict[Path, bytes],
        contract_raw: bytes,
        config_raw: bytes,
        proof_raw: bytes | None,
        *,
        permitted_additional_configs: set[Path] | None = None) -> list[str]:
    errors: list[str] = []
    existing = set(SHARD_DIRECTORY.glob("*.json")) if SHARD_DIRECTORY.exists() else set()
    permitted = permitted_additional_configs or set()
    unexpected = sorted(existing - set(generated) - permitted)
    if unexpected:
        errors.append("unexpected shard configs: " + ", ".join(str(path) for path in unexpected))
    for path, expected in generated.items():
        try:
            actual = path.read_bytes()
        except OSError as exc:
            errors.append(f"missing/unreadable {path}: {exc}")
            continue
        if actual != expected:
            errors.append(f"generated config differs: {path}")
    try:
        actual_config = OUTPUT_CONFIG.read_bytes()
    except OSError as exc:
        errors.append(f"missing/unreadable {OUTPUT_CONFIG}: {exc}")
    else:
        if actual_config != config_raw:
            errors.append(f"generated config differs: {OUTPUT_CONFIG}")
    try:
        actual_contract = CONTRACT_PATH.read_bytes()
    except OSError as exc:
        errors.append(f"missing/unreadable {CONTRACT_PATH}: {exc}")
    else:
        if actual_contract != contract_raw:
            errors.append(f"generated contract differs: {CONTRACT_PATH}")
    if PROOF_PATH is not None:
        if proof_raw is None:
            errors.append("proof output was requested but not generated")
        else:
            try:
                actual_proof = PROOF_PATH.read_bytes()
            except OSError as exc:
                errors.append(f"missing/unreadable {PROOF_PATH}: {exc}")
            else:
                if actual_proof != proof_raw:
                    errors.append(f"generated proof differs: {PROOF_PATH}")
    return errors


def _check_singleton_tree(
        generated: dict[Path, bytes], directory: Path,
        manifest_path: Path, manifest_raw: bytes, *,
        permitted_additional_configs: set[Path] | None = None) -> list[str]:
    """Check one additive singleton tree without touching its parent shards."""
    errors: list[str] = []
    existing = set(directory.glob("*.json")) if directory.exists() else set()
    permitted = permitted_additional_configs or set()
    unexpected = sorted(existing - set(generated) - permitted)
    if unexpected:
        errors.append(
            "unexpected singleton configs: "
            + ", ".join(str(path) for path in unexpected))
    for path, expected in generated.items():
        try:
            actual = path.read_bytes()
        except OSError as exc:
            errors.append(f"missing/unreadable {path}: {exc}")
            continue
        if actual != expected:
            errors.append(f"generated singleton config differs: {path}")
    try:
        actual_manifest = manifest_path.read_bytes()
    except OSError as exc:
        errors.append(f"missing/unreadable {manifest_path}: {exc}")
    else:
        if actual_manifest != manifest_raw:
            errors.append(
                f"generated singleton partition manifest differs: {manifest_path}")
    return errors


def main() -> int:
    global ACTIVE_EXPECTED_CAMPAIGN_DIGEST
    global CAMPAIGN_ID
    global CONTRACT_PATH
    global EXECUTION_CONTRACT_ID
    global NO_INDEX_CELLS_PER_SHARD
    global OUTPUT_CONFIG
    global PROOF_PATH
    global QUERY_FRAGMENT_COUNT
    global RESULT_STATUS_CONTRACT
    global SHARD_DIRECTORY
    global SHARD_FILENAME_PREFIX

    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--write", action="store_true")
    action.add_argument("--check", action="store_true")
    parser.add_argument("--campaign-id", default=CANONICAL_CAMPAIGN_ID)
    parser.add_argument(
        "--output-config",
        default=str(SOURCE_CONFIG.relative_to(REPOSITORY_ROOT)))
    parser.add_argument(
        "--shard-directory",
        default=str(CANONICAL_SHARD_DIRECTORY.relative_to(REPOSITORY_ROOT)))
    parser.add_argument(
        "--contract-path",
        default=str(CANONICAL_CONTRACT_PATH.relative_to(REPOSITORY_ROOT)))
    parser.add_argument("--proof-path")
    parser.add_argument(
        "--shard-filename-prefix",
        default="bench_e2e_algorithm_matrix_campaign")
    parser.add_argument(
        "--fragment-count", type=int, default=EXPECTED_CANONICAL_QUERY_COUNT)
    parser.add_argument(
        "--no-index-cells-per-shard", type=int, choices=(4, 80), default=80)
    parser.add_argument(
        "--result-status-contract",
        choices=(LEGACY_RESULT_STATUS_CONTRACT,
                 OBSERVED_RESULT_STATUS_CONTRACT),
        default=LEGACY_RESULT_STATUS_CONTRACT,
    )
    parser.add_argument("--execution-contract-id")
    parser.add_argument("--expected-campaign-digest")
    parser.add_argument(
        "--singleton-partition",
        action="store_true",
        help=(
            "generate/check a separately sealed 1,296-cell singleton execution "
            "partition without changing its canonical campaign"),
    )
    args = parser.parse_args()
    try:
        CAMPAIGN_ID = args.campaign_id
        OUTPUT_CONFIG = _repository_output_path(
            args.output_config, "output config")
        SHARD_DIRECTORY = _repository_output_path(
            args.shard_directory, "shard directory")
        CONTRACT_PATH = _repository_output_path(
            args.contract_path, "contract path")
        PROOF_PATH = (_repository_output_path(args.proof_path, "proof path")
                      if args.proof_path is not None else None)
        SHARD_FILENAME_PREFIX = args.shard_filename_prefix
        QUERY_FRAGMENT_COUNT = args.fragment_count
        NO_INDEX_CELLS_PER_SHARD = args.no_index_cells_per_shard
        RESULT_STATUS_CONTRACT = args.result_status_contract
        EXECUTION_CONTRACT_ID = args.execution_contract_id
        if not re.fullmatch(r"[a-z0-9_]+", SHARD_FILENAME_PREFIX):
            raise GenerationError(
                "shard filename prefix must contain only lowercase letters, digits, underscores")
        if (args.expected_campaign_digest is not None
                and re.fullmatch(r"[0-9a-f]{64}",
                                 args.expected_campaign_digest) is None):
            raise GenerationError(
                "expected campaign digest must be 64 lowercase hexadecimal characters")

        canonical_defaults = (
            CAMPAIGN_ID == CANONICAL_CAMPAIGN_ID
            and OUTPUT_CONFIG == SOURCE_CONFIG
            and SHARD_DIRECTORY == CANONICAL_SHARD_DIRECTORY
            and CONTRACT_PATH == CANONICAL_CONTRACT_PATH
            and PROOF_PATH is None
            and SHARD_FILENAME_PREFIX
                == "bench_e2e_algorithm_matrix_campaign"
            and QUERY_FRAGMENT_COUNT == EXPECTED_CANONICAL_QUERY_COUNT
            and NO_INDEX_CELLS_PER_SHARD == 80
            and RESULT_STATUS_CONTRACT == LEGACY_RESULT_STATUS_CONTRACT
            and EXECUTION_CONTRACT_ID is None)
        if (args.singleton_partition and not canonical_defaults
                and not _observes_result_status()):
            raise GenerationError(
                "singleton partitions accept only canonical q1024 or exact "
                "observed-rung inputs")
        if not canonical_defaults:
            if (OUTPUT_CONFIG == SOURCE_CONFIG
                    or SHARD_DIRECTORY == CANONICAL_SHARD_DIRECTORY
                    or CONTRACT_PATH == CANONICAL_CONTRACT_PATH
                    or (CAMPAIGN_ID == CANONICAL_CAMPAIGN_ID
                        and not _observes_result_status())):
                raise GenerationError(
                    "a projected campaign must use separate config, shard, contract, "
                    "and campaign identities")
            if (QUERY_FRAGMENT_COUNT == 8
                    and RESULT_STATUS_CONTRACT
                        == LEGACY_RESULT_STATUS_CONTRACT):
                q8_paths = (
                    CAMPAIGN_ID,
                    str(OUTPUT_CONFIG.relative_to(REPOSITORY_ROOT)),
                    str(SHARD_DIRECTORY.relative_to(REPOSITORY_ROOT)),
                    str(CONTRACT_PATH.relative_to(REPOSITORY_ROOT)),
                    SHARD_FILENAME_PREFIX,
                    str(PROOF_PATH.relative_to(REPOSITORY_ROOT))
                        if PROOF_PATH is not None else "",
                )
                if (NO_INDEX_CELLS_PER_SHARD != 4 or PROOF_PATH is None
                        or any("q8" not in value for value in q8_paths)):
                    raise GenerationError(
                        "q8 requires q8-named separate paths, a proof path, and a four-cell cap")
        if _observes_result_status():
            expected_id = _observed_rung_execution_contract_id(
                QUERY_FRAGMENT_COUNT)
            if (QUERY_FRAGMENT_COUNT not in RUNG_QUERY_COUNTS
                    or CAMPAIGN_ID != CANONICAL_CAMPAIGN_ID
                    or EXECUTION_CONTRACT_ID != expected_id):
                raise GenerationError(
                    "observed status requires canonical logical tuple identity "
                    "and an exact q-ladder execution identity: "
                    f"expected={expected_id}")
            if PROOF_PATH is not None:
                raise GenerationError(
                    "observed rung projection is embedded in its manifest; "
                    "a second proof path is forbidden")
            if NO_INDEX_CELLS_PER_SHARD != 4:
                raise GenerationError(
                    "observed singleton/campaign topology requires the existing "
                    "four-cell no-index shard cap")
            if CONTRACT_PATH in {
                    HISTORICAL_Q8_CONTRACT_PATH, Q8_RESCUE_MANIFEST_PATH,
                    HISTORICAL_DEADLINE_V2_MANIFEST_PATH}:
                raise GenerationError(
                    "observed rung may not replace a historical manifest")
            if (OUTPUT_CONFIG == HISTORICAL_Q8_CONFIG_PATH
                    or SHARD_DIRECTORY in {
                        HISTORICAL_Q8_SHARD_DIRECTORY,
                        HISTORICAL_DEADLINE_V2_SHARD_DIRECTORY}):
                raise GenerationError(
                    "observed rung may not replace a historical config tree")
            if CONTRACT_PATH.exists():
                existing_contract, _ = _read_json(CONTRACT_PATH)
                existing_campaign = existing_contract.get("campaign")
                if (existing_contract.get("schema") != CAMPAIGN_SCHEMA
                        or existing_contract.get("version")
                            != OBSERVED_RUNG_CAMPAIGN_VERSION
                        or not isinstance(existing_campaign, dict)
                        or existing_campaign.get("campaign_id")
                            != CANONICAL_CAMPAIGN_ID
                        or existing_campaign.get("execution_contract_id")
                            != expected_id
                        or existing_campaign.get("result_status_contract")
                            != OBSERVED_RESULT_STATUS_CONTRACT):
                    raise GenerationError(
                        "observed rung refuses to replace an existing foreign manifest")
        elif EXECUTION_CONTRACT_ID is not None:
            raise GenerationError(
                "legacy campaigns may not declare an execution-contract identity")
        ACTIVE_EXPECTED_CAMPAIGN_DIGEST = (
            args.expected_campaign_digest
            if args.expected_campaign_digest is not None
            else EXPECTED_CAMPAIGN_DIGEST if canonical_defaults else None)

        generated, contract_raw, config_raw, proof_raw = _expected_outputs()
        singleton_generated: dict[Path, bytes] = {}
        singleton_manifest_raw: bytes | None = None
        singleton_directory = SHARD_DIRECTORY
        singleton_manifest_path = SINGLETON_PARTITION_PATH
        if _observes_result_status():
            (singleton_directory, singleton_manifest_path,
             _, _) = _observed_singleton_locations(QUERY_FRAGMENT_COUNT)
        should_generate_singletons = (
            args.singleton_partition
            or canonical_defaults and SINGLETON_PARTITION_PATH.exists()
            or _observes_result_status() and singleton_manifest_path.exists()
        )
        if should_generate_singletons:
            canonical_contract = json.loads(
                contract_raw.decode("utf-8"), object_pairs_hook=_strict_object)
            if _observes_result_status():
                (singleton_generated, singleton_manifest_raw,
                 singleton_directory, singleton_manifest_path) = (
                    _expected_observed_singleton_partition_outputs(
                        generated, canonical_contract, contract_raw,
                        config_raw))
            else:
                singleton_generated, singleton_manifest_raw = (
                    _expected_singleton_partition_outputs(
                        generated, canonical_contract, contract_raw))

        if args.singleton_partition:
            if singleton_manifest_raw is None:
                raise AssertionError("singleton manifest was not generated")
            canonical_errors = _check_exact_tree(
                generated,
                contract_raw,
                config_raw,
                proof_raw,
                permitted_additional_configs=(
                    set(singleton_generated)
                    if singleton_directory == SHARD_DIRECTORY else set()),
            )
            if args.write:
                singleton_directory.mkdir(parents=True, exist_ok=True)
                unexpected = (
                    set(singleton_directory.glob("*.json"))
                    - set(generated)
                    - set(singleton_generated)
                )
                if unexpected:
                    raise GenerationError(
                        "refusing to delete unexpected shard configs: "
                        + ", ".join(str(path) for path in sorted(unexpected)))
                for path, raw in singleton_generated.items():
                    if not path.exists() or path.read_bytes() != raw:
                        _atomic_write(path, raw)
                if (not singleton_manifest_path.exists()
                        or singleton_manifest_path.read_bytes()
                            != singleton_manifest_raw):
                    _atomic_write(
                        singleton_manifest_path, singleton_manifest_raw)
            singleton_errors = _check_singleton_tree(
                singleton_generated,
                singleton_directory,
                singleton_manifest_path,
                singleton_manifest_raw,
                permitted_additional_configs=(
                    set(generated)
                    if singleton_directory == SHARD_DIRECTORY else set()),
            )
            errors = canonical_errors + singleton_errors
            if errors:
                raise GenerationError("; ".join(errors))
            print(
                f"q{QUERY_FRAGMENT_COUNT} "
                f"{'observed ' if _observes_result_status() else ''}"
                "singleton partition current: "
                f"{len(singleton_generated)} configs; "
                "canonical_campaign_digest="
                f"{_read_json(CONTRACT_PATH)[0]['campaign']['campaign_digest']}; "
                "partition_manifest_sha256="
                f"{_sha256(singleton_manifest_raw)}")
            return 0

        permitted_additional = (
            set(singleton_generated)
            if singleton_directory == SHARD_DIRECTORY else set())
        if args.write:
            SHARD_DIRECTORY.mkdir(parents=True, exist_ok=True)
            unexpected = (
                set(SHARD_DIRECTORY.glob("*.json"))
                - set(generated)
                - permitted_additional
            )
            if unexpected:
                raise GenerationError(
                    "refusing to delete unexpected shard configs: "
                    + ", ".join(str(path) for path in sorted(unexpected)))
            for path, raw in generated.items():
                if not path.exists() or path.read_bytes() != raw:
                    _atomic_write(path, raw)
            if OUTPUT_CONFIG == SOURCE_CONFIG:
                if OUTPUT_CONFIG.read_bytes() != config_raw:
                    raise GenerationError(
                        "refusing to rewrite canonical source config")
            elif not OUTPUT_CONFIG.exists() or OUTPUT_CONFIG.read_bytes() != config_raw:
                _atomic_write(OUTPUT_CONFIG, config_raw)
            if not CONTRACT_PATH.exists() or CONTRACT_PATH.read_bytes() != contract_raw:
                _atomic_write(CONTRACT_PATH, contract_raw)
            if PROOF_PATH is not None and proof_raw is not None \
                    and (not PROOF_PATH.exists()
                         or PROOF_PATH.read_bytes() != proof_raw):
                _atomic_write(PROOF_PATH, proof_raw)
        errors = _check_exact_tree(
            generated,
            contract_raw,
            config_raw,
            proof_raw,
            permitted_additional_configs=permitted_additional,
        )
        if singleton_manifest_raw is not None:
            errors.extend(_check_singleton_tree(
                singleton_generated,
                singleton_directory,
                singleton_manifest_path,
                singleton_manifest_raw,
                permitted_additional_configs=(
                    set(generated)
                    if singleton_directory == SHARD_DIRECTORY else set()),
            ))
        if errors:
            raise GenerationError("; ".join(errors))
    except GenerationError as exc:
        print(f"campaign shard generation failed: {exc}", file=os.sys.stderr)
        return 2
    print(
        f"campaign shard tree current: {len(generated)} configs; "
        f"contract={CONTRACT_PATH.relative_to(REPOSITORY_ROOT)}; "
        f"campaign_digest={_read_json(CONTRACT_PATH)[0]['campaign']['campaign_digest']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
