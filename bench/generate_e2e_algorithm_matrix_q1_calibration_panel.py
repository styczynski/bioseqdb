#!/usr/bin/env python3
"""Seal the five-cell q1 calibration panel without generating shard configs.

The shard JSON files are independently generated from their exact q8 sources.
This tool validates that transformation and writes only the q1 prefix proof and
panel manifest.  ``--check`` is side-effect free.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import tempfile
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "bench/bench_e2e_algorithm_matrix_q1_calibration_manifest.json"
PROOF = ROOT / "bench/bench_e2e_algorithm_matrix_q1_calibration_prefix_proof.json"
TEST_CASE_Q8 = "M2_D4_K15_W10"
TEST_CASE_Q1 = "M2_D4_K15_W10_Q1_CALIBRATION"
Q1_TEST_DESCRIPTION = (
    "Q1_CALIBRATION: 2 Gbp dna4, k15/w10, exactly one deterministic "
    "first-query prefix of q8"
)

CELLS = (
    {
        "id": "minimap2_no_index",
        "role": "minimap2_baseline",
        "q1": (
            "bench/e2e_algorithm_matrix_q1_calibration_shards/"
            "bench_e2e_algorithm_matrix_q1_calibration_m2_d4_k15_w10_"
            "isolated_minimap2_no_index.json"
        ),
        "q8": (
            "bench/e2e_algorithm_matrix_q8_shards/"
            "bench_e2e_algorithm_matrix_q8_campaign_shard_m2_d4_k15_w10_"
            "isolated_minimap2_no_index.json"
        ),
    },
    {
        "id": "fragment_ksw2",
        "role": "measured_bioseq_anchor",
        "q1": (
            "bench/e2e_algorithm_matrix_q1_calibration_shards/"
            "bench_e2e_algorithm_matrix_q1_calibration_m2_d4_k15_w10_no_index_"
            "scalar_scalar_minimap2_fragment_ksw2.json"
        ),
        "q8": (
            "bench/e2e_algorithm_matrix_q8_rescue_shards/"
            "bench_e2e_algorithm_matrix_q8_ordinal1_rescue_m2_d4_k15_w10_"
            "no_index_scalar_scalar_minimap2_fragment_ksw2.json"
        ),
    },
    {
        "id": "fragment_block_aligner",
        "role": "bioseq_calibration",
        "q1": (
            "bench/e2e_algorithm_matrix_q1_calibration_shards/"
            "bench_e2e_algorithm_matrix_q1_calibration_m2_d4_k15_w10_no_index_"
            "scalar_scalar_minimap2_fragment_block_aligner.json"
        ),
        "q8": (
            "bench/e2e_algorithm_matrix_q8_rescue_shards/"
            "bench_e2e_algorithm_matrix_q8_ordinal1_rescue_m2_d4_k15_w10_"
            "no_index_scalar_scalar_minimap2_fragment_block_aligner.json"
        ),
    },
    {
        "id": "sequence_ksw2",
        "role": "bioseq_calibration",
        "q1": (
            "bench/e2e_algorithm_matrix_q1_calibration_shards/"
            "bench_e2e_algorithm_matrix_q1_calibration_m2_d4_k15_w10_no_index_"
            "scalar_scalar_minimap2_sequence_ksw2.json"
        ),
        "q8": (
            "bench/e2e_algorithm_matrix_q8_rescue_shards/"
            "bench_e2e_algorithm_matrix_q8_ordinal1_rescue_m2_d4_k15_w10_"
            "no_index_scalar_scalar_minimap2_sequence_ksw2.json"
        ),
    },
    {
        "id": "sequence_block_aligner",
        "role": "bioseq_calibration",
        "q1": (
            "bench/e2e_algorithm_matrix_q1_calibration_shards/"
            "bench_e2e_algorithm_matrix_q1_calibration_m2_d4_k15_w10_no_index_"
            "scalar_scalar_minimap2_sequence_block_aligner.json"
        ),
        "q8": (
            "bench/e2e_algorithm_matrix_q8_rescue_shards/"
            "bench_e2e_algorithm_matrix_q8_ordinal1_rescue_m2_d4_k15_w10_"
            "no_index_scalar_scalar_minimap2_sequence_block_aligner.json"
        ),
    },
)


def _reject_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON number {value!r}")


def _pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _load(relative: str) -> dict[str, Any]:
    with (ROOT / relative).open("r", encoding="utf-8") as handle:
        value = json.load(
            handle,
            object_pairs_hook=_pairs,
            parse_constant=_reject_constant,
        )
    if not isinstance(value, dict):
        raise ValueError(f"{relative} is not a JSON object")
    return value


def _sha(relative_or_path: str | Path) -> str:
    path = (
        relative_or_path
        if isinstance(relative_or_path, Path)
        else ROOT / relative_or_path
    )
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _canonical_bytes(value: Any) -> bytes:
    return (json.dumps(
        value,
        ensure_ascii=True,
        allow_nan=False,
        indent=2,
    ) + "\n").encode("utf-8")


def _canonical_sha(value: Any) -> str:
    return hashlib.sha256(json.dumps(
        value,
        ensure_ascii=True,
        allow_nan=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")).hexdigest()


def _atomic_write(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent, prefix=f".{path.name}.", suffix=".tmp")
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _single_case(config: dict[str, Any], name: str) -> dict[str, Any]:
    cases = config.get("test_cases")
    if not isinstance(cases, dict) or set(cases) != {name}:
        raise ValueError(f"config must contain only test case {name}")
    case = cases[name]
    if not isinstance(case, dict):
        raise ValueError("test case is malformed")
    return case


def _normalized_q1(config: dict[str, Any], source: dict[str, Any]) -> dict[str, Any]:
    """Reverse the seven authorized presentation/query-count changes."""
    normalized = copy.deepcopy(config)
    q1_case = _single_case(normalized, TEST_CASE_Q1)
    q8_case = _single_case(source, TEST_CASE_Q8)
    normalized["test_cases"] = {TEST_CASE_Q8: q1_case}
    q1_case["data"]["fragment_count"] = 8
    q1_case["description"] = q8_case["description"]
    bench = normalized["bench_e2e"]
    source_bench = source["bench_e2e"]
    bench["tags"] = copy.deepcopy(source_bench["tags"])
    bench["description"] = source_bench["description"]
    repetitions = bench["matrix"]["for"].pop(TEST_CASE_Q1)
    bench["matrix"]["for"][TEST_CASE_Q8] = repetitions
    return normalized


def _algorithm_identity(config: dict[str, Any]) -> dict[str, Any]:
    over = config["bench_e2e"]["matrix"]["over"]
    if set(over) == {"minimap2_no_index"}:
        return {
            "algorithm": "minimap2_no_index",
            "engine": "minimap2",
            "index_mode": "no_index",
            "minimizer_algorithm": None,
            "chaining_algorithm": None,
            "base_alignment_algorithm": None,
            "minimizer_index": None,
        }
    if set(over) != {"bioseq_algorithm_matrix_no_index"}:
        raise ValueError("unexpected q1 algorithm group")
    axes = over["bioseq_algorithm_matrix_no_index"]
    return {
        "algorithm": "bioseq_algorithm_matrix_no_index",
        "engine": "bioseq",
        "index_mode": "no_index",
        "minimizer_algorithm": axes["minimizer_algorithm"][0],
        "chaining_algorithm": axes["chaining_algorithm"][0],
        "base_alignment_algorithm": axes["base_alignment_algorithm"][0],
        "minimizer_index": axes["minimizer_index"][0],
    }


def _validate_pair(cell: dict[str, str]) -> tuple[dict[str, Any], dict[str, Any]]:
    q1 = _load(cell["q1"])
    q8 = _load(cell["q8"])
    case = _single_case(q1, TEST_CASE_Q1)
    data = case.get("data")
    bench = q1.get("bench_e2e")
    if (not isinstance(data, dict)
            or data.get("fragment_count") != 1
            or data.get("cigar_projection") is not True
            or data.get("mapq_projection") is not True
            or not isinstance(bench, dict)
            or bench.get("fast") is not False
            or bench.get("tags") != [
                "e2e", "algorithm-matrix", "calibration-shard", "q1-calibration"]
            or bench.get("matrix", {}).get("for") != {TEST_CASE_Q1: 5}
            or "ksw2_batched" in json.dumps(q1, sort_keys=True)):
        raise ValueError(f"{cell['q1']} violates the q1 runtime contract")
    if _normalized_q1(q1, q8) != q8:
        raise ValueError(
            f"{cell['q1']} is not the exact authorized q8-to-q1 transformation")
    identity = _algorithm_identity(q1)
    over = bench["matrix"]["over"]
    if identity["engine"] == "bioseq":
        axes = over["bioseq_algorithm_matrix_no_index"]
        if (axes.get("pipeline_mode") != ["task_parallel"]
                or axes.get("recompute_async_persistence") != ["off"]):
            raise ValueError("q1 Bioseq persistence/scheduler contract changed")
    for key, value in _walk_items(q1):
        if (key.lower() in {"prefilter", "prefilter_enabled", "use_prefilter"}
                and value not in (False, 0, None, "off")):
            raise ValueError("q1 prefilter must remain disabled")
    return q1, q8


def _walk_items(value: Any):
    if isinstance(value, dict):
        for key, child in value.items():
            yield key, child
            yield from _walk_items(child)
    elif isinstance(value, list):
        for child in value:
            yield from _walk_items(child)


def _build_documents() -> tuple[dict[str, Any], dict[str, Any]]:
    bindings: list[dict[str, Any]] = []
    manifest_cells: list[dict[str, Any]] = []
    data_projection = None
    for ordinal, cell in enumerate(CELLS):
        q1, _ = _validate_pair(cell)
        data = copy.deepcopy(_single_case(q1, TEST_CASE_Q1)["data"])
        data.pop("fragment_count")
        if data_projection is None:
            data_projection = data
        elif data != data_projection:
            raise ValueError("q1 panel generation data differs across cells")
        metrics = list(q1["bench_e2e"]["matrix"]["metrics"])
        q1_sha = _sha(cell["q1"])
        q8_sha = _sha(cell["q8"])
        binding = {
            "ordinal": ordinal,
            "id": cell["id"],
            "q1_path": cell["q1"],
            "q1_sha256": q1_sha,
            "q8_source_path": cell["q8"],
            "q8_source_sha256": q8_sha,
            "q1_fragment_count": 1,
            "q8_fragment_count": 8,
        }
        bindings.append(binding)
        measured = None
        if cell["id"] == "fragment_ksw2":
            measured = {
                "preserved": True,
                "complete": True,
                "record_count": 5,
                "report_path": "reporting/bioseq_benchmark_e2e/report_8.json",
                "report_sha256": (
                    "5fc7759f9e38de1c6b541831684b4cf00bd855432965e177bb58858e4c28156f"),
                "raw_path": "reporting/bioseq_benchmark_e2e/report_8.raw.jsonl",
                "raw_sha256": (
                    "dc2fddbc9f8897be0e8909278afa496a70af0bd48a1203718436a4554fdb00b2"),
            }
        manifest_cells.append({
            "ordinal": ordinal,
            "id": cell["id"],
            "role": cell["role"],
            "path": cell["q1"],
            "config_sha256": q1_sha,
            "q8_source_path": cell["q8"],
            "q8_source_sha256": q8_sha,
            "test_case": TEST_CASE_Q1,
            "execution_identity": _algorithm_identity(q1),
            "metric_names": metrics,
            "metric_count": len(metrics),
            "repetitions": 5,
            "expected_records": 5,
            "measured_evidence": measured,
        })

    assert data_projection is not None
    source_bindings = {
        relative: _sha(relative)
        for relative in (
            "bench/e2e/index.cppm",
            "bioseq/backend/sequences/generation/sequence_generator.cppm",
        )
    }
    proof = {
        "schema": "bioseq.e2e_algorithm_matrix_q1_calibration_prefix_proof",
        "schema_version": 2,
        "identity": "Q1_CALIBRATION_PANEL",
        "verdict": "proven_exact_first_query_prefix_of_q8_against_current_source",
        "config_bindings": bindings,
        "current_source_bindings": source_bindings,
        "generation_data_without_fragment_count": {
            "canonical_json_sha256": _canonical_sha(data_projection),
            "value": data_projection,
        },
        "deterministic_prefix_reasoning": {
            "fragment_count_is_query_count": True,
            "generation_entrypoint": (
                "generate_sequences_pairs(count) constructs one reference then "
                "calls generate_fragment_pairs(reference, 0, count)"),
            "reference_independent_of_fragment_count": True,
            "fragment_stream_key": (
                "seed_stream(seed, k_reference_stream_space + fragment_index)"),
            "q1_fragment_indices": [0],
            "q8_fragment_indices": list(range(8)),
            "formal_relation": "q1_queries == q8_queries[0:1]",
            "parallel_schedule_independent": True,
            "ordered_output_identity": True,
        },
        "validation_scope": {
            "pair_count": 5,
            "q1_cells": 5,
            "q1_records": 25,
            "exact_q8_to_q1_transformation": True,
            "all_other_json_content_matches": True,
        },
    }
    proof_sha = hashlib.sha256(_canonical_bytes(proof)).hexdigest()
    manifest = {
        "schema": "bioseq.e2e_algorithm_matrix_q1_calibration_manifest",
        "schema_version": 2,
        "created_at": "2026-08-26T04:26:24+02:00",
        "updated_at": "2026-08-26T20:38:31+02:00",
        "identity": "Q1_CALIBRATION_PANEL",
        "status": "local_only_static_configuration",
        "purpose": (
            "Five-cell single-query feasibility panel mechanically derived from "
            "the sealed q8 baseline and ordinal-1 rescue children."),
        "panel_contract": {
            "result_cells": 5,
            "repetitions_per_cell": 5,
            "repeat_indices": list(range(5)),
            "expected_raw_records": 25,
            "exact_nonoverlapping": True,
            "cell_ids": [cell["id"] for cell in CELLS],
        },
        "prefix_proof": {
            "path": PROOF.relative_to(ROOT).as_posix(),
            "sha256": proof_sha,
            "verdict": proof["verdict"],
        },
        "transformation_contract": {
            "source_query_count": 8,
            "execution_query_count": 1,
            "source_test_case": TEST_CASE_Q8,
            "execution_test_case": TEST_CASE_Q1,
            "permitted_changes": [
                "test-case key", "fragment_count 8->1", "test-case description",
                "two calibration tags", "benchmark description", "matrix.for key",
            ],
            "all_other_json_content_must_match": True,
        },
        "cells": manifest_cells,
        "runtime_contract": {
            "mode": "benchmark-e2e",
            "exact_benchmark_filter": {"benchmarks": ["bench_e2e"]},
            "fast": False,
            "prefilter": False,
            "prefilter_encoding": "omitted_or_explicit_default_off",
            "pipeline_mode": "task_parallel",
            "recompute_async_persistence": "off",
            "cigar_projection": True,
            "mapq_projection": True,
            "keep_benchmark_artifacts": False,
            "repetitions_per_cell": 5,
            "ksw2_batched_permitted": False,
            "exact_parity_and_fallback_attestation_required": True,
            "priority": {
                "darwin_worker_nice": 0,
                "fail_before_launch_if_worker_nice_is_not_zero": True,
                "runner_and_postgresql_backend_nice_observation_per_run": True,
                "postgresql_inheritance_claimed": False,
            },
        },
        "nonpublication_rule": {
            "classification": "feasibility_only_nonpublishable",
            "merge_authority": "none",
            "q1_must_not_enter_Q8_RESULTS_yaml": True,
            "q1_must_not_enter_ALL_RESULTS_yaml": True,
            "q1_does_not_replace_q8_or_q1024": True,
            "partial_records_are_forensic_only": True,
        },
    }
    return proof, manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--write", action="store_true")
    group.add_argument("--check", action="store_true")
    args = parser.parse_args()
    proof, manifest = _build_documents()
    expected = ((PROOF, _canonical_bytes(proof)), (MANIFEST, _canonical_bytes(manifest)))
    if args.check:
        mismatches = [path for path, payload in expected
                      if not path.is_file() or path.read_bytes() != payload]
        if mismatches:
            for path in mismatches:
                print(f"stale: {path.relative_to(ROOT)}")
            return 1
    else:
        for path, payload in expected:
            _atomic_write(path, payload)
    print("q1 calibration panel: 5 cells, 5 repeats each, 25 records")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
