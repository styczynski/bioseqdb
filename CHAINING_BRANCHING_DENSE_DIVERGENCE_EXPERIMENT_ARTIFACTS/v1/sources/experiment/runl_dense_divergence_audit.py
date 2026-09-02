#!/usr/bin/env python3
"""Audit and summarize the 0--20 percent dense mosaic divergence sweep.

The accepted experiment runner remains the authority for collecting raw
minimap2 output.  This program treats its immutable result shards as inputs,
validates their bindings to the manifest and raw artifacts, and independently
replays the exact ``Run_4`` predicate from every initial DP predecessor dump.
It then emits a review-oriented YAML document and native LaTeX coordinate
macros for the thesis figure and table.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import asdict, dataclass
from decimal import Decimal, ROUND_HALF_UP
import hashlib
import json
import math
from pathlib import Path
from statistics import median
import sys
from typing import Any, Iterable, Mapping, Sequence

import yaml

import runl_repeat_run_experiment as core_runner
from runl_repeat_common import (
    CaseSpec,
    atomic_write_bytes,
    canonical_json_sha256,
    load_manifest,
    sha256_file,
    utc_timestamp,
)
SCHEMA_VERSION = 1
EXPERIMENT_ID = "runl_dense_divergence_audit"
LANES = 4
REPLICATE_COUNT = 3
LEVEL_COUNT = 81
EXPECTED_CASE_COUNT = REPLICATE_COUNT * (LEVEL_COUNT + 1)
LEVEL_STEP = Decimal("0.0025")
MAX_LEVEL = Decimal("0.20")
EXPECTED_LEVELS = tuple(LEVEL_STEP * index for index in range(LEVEL_COUNT))
EXPECTED_DENSE_ANALYSIS_SOURCES = {
    "runl_repeat_common.py",
    "runl_repeat_dense_run_experiment.py",
    "runl_repeat_run_experiment.py",
}
SELECTED_LEVELS = tuple(
    Decimal(value)
    for value in ("0", "0.0025", "0.005", "0.01", "0.02", "0.05", "0.10", "0.20")
)
COUNTER_FIELDS = (
    "loop_evaluations",
    "bounds_pass",
    "bounds_fail_current",
    "bounds_fail_capacity",
    "bounds_fail_both",
    "contiguous_mismatch",
    "fast_batches",
    "scalar_fallbacks",
    "primary_path_anchors",
    "fast_covered_anchors",
    "emulation_error",
)


class AuditError(ValueError):
    """Raised when any evidence or derived value fails a required check."""


@dataclass(frozen=True)
class DumpData:
    """The subset of a chaining dump needed for an independent replay."""

    stage: str
    call_id: int
    anchor_count: int
    candidate_end_count: int
    min_score: int
    candidates: tuple[tuple[int, int, int], ...]
    scores: tuple[int, ...]
    predecessors: tuple[int, ...]


@dataclass(frozen=True)
class CaseMetrics:
    """Audited measurements for one accepted mosaic case."""

    case_id: str
    replicate_id: str
    seed: int
    independent_control: bool
    nominal_divergence_fraction: float | None
    substitution_count: int
    realized_divergence_percent: float
    path_anchor_count: int
    grouped_path_percent: float
    fast_batches: int
    loop_evaluations: int
    bounds_pass: int
    fire_percent: float
    contiguous_percent: float
    coverage_percent: float
    l4_counter_sha256: str


def parse_args() -> argparse.Namespace:
    """Parse explicit evidence and output paths."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--raw-dir", type=Path, required=True)
    parser.add_argument("--shard-dir", type=Path, required=True)
    parser.add_argument("--analysis-yaml", type=Path, required=True)
    parser.add_argument("--latex-data", type=Path, required=True)
    parser.add_argument(
        "--old-results",
        type=Path,
        help="optional accepted coarse-sweep runner aggregate to cross-check",
    )
    parser.add_argument(
        "--replace",
        action="store_true",
        help="atomically replace existing analysis outputs",
    )
    return parser.parse_args()


def require(condition: bool, message: str) -> None:
    """Raise a stable audit error when a required condition is false."""

    if not condition:
        raise AuditError(message)


def require_mapping(value: Any, context: str) -> dict[str, Any]:
    """Return an object after validating its type."""

    require(isinstance(value, dict), f"{context} must be an object")
    return value


def require_list(value: Any, context: str) -> list[Any]:
    """Return an array after validating its type."""

    require(isinstance(value, list), f"{context} must be an array")
    return value


def load_yaml_mapping(path: Path) -> dict[str, Any]:
    """Load a YAML document whose root must be an object."""

    with path.open("r", encoding="utf-8") as source:
        return require_mapping(yaml.safe_load(source), str(path))


def decimal_level(value: Any, context: str) -> Decimal:
    """Parse a finite configured divergence fraction without float rounding."""

    try:
        parsed = Decimal(str(value))
    except Exception as error:
        raise AuditError(f"{context} is not a decimal divergence fraction") from error
    require(parsed.is_finite(), f"{context} must be finite")
    return parsed


def percent(numerator: int, denominator: int, context: str) -> float:
    """Compute a finite percentage with a required positive denominator."""

    require(denominator > 0, f"{context} has an empty denominator")
    value = 100.0 * numerator / denominator
    require(math.isfinite(value), f"{context} is not finite")
    return value


def read_fasta_sequence(path: Path) -> bytes:
    """Read the one-record FASTA already authenticated by ``load_manifest``."""

    sequence = bytearray()
    header_count = 0
    with path.open("rb") as source:
        for raw_line in source:
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith(b">"):
                header_count += 1
            else:
                sequence.extend(line.upper())
    require(header_count == 1, f"{path} must contain exactly one FASTA record")
    return bytes(sequence)


def parse_dump_independently(path: Path) -> DumpData:
    """Parse one DP dump without using the runner's dump parser."""

    meta: tuple[str, int, int, int, int] | None = None
    candidates: list[tuple[int, int, int]] = []
    scores: list[int] = []
    predecessors: list[int] = []
    with path.open("r", encoding="ascii") as source:
        for line_number, raw_line in enumerate(source, start=1):
            fields = raw_line.rstrip("\n").split("\t")
            require(fields and fields[0], f"{path}:{line_number} is empty")
            if fields[0] == "META":
                require(
                    len(fields) == 6 and meta is None,
                    f"{path}:{line_number} has an invalid META row",
                )
                meta = (
                    fields[1],
                    int(fields[2]),
                    int(fields[3]),
                    int(fields[4]),
                    int(fields[5]),
                )
            elif fields[0] == "Z":
                require(
                    len(fields) == 4,
                    f"{path}:{line_number} has an invalid candidate row",
                )
                rank, score, anchor_index = map(int, fields[1:])
                require(
                    rank == len(candidates),
                    f"{path}:{line_number} has a nonconsecutive candidate rank",
                )
                candidates.append((rank, score, anchor_index))
            elif fields[0] == "P":
                require(
                    len(fields) == 4,
                    f"{path}:{line_number} has an invalid predecessor row",
                )
                index, score, predecessor = map(int, fields[1:])
                require(
                    index == len(scores),
                    f"{path}:{line_number} has a nonconsecutive anchor index",
                )
                require(
                    -1 <= predecessor < index,
                    f"{path}:{line_number} has invalid p[{index}]={predecessor}",
                )
                scores.append(score)
                predecessors.append(predecessor)
            else:
                raise AuditError(f"{path}:{line_number} has unknown row {fields[0]!r}")
    require(meta is not None, f"{path} lacks a META row")
    stage, call_id, anchor_count, candidate_count, min_score = meta
    require(stage == "dp", f"{path} is not an initial DP dump")
    require(anchor_count == len(scores), f"{path} anchor count differs from P rows")
    require(candidate_count == len(candidates), f"{path} candidate count differs from Z rows")
    observed_candidate_pairs: list[tuple[int, int]] = []
    for _, score, anchor_index in candidates:
        require(0 <= anchor_index < anchor_count, f"{path} candidate index is outside f[]")
        require(score == scores[anchor_index], f"{path} candidate score differs from f[{anchor_index}]")
        require(score >= min_score, f"{path} candidate score is below min_sc")
        observed_candidate_pairs.append((score, anchor_index))
    expected_candidate_pairs = sorted(
        (score, index)
        for index, score in enumerate(scores)
        if score >= min_score
    )
    require(
        observed_candidate_pairs == expected_candidate_pairs,
        f"{path} Z is not the exact score/index ordering derived from f[]",
    )
    return DumpData(
        stage=stage,
        call_id=call_id,
        anchor_count=anchor_count,
        candidate_end_count=candidate_count,
        min_score=min_score,
        candidates=tuple(candidates),
        scores=tuple(scores),
        predecessors=tuple(predecessors),
    )


def replay_runl4(start_index: int, predecessors: Sequence[int]) -> dict[str, int]:
    """Replay the thesis ``Run_L`` loop for ``L=4`` from the raw p array."""

    n = len(predecessors)
    require(0 <= start_index < n, "initial DP start index is outside the p array")
    j = start_index
    consumed = 0
    counters = {field: 0 for field in COUNTER_FIELDS}
    while j >= 0 and consumed < n:
        position_ok = j >= LANES - 1
        capacity_ok = consumed <= n - LANES
        counters["loop_evaluations"] += 1
        if position_ok and capacity_ok:
            counters["bounds_pass"] += 1
        elif not position_ok and not capacity_ok:
            counters["bounds_fail_both"] += 1
        elif not position_ok:
            counters["bounds_fail_current"] += 1
        else:
            counters["bounds_fail_capacity"] += 1

        contiguous = position_ok and capacity_ok and all(
            predecessors[j - offset] == j - offset - 1
            for offset in range(LANES)
        )
        if contiguous:
            counters["fast_batches"] += 1
            counters["fast_covered_anchors"] += LANES
            j -= LANES
            consumed += LANES
            continue

        if position_ok and capacity_ok:
            counters["contiguous_mismatch"] += 1
        counters["scalar_fallbacks"] += 1
        if not 0 <= j < n:
            counters["emulation_error"] = 1
            break
        predecessor = predecessors[j]
        if predecessor < -1 or predecessor >= j:
            counters["emulation_error"] = 1
            break
        j = predecessor
        consumed += 1

    counters["primary_path_anchors"] = consumed
    return counters


def find_initial_l4_record(run: Mapping[str, Any], context: str) -> dict[str, Any]:
    """Find the single lane-four record from the runner's initial DP call."""

    validation = require_mapping(run.get("validation"), f"{context}.validation")
    telemetry = require_mapping(validation.get("telemetry"), f"{context}.telemetry")
    initial_call_id = int(telemetry.get("initial_dp_call_id", -1))
    records = require_list(telemetry.get("initial_dp_records"), f"{context}.initial records")
    matching = [
        require_mapping(record, f"{context}.initial record")
        for record in records
        if int(require_mapping(record, f"{context}.initial record").get("lanes", -1))
        == LANES
        and int(record.get("call_id", -1)) == initial_call_id
        and record.get("stage") == "dp"
    ]
    require(len(matching) == 1, f"{context} must have one initial DP L=4 record")
    return matching[0]


def initial_dump_path(run: Mapping[str, Any], raw_case: Path, context: str) -> Path:
    """Resolve the dump declared for the initial DP call."""

    validation = require_mapping(run.get("validation"), f"{context}.validation")
    telemetry = require_mapping(validation.get("telemetry"), f"{context}.telemetry")
    call_id = int(telemetry.get("initial_dp_call_id", -1))
    expected_name = f"runl_call_{call_id}_dp.tsv"
    artifacts = require_list(validation.get("dump_artifacts"), f"{context}.dump artifacts")
    matching: list[Path] = []
    for artifact_value in artifacts:
        artifact = require_mapping(artifact_value, f"{context}.dump artifact")
        relative = Path(str(artifact.get("path", "")))
        if relative.name == expected_name:
            candidate = (raw_case / relative).resolve()
            try:
                candidate.relative_to(raw_case.resolve())
            except ValueError as error:
                raise AuditError(f"{context} initial dump escapes its case directory") from error
            matching.append(candidate)
    require(len(matching) == 1, f"{context} must declare one initial DP dump")
    return matching[0]


def validate_paf_and_path(case: CaseSpec, run: Mapping[str, Any]) -> None:
    """Recheck the manifest's full-diagonal, PAF, and filtering requirements."""

    context = case.case_id
    validation = require_mapping(run.get("validation"), f"{context}.validation")
    require(run.get("status") == "accepted", f"{context} was not accepted")
    require(validation.get("accepted") is True, f"{context} validation was not accepted")
    require(validation.get("errors") == [], f"{context} retained validation errors")
    process = require_mapping(run.get("process"), f"{context}.process")
    require(int(process.get("exit_code", -1)) == 0, f"{context} minimap2 did not exit cleanly")
    require(process.get("timed_out") is False, f"{context} timed out")
    require(process.get("interrupted") is False, f"{context} was interrupted")

    first_path = require_mapping(validation.get("first_path"), f"{context}.first path")
    coordinates = require_mapping(
        first_path.get("declared_primary_coordinates"),
        f"{context}.declared primary coordinates",
    )
    expected_path = require_mapping(case.expectation.get("primary_path"), f"{context}.expectation.primary_path")
    require(coordinates.get("declared") is True, f"{context} lacks a declared coordinate path")
    path_count = int(coordinates.get("path_anchor_count", -1))
    matching_count = int(coordinates.get("matching_anchor_count", -1))
    require(path_count >= int(case.expectation.get("minimum_path_anchors", 0)), f"{context} path is too short")
    require(matching_count == path_count, f"{context} first path is not entirely on the declared diagonal")
    require(float(coordinates.get("matching_fraction", -1.0)) == 1.0, f"{context} diagonal fraction is not one")
    require(float(coordinates.get("required_fraction", -1.0)) == float(expected_path["minimum_fraction_on_declared_path"]), f"{context} path requirement changed")
    for endpoint in ("start_anchor", "terminal_anchor"):
        anchor = require_mapping(coordinates.get(endpoint), f"{context}.{endpoint}")
        require(anchor.get("target_name") == expected_path["target_name"], f"{context}.{endpoint} target differs")
        require(anchor.get("strand") == expected_path["strand"], f"{context}.{endpoint} strand differs")
        require(int(anchor.get("query_end", -1)) - int(anchor.get("target_end", -1)) == int(expected_path["diagonal"]), f"{context}.{endpoint} diagonal differs")

    ordering = require_mapping(first_path.get("candidate_ordering"), f"{context}.candidate ordering")
    if case.expectation.get("require_unique_top_score", False):
        require(ordering.get("unique_maximum_score") is True, f"{context} top score is tied")
        require(int(ordering.get("maximum_score_candidate_count", -1)) == 1, f"{context} has multiple maximum-score ends")

    paf = require_mapping(validation.get("paf"), f"{context}.paf")
    paf_expectation = require_mapping(case.expectation.get("paf"), f"{context}.expectation.paf")
    require(int(paf.get("valid_record_count", 0)) >= int(paf_expectation.get("minimum_record_count", 1)), f"{context} has too few PAF records")
    matching_records = require_list(paf.get("matching_records"), f"{context}.matching PAF records")
    require(int(paf.get("matching_qualified_record_count", 0)) >= 1 and matching_records, f"{context} lacks a qualifying PAF record")
    qualifies = False
    for record_value in matching_records:
        record = require_mapping(record_value, f"{context}.PAF record")
        query_length = int(record.get("query_length", 0))
        target_length = int(record.get("target_length", 0))
        query_coverage = (int(record.get("query_end", 0)) - int(record.get("query_start", 0))) / query_length if query_length else 0.0
        target_coverage = (int(record.get("target_end", 0)) - int(record.get("target_start", 0))) / target_length if target_length else 0.0
        if (
            record.get("query_name") == paf_expectation["required_query_name"]
            and record.get("target_name") == paf_expectation["required_target_name"]
            and record.get("strand") == paf_expectation["required_strand"]
            and query_length == int(case.query["length"])
            and target_length == int(case.reference["length"])
            and query_coverage >= float(paf_expectation.get("minimum_query_coverage_fraction", 0.0))
            and target_coverage >= float(paf_expectation.get("minimum_target_coverage_fraction", 0.0))
        ):
            qualifies = True
            break
    require(qualifies, f"{context} stored PAF evidence does not satisfy its manifest")

    telemetry = require_mapping(validation.get("telemetry"), f"{context}.telemetry")
    require(telemetry.get("parse_errors") == [], f"{context} has telemetry parse errors")
    require(telemetry.get("instrumentation_errors") == [], f"{context} has instrumentation errors")
    query_filter = require_mapping(telemetry.get("query_filter"), f"{context}.query filter")
    raw_minimizers = int(query_filter.get("raw_query_minimizers", -1))
    retained_minimizers = int(query_filter.get("query_minimizers_after_query_filter", -1))
    require(raw_minimizers > 0, f"{context} has no query minimizers")
    if case.expectation.get("require_no_query_minimizer_filter_loss", False):
        require(raw_minimizers == retained_minimizers, f"{context} lost query minimizers to filtering")
        require(int(query_filter.get("removed_query_minimizers", -1)) == 0, f"{context} reports removed query minimizers")

    frequency = require_mapping(telemetry.get("seed_frequency"), f"{context}.seed frequency")
    frequency_expectation = require_mapping(case.expectation.get("seed_frequency"), f"{context}.expectation.seed frequency")
    require(int(frequency.get("filtered_count", -1)) == int(frequency_expectation["required_initial_filtered_count"]), f"{context} reference filtering differs")
    allowed = {int(value) for value in frequency_expectation["allowed_initial_reference_occurrences"]}
    observed = {int(value) for value in frequency.get("observed_reference_occurrence_values", [])}
    require(observed and observed <= allowed, f"{context} reference occurrence values {sorted(observed)} exceed {sorted(allowed)}")


def audit_case(case: CaseSpec, run: Mapping[str, Any], raw_case: Path) -> CaseMetrics:
    """Validate one accepted result and independently derive its L=4 metrics."""

    validate_paf_and_path(case, run)
    context = case.case_id
    record = find_initial_l4_record(run, context)
    dump_path = initial_dump_path(run, raw_case, context)
    dump = parse_dump_independently(dump_path)
    require(dump.call_id == int(record["call_id"]), f"{context} dump call differs")
    require(dump.anchor_count == int(record["anchor_count"]), f"{context} dump anchor count differs")
    require(dump.candidate_end_count == int(record["candidate_end_count"]), f"{context} dump candidate count differs")
    require(dump.min_score == int(record["min_score"]), f"{context} dump minimum score differs")
    require(dump.candidates, f"{context} initial DP dump has no ranked candidate end")
    _, independently_ranked_score, independently_ranked_index = dump.candidates[-1]
    start_index = int(record["start_index"])
    require(start_index == independently_ranked_index, f"{context} telemetry start index differs from raw Z/f ranking")
    require(int(record["start_score"]) == independently_ranked_score, f"{context} telemetry start score differs from raw Z/f ranking")
    require(dump.scores[start_index] == independently_ranked_score, f"{context} ranked start score differs from f[]")
    counters = replay_runl4(start_index, dump.predecessors)
    for field in COUNTER_FIELDS:
        require(int(record.get(field, -1)) == counters[field], f"{context} L=4 {field} differs from independent replay")
    require(counters["emulation_error"] == 0, f"{context} independent L=4 replay failed")

    validation = require_mapping(run.get("validation"), f"{context}.validation")
    first_path = require_mapping(validation.get("first_path"), f"{context}.first path")
    stored_l4 = require_mapping(require_mapping(first_path.get("by_lane"), f"{context}.by lane").get(str(LANES)), f"{context}.stored L=4")
    for field in COUNTER_FIELDS:
        if field in stored_l4:
            require(int(stored_l4[field]) == counters[field], f"{context} first-path L=4 {field} differs")
    require(int(stored_l4.get("scalar_validation_error", -1)) == 0, f"{context} scalar path validation failed")
    require(int(stored_l4.get("contiguous_mismatch", -1)) == counters["contiguous_mismatch"], f"{context} stored contiguous mismatch differs")

    repeat_stats = require_mapping(first_path.get("realized_repeat_statistics"), f"{context}.repeat statistics")
    selected_path_anchors = int(repeat_stats.get("selected_path_anchor_count", -1))
    grouped_anchors = int(repeat_stats.get("anchors_in_group_larger_than_one", -1))
    require(selected_path_anchors == counters["primary_path_anchors"], f"{context} selected path count differs from replay")
    grouped_fraction = grouped_anchors / selected_path_anchors
    require(math.isclose(grouped_fraction, float(repeat_stats.get("fraction_in_group_larger_than_one", -1.0)), rel_tol=0.0, abs_tol=1e-15), f"{context} grouped-path fraction differs from counts")

    parameters = require_mapping(case.parameters, f"{context}.parameters")
    independent = parameters.get("similarity_level") == "independent_control"
    a_length = int(parameters.get("a_length", 0))
    require(a_length > 0, f"{context} lacks a positive A length")
    reference_sequence = read_fasta_sequence(Path(str(case.reference["resolved_path"])))
    query_sequence = read_fasta_sequence(Path(str(case.query["resolved_path"])))
    require(reference_sequence == query_sequence, f"{context} reference and query are not byte-identical")
    with Path(str(case.construction_artifact["resolved_path"])).open("r", encoding="utf-8") as source:
        construction = require_mapping(json.load(source), f"{context}.construction")
    segments = {
        str(segment["name"]): require_mapping(segment, f"{context}.segment")
        for segment in require_list(construction.get("segments"), f"{context}.segments")
    }
    require({"A", "A_prime"} <= set(segments), f"{context} lacks A or A_prime segment")
    a_segment = segments["A"]
    ap_segment = segments["A_prime"]
    a = reference_sequence[int(a_segment["start"]):int(a_segment["end"])]
    ap = reference_sequence[int(ap_segment["start"]):int(ap_segment["end"])]
    require(len(a) == a_length and len(ap) == a_length, f"{context} block geometry differs")
    substitution_positions = tuple(index for index, pair in enumerate(zip(a, ap, strict=True)) if pair[0] != pair[1])
    substitution_count = len(substitution_positions)
    declared_substitutions = parameters.get("substitution_count")
    nominal: Decimal | None
    if independent:
        require(declared_substitutions is None, f"{context} independent control declares a nested count")
        nominal = None
    else:
        nominal = decimal_level(parameters.get("similarity_level"), f"{context}.similarity_level")
        require(int(declared_substitutions) == substitution_count, f"{context} FASTA substitution count differs from manifest")
        require(int(construction.get("declared_substitution_count_between_A_and_A_prime", -1)) == substitution_count, f"{context} construction substitution count differs")

    b = counters["fast_batches"]
    e = counters["loop_evaluations"]
    p = counters["bounds_pass"]
    q = counters["primary_path_anchors"]
    fire = percent(b, e, f"{context} Fire_4")
    contiguous = percent(b, p, f"{context} Contiguous_4")
    coverage = percent(LANES * b, q, f"{context} Coverage_4")
    require(0.0 <= fire <= 100.0 and 0.0 <= contiguous <= 100.0 and 0.0 <= coverage <= 100.0, f"{context} L=4 percentage is outside [0,100]")
    return CaseMetrics(
        case_id=context,
        replicate_id=case.replicate_id,
        seed=case.seed,
        independent_control=independent,
        nominal_divergence_fraction=float(nominal) if nominal is not None else None,
        substitution_count=substitution_count,
        realized_divergence_percent=100.0 * substitution_count / a_length,
        path_anchor_count=q,
        grouped_path_percent=100.0 * grouped_fraction,
        fast_batches=b,
        loop_evaluations=e,
        bounds_pass=p,
        fire_percent=fire,
        contiguous_percent=contiguous,
        coverage_percent=coverage,
        l4_counter_sha256=canonical_json_sha256(counters),
    )


def preservation_probability(block_length: int, substitutions: int, span: int) -> float:
    """Return H_r(M,s) for fixed-count sampling without replacement."""

    require(block_length > 0, "block length must be positive")
    require(0 <= substitutions <= block_length, "substitution count is outside the block")
    require(0 <= span <= block_length, "preservation span is outside the block")
    if substitutions > block_length - span:
        return 0.0
    result = 1.0
    for offset in range(span):
        result *= (block_length - substitutions - offset) / (block_length - offset)
    return result


def range_summary(values: Sequence[float]) -> dict[str, float]:
    """Return deterministic median, minimum, and maximum statistics."""

    require(len(values) == REPLICATE_COUNT, f"expected {REPLICATE_COUNT} replicate values")
    return {
        "median": float(median(values)),
        "minimum": float(min(values)),
        "maximum": float(max(values)),
    }


def aggregate_levels(
    metrics: Sequence[CaseMetrics], block_length: int, sequence_length: int
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Aggregate all 81 nested levels and the separate independent controls."""

    nested: dict[Decimal, list[CaseMetrics]] = defaultdict(list)
    controls: list[CaseMetrics] = []
    for item in metrics:
        if item.independent_control:
            controls.append(item)
        else:
            require(item.nominal_divergence_fraction is not None, f"{item.case_id} lacks its nominal level")
            nested[Decimal(str(item.nominal_divergence_fraction))].append(item)
    require(set(nested) == set(EXPECTED_LEVELS), "audited result levels are not the exact 0--20 percent grid")
    require(len(controls) == REPLICATE_COUNT, "independent control count differs")
    alpha = 2.0 * block_length / sequence_length
    levels: list[dict[str, Any]] = []
    for level in EXPECTED_LEVELS:
        replicates = sorted(nested[level], key=lambda item: item.replicate_id)
        require(len(replicates) == REPLICATE_COUNT, f"level {level} lacks three replicates")
        substitutions = {item.substitution_count for item in replicates}
        require(len(substitutions) == 1, f"level {level} has unequal fixed substitution counts")
        substitution_count = next(iter(substitutions))
        realized_values = {item.realized_divergence_percent for item in replicates}
        require(len(realized_values) == 1, f"level {level} has unequal realized divergence")
        h15 = preservation_probability(block_length, substitution_count, 15)
        h24 = preservation_probability(block_length, substitution_count, 24)
        levels.append(
            {
                "nominal_divergence_fraction": float(level),
                "nominal_divergence_percent": float(100 * level),
                "substitution_count": substitution_count,
                "realized_divergence_percent": next(iter(realized_values)),
                "replicates": [asdict(item) for item in replicates],
                "grouped_path_percent": range_summary([item.grouped_path_percent for item in replicates]),
                "fire_4_percent": range_summary([item.fire_percent for item in replicates]),
                "contiguous_4_percent": range_summary([item.contiguous_percent for item in replicates]),
                "coverage_4_percent": range_summary([item.coverage_percent for item in replicates]),
                "theory": {
                    "H_15": h15,
                    "H_24": h24,
                    "length_weight_alpha": alpha,
                    "weighted_H_15_percent": 100.0 * alpha * h15,
                    "weighted_H_24_percent": 100.0 * alpha * h24,
                },
            }
        )
    control_summary = {
        "replicates": [asdict(item) for item in sorted(controls, key=lambda item: item.replicate_id)],
        "realized_divergence_percent": range_summary([item.realized_divergence_percent for item in controls]),
        "grouped_path_percent": range_summary([item.grouped_path_percent for item in controls]),
        "fire_4_percent": range_summary([item.fire_percent for item in controls]),
        "contiguous_4_percent": range_summary([item.contiguous_percent for item in controls]),
        "coverage_4_percent": range_summary([item.coverage_percent for item in controls]),
    }
    return levels, control_summary


def validate_nested_inputs(cases: Sequence[CaseSpec]) -> tuple[int, int, dict[str, Any]]:
    """Validate the exact grid and prove that each seed uses nested mutation sets."""

    require(len(cases) == EXPECTED_CASE_COUNT, f"manifest has {len(cases)} cases, expected {EXPECTED_CASE_COUNT}")
    require(all(case.group == "mosaic" for case in cases), "dense manifest must be mosaic-only")
    by_replicate: dict[str, list[CaseSpec]] = defaultdict(list)
    for case in cases:
        by_replicate[case.replicate_id].append(case)
    require(len(by_replicate) == REPLICATE_COUNT, "dense manifest must contain three replicates")
    block_lengths = {int(case.parameters.get("a_length", 0)) for case in cases}
    sequence_lengths = {int(case.reference["length"]) for case in cases}
    require(len(block_lengths) == 1 and min(block_lengths) > 0, "A length differs across cases")
    require(len(sequence_lengths) == 1 and min(sequence_lengths) > 0, "sequence length differs across cases")
    block_length = next(iter(block_lengths))
    sequence_length = next(iter(sequence_lengths))
    replicate_checks: dict[str, Any] = {}
    replicate_seeds: set[int] = set()

    for replicate_id, replicate_cases in sorted(by_replicate.items()):
        seeds = {case.seed for case in replicate_cases}
        require(len(seeds) == 1, f"{replicate_id} uses multiple master seeds")
        replicate_seeds.update(seeds)
        controls = [case for case in replicate_cases if case.parameters.get("similarity_level") == "independent_control"]
        nested_cases = [case for case in replicate_cases if case not in controls]
        require(len(controls) == 1, f"{replicate_id} must contain one independent control")
        require(len(nested_cases) == LEVEL_COUNT, f"{replicate_id} must contain 81 nested levels")

        level_cases: dict[Decimal, CaseSpec] = {}
        mutation_maps: dict[Decimal, dict[int, int]] = {}
        a_digests: set[str] = set()
        for case in nested_cases:
            level = decimal_level(case.parameters.get("similarity_level"), f"{case.case_id}.similarity_level")
            require(level not in level_cases, f"{replicate_id} repeats level {level}")
            level_cases[level] = case
            expected_count = int((level * block_length).to_integral_value(rounding=ROUND_HALF_UP))
            require(int(case.parameters.get("substitution_count", -1)) == expected_count, f"{case.case_id} count does not use half-up rounding")

            sequence = read_fasta_sequence(Path(str(case.reference["resolved_path"])))
            with Path(str(case.construction_artifact["resolved_path"])).open("r", encoding="utf-8") as source:
                construction = require_mapping(json.load(source), f"{case.case_id}.construction")
            segments = {str(item["name"]): item for item in construction["segments"]}
            a = sequence[int(segments["A"]["start"]):int(segments["A"]["end"])]
            ap = sequence[int(segments["A_prime"]["start"]):int(segments["A_prime"]["end"])]
            require(len(a) == block_length and len(ap) == block_length, f"{case.case_id} block geometry differs")
            differences = {
                index: pair[1]
                for index, pair in enumerate(zip(a, ap, strict=True))
                if pair[0] != pair[1]
            }
            require(len(differences) == expected_count, f"{case.case_id} actual mutation count differs")
            mutation_maps[level] = differences
            a_digests.add(hashlib.sha256(a).hexdigest())

        require(set(level_cases) == set(EXPECTED_LEVELS), f"{replicate_id} does not contain the exact grid")
        require(len(a_digests) == 1, f"{replicate_id} changes the source A block across levels")
        previous: dict[int, int] = {}
        previous_count = -1
        counts: list[int] = []
        for level in EXPECTED_LEVELS:
            current = mutation_maps[level]
            require(
                all(current.get(position) == base for position, base in previous.items()),
                f"{replicate_id} mutations at {level} do not extend the prior prefix",
            )
            require(len(current) > previous_count, f"{replicate_id} mutation count is not strictly increasing at {level}")
            previous = current
            previous_count = len(current)
            counts.append(len(current))
        replicate_checks[replicate_id] = {
            "seed": next(iter(seeds)),
            "level_count": len(nested_cases),
            "independent_control_count": len(controls),
            "minimum_substitution_count": min(counts),
            "maximum_substitution_count": max(counts),
            "strictly_increasing_counts": True,
            "nested_mutation_sets_verified_from_fasta": True,
        }
    require(len(replicate_seeds) == REPLICATE_COUNT, "replicates do not use three distinct seeds")
    return block_length, sequence_length, replicate_checks


def result_metric_signature(run: Mapping[str, Any]) -> dict[str, Any]:
    """Select deterministic evidence used for old-versus-dense cross-checks."""

    record = find_initial_l4_record(run, str(run.get("case_id", "old case")))
    validation = require_mapping(run.get("validation"), "cross-check validation")
    first_path = require_mapping(validation.get("first_path"), "cross-check first path")
    artifacts = require_mapping(run.get("input_artifacts"), "cross-check input artifacts")
    reference = require_mapping(artifacts.get("reference"), "cross-check reference")
    query = require_mapping(artifacts.get("query"), "cross-check query")
    parameters = require_mapping(run.get("parameters"), "cross-check parameters")
    coordinates = require_mapping(
        first_path.get("declared_primary_coordinates"),
        "cross-check declared coordinates",
    )

    def normalized_anchor(value: Any) -> dict[str, Any] | None:
        if value is None:
            return None
        anchor = require_mapping(value, "cross-check coordinate anchor")
        return {
            key: anchor.get(key)
            for key in (
                "index",
                "target_end",
                "query_end",
                "span",
                "strand",
                "printed_diagonal_delta",
            )
        }

    paf = require_mapping(validation.get("paf"), "cross-check PAF")
    normalized_paf_records = []
    for record_value in require_list(paf.get("matching_records"), "cross-check PAF records"):
        paf_record = require_mapping(record_value, "cross-check PAF record")
        normalized_paf_records.append(
            {
                key: paf_record.get(key)
                for key in (
                    "query_length",
                    "query_start",
                    "query_end",
                    "strand",
                    "target_length",
                    "target_start",
                    "target_end",
                    "residue_matches",
                    "alignment_block_length",
                    "mapping_quality",
                )
            }
        )
    return {
        "input_sequences": {
            "reference_sequence_sha256": reference.get("sequence_sha256"),
            "reference_length": reference.get("length"),
            "query_sequence_sha256": query.get("sequence_sha256"),
            "query_length": query.get("length"),
        },
        "geometry": {
            "a_length": int(parameters.get("a_length", -1)),
            "a_to_a_prime_offset": int(parameters.get("a_to_a_prime_offset", -1)),
            "substitution_count": parameters.get("substitution_count"),
            "independent_control": parameters.get("similarity_level")
            == "independent_control",
        },
        "l4_counters": {field: int(record[field]) for field in COUNTER_FIELDS},
        "repeat_statistics": first_path.get("realized_repeat_statistics"),
        "declared_primary_coordinates": {
            "declared": coordinates.get("declared"),
            "path_anchor_count": coordinates.get("path_anchor_count"),
            "matching_anchor_count": coordinates.get("matching_anchor_count"),
            "matching_fraction": coordinates.get("matching_fraction"),
            "required_fraction": coordinates.get("required_fraction"),
            "start_anchor": normalized_anchor(coordinates.get("start_anchor")),
            "terminal_anchor": normalized_anchor(coordinates.get("terminal_anchor")),
        },
        "paf": {
            "valid_record_count": paf.get("valid_record_count"),
            "matching_qualified_record_count": paf.get(
                "matching_qualified_record_count"
            ),
            "matching_records_without_names": normalized_paf_records,
        },
        "query_filter": require_mapping(validation.get("telemetry"), "cross-check telemetry").get("query_filter"),
        "seed_frequency": require_mapping(validation.get("telemetry"), "cross-check telemetry").get("seed_frequency"),
    }


def overlap_key(run: Mapping[str, Any]) -> tuple[str, str]:
    """Identify a coarse/dense overlap without depending on case-id formatting."""

    replicate_id = str(run.get("replicate_id", ""))
    parameters = require_mapping(run.get("parameters"), "cross-check parameters")
    if parameters.get("similarity_level") == "independent_control":
        return replicate_id, "independent"
    return replicate_id, f"s{int(parameters.get('substitution_count', -1))}"


def cross_check_old_results(
    old_path: Path | None,
    dense_runs: Mapping[str, Mapping[str, Any]],
    dense_binary_digest: str,
) -> dict[str, Any]:
    """Cross-check all 24 controls and coarse levels shared by the old sweep."""

    if old_path is None:
        return {"performed": False, "reason": "--old-results was not supplied"}
    old_document = load_yaml_mapping(old_path)
    old_binary = require_mapping(old_document.get("minimap2"), "old-results.minimap2")
    require(
        old_binary.get("sha256") == dense_binary_digest,
        "old and dense sweeps used different minimap2 binaries",
    )
    old_runs_list = require_list(old_document.get("runs"), "old-results.runs")
    old_runs: dict[tuple[str, str], Mapping[str, Any]] = {}
    for run_value in old_runs_list:
        run = require_mapping(run_value, "old-results run")
        if run.get("group") != "mosaic":
            continue
        key = overlap_key(run)
        require(key not in old_runs, f"old results repeat mosaic key {key}")
        old_runs[key] = run

    dense_by_key: dict[tuple[str, str], Mapping[str, Any]] = {}
    for run in dense_runs.values():
        require(run.get("group") == "mosaic", "dense results contain a non-mosaic run")
        key = overlap_key(run)
        require(key not in dense_by_key, f"dense results repeat mosaic key {key}")
        dense_by_key[key] = run

    matches: list[dict[str, Any]] = []
    for key in sorted(set(dense_by_key) & set(old_runs)):
        dense = dense_by_key[key]
        old = old_runs[key]
        require(old.get("status") == "accepted", f"old overlap {key} was not accepted")
        dense_signature = result_metric_signature(dense)
        old_signature = result_metric_signature(old)
        require(dense_signature == old_signature, f"old overlap {key} differs from dense evidence")
        matches.append(
            {
                "replicate_id": key[0],
                "overlap_key": key[1],
                "dense_case_id": dense.get("case_id"),
                "old_case_id": old.get("case_id"),
                "signature_sha256": canonical_json_sha256(dense_signature),
            }
        )
    require(len(matches) == 24, f"old coarse cross-check found {len(matches)} overlaps, expected 24")
    return {
        "performed": True,
        "old_results_path": str(old_path.resolve()),
        "old_results_sha256": sha256_file(old_path),
        "overlap_count": len(matches),
        "all_overlap_signatures_equal": True,
        "overlaps": matches,
    }


def format_coordinate(value: float) -> str:
    """Format a chart coordinate deterministically without scientific notation."""

    return f"{value:.8f}".rstrip("0").rstrip(".") or "0"


def coordinate_macro(name: str, points: Iterable[tuple[float, float]]) -> str:
    """Create a wrapped TeX macro containing TikZ plot coordinates."""

    encoded = [f"({format_coordinate(x)},{format_coordinate(y)})" for x, y in points]
    lines = [f"\\def\\{name}{{%"]
    for offset in range(0, len(encoded), 4):
        lines.append("  " + " ".join(encoded[offset:offset + 4]) + "%")
    lines.append("}")
    return "\n".join(lines)


def table_stat(value: Mapping[str, Any]) -> str:
    """Format one median and observed range for a compact LaTeX row."""

    return (
        f"{float(value['median']):.2f} "
        f"[{float(value['minimum']):.2f}, {float(value['maximum']):.2f}]"
    )


def latex_document(
    levels: Sequence[Mapping[str, Any]],
    control: Mapping[str, Any],
    manifest_digest: str,
    aggregate_digest: str,
) -> bytes:
    """Build native LaTeX data macros for the dense figure and selected table."""

    metric_specs = (
        ("RunLDenseGrouped", "grouped_path_percent"),
        ("RunLDenseFireFour", "fire_4_percent"),
        ("RunLDenseCoverageFour", "coverage_4_percent"),
    )
    chunks = [
        "% Generated by runl_dense_divergence_audit.py. Do not edit by hand.",
        f"% manifest sha256: {manifest_digest}",
        f"% runner aggregate sha256: {aggregate_digest}",
    ]
    for macro_prefix, key in metric_specs:
        for suffix, statistic in (("MedianCoordinates", "median"), ("MinimumCoordinates", "minimum"), ("MaximumCoordinates", "maximum")):
            chunks.append(
                coordinate_macro(
                    macro_prefix + suffix,
                    (
                        (float(level["realized_divergence_percent"]), float(level[key][statistic]))
                        for level in levels
                    ),
                )
            )
    chunks.append(
        coordinate_macro(
            "RunLDenseTheoryHfifteenCoordinates",
            (
                (float(level["realized_divergence_percent"]), float(level["theory"]["weighted_H_15_percent"]))
                for level in levels
            ),
        )
    )
    chunks.append(
        coordinate_macro(
            "RunLDenseTheoryHtwentyfourCoordinates",
            (
                (float(level["realized_divergence_percent"]), float(level["theory"]["weighted_H_24_percent"]))
                for level in levels
            ),
        )
    )

    selected = {
        Decimal(str(level["nominal_divergence_fraction"])): level
        for level in levels
    }
    rows = ["\\def\\RunLDenseSelectedTableRows{%"]
    for configured in SELECTED_LEVELS:
        level = selected[configured]
        label = "0" if configured == 0 else f"{configured:.4f}"
        rows.append(
            f"  \\(\\delta={label}\\) & {int(level['substitution_count'])} & "
            f"{float(level['realized_divergence_percent']):.3f} & "
            f"{table_stat(level['grouped_path_percent'])} & "
            f"{table_stat(level['fire_4_percent'])} & "
            f"{table_stat(level['coverage_4_percent'])} \\\\%"
        )
    rows.append("}")
    chunks.append("\n".join(rows))
    chunks.append(
        "\\def\\RunLDenseIndependentTableRow{%\n"
        f"  Independent \\(A'\\) & \\text{{n/a}} & "
        f"{float(control['realized_divergence_percent']['median']):.3f} & "
        f"{table_stat(control['grouped_path_percent'])} & "
        f"{table_stat(control['fire_4_percent'])} & "
        f"{table_stat(control['coverage_4_percent'])} \\\\%\n"
        "}"
    )
    return ("\n\n".join(chunks) + "\n").encode("ascii")


def selected_table_rows(levels: Sequence[Mapping[str, Any]]) -> list[dict[str, Any]]:
    """Return the small set of rows intended for the prose table."""

    by_level = {
        Decimal(str(level["nominal_divergence_fraction"])): level
        for level in levels
    }
    return [
        {
            key: value
            for key, value in by_level[level].items()
            if key != "replicates"
        }
        for level in SELECTED_LEVELS
    ]


def ensure_output_policy(args: argparse.Namespace) -> None:
    """Reject accidental evidence overwrite and unrequested output replacement."""

    evidence = {
        args.manifest.resolve(),
        args.results.resolve(),
        args.raw_dir.resolve(),
        args.shard_dir.resolve(),
    }
    if args.old_results is not None:
        evidence.add(args.old_results.resolve())
    outputs = {args.analysis_yaml.resolve(), args.latex_data.resolve()}
    require(len(outputs) == 2, "analysis YAML and LaTeX data paths must differ")
    require(not outputs & evidence, "an output path overlaps experiment evidence")
    if not args.replace:
        existing = [str(path) for path in outputs if path.exists()]
        require(not existing, f"refusing to replace existing outputs: {existing}")


def validate_dense_analysis_sources(
    runs: Mapping[str, Mapping[str, Any]],
) -> dict[str, str]:
    """Authenticate the exact source mapping embedded in every dense shard."""

    observed_mappings: dict[str, dict[str, str]] = {}
    for case_id, run in runs.items():
        provenance = require_mapping(run.get("provenance"), f"{case_id}.provenance")
        raw_mapping = require_mapping(
            provenance.get("analysis_source_sha256"),
            f"{case_id}.analysis source mapping",
        )
        normalized: dict[str, str] = {}
        for raw_name, raw_digest in raw_mapping.items():
            name = str(raw_name)
            digest = str(raw_digest)
            require(Path(name).name == name, f"{case_id} analysis source name is not a basename")
            require(len(digest) == 64 and all(character in "0123456789abcdef" for character in digest), f"{case_id} analysis source {name} has an invalid digest")
            normalized[name] = digest
        mapping_digest = canonical_json_sha256(normalized)
        observed_mappings[mapping_digest] = normalized
    require(len(observed_mappings) == 1, "dense shards do not share one analysis-source mapping")
    observed = next(iter(observed_mappings.values()))
    require(set(observed) == EXPECTED_DENSE_ANALYSIS_SOURCES, "dense analysis-source set differs from wrapper, core, and common helper")
    source_root = Path(__file__).resolve().parent
    for name, expected_digest in observed.items():
        source_path = source_root / name
        require(source_path.is_file(), f"dense analysis source is missing: {source_path}")
        require(sha256_file(source_path) == expected_digest, f"dense analysis source digest differs: {name}")
    return dict(sorted(observed.items()))


def main() -> int:
    """Validate all dense-sweep evidence and publish derived artifacts."""

    args = parse_args()
    ensure_output_policy(args)
    manifest_path = args.manifest.resolve()
    results_path = args.results.resolve()
    raw_dir = args.raw_dir.resolve()
    shard_dir = args.shard_dir.resolve()
    require(manifest_path.is_file(), f"missing manifest: {manifest_path}")
    require(results_path.is_file(), f"missing runner aggregate: {results_path}")
    require(raw_dir.is_dir(), f"missing raw directory: {raw_dir}")
    require(shard_dir.is_dir(), f"missing shard directory: {shard_dir}")

    manifest, cases = load_manifest(manifest_path)
    manifest_digest = sha256_file(manifest_path)
    block_length, sequence_length, replicate_checks = validate_nested_inputs(cases)
    aggregate = load_yaml_mapping(results_path)
    aggregate_digest = sha256_file(results_path)
    require(aggregate.get("schema_version") == 1, "runner aggregate schema differs")
    require(aggregate.get("experiment_id") == "runl_repeat_disruption", "runner aggregate experiment id differs")
    require(aggregate.get("experiment_status") == "complete_pass", "runner aggregate is not complete_pass")
    require(aggregate.get("lane_widths") == manifest.get("lane_widths"), "runner aggregate lane widths differ from manifest")
    aggregate_manifest = require_mapping(aggregate.get("manifest"), "aggregate.manifest")
    require(aggregate_manifest.get("sha256") == manifest_digest, "runner aggregate is bound to a different manifest")
    selection = require_mapping(aggregate.get("selection"), "aggregate.selection")
    require(selection.get("is_full_manifest") is True, "runner aggregate is not a full-manifest run")
    expected_case_ids = {case.case_id for case in cases}
    require(set(selection.get("selected_case_ids", [])) == expected_case_ids, "runner aggregate selected case ids differ")
    summary = require_mapping(aggregate.get("summary"), "aggregate.summary")
    for key, expected in (
        ("manifest_case_count", EXPECTED_CASE_COUNT),
        ("selected_case_count", EXPECTED_CASE_COUNT),
        ("attempted_global", EXPECTED_CASE_COUNT),
        ("accepted", EXPECTED_CASE_COUNT),
        ("rejected", 0),
        ("pending_global", 0),
    ):
        require(int(summary.get(key, -1)) == expected, f"aggregate.summary.{key} differs")

    minimap2 = require_mapping(aggregate.get("minimap2"), "aggregate.minimap2")
    binary = Path(str(minimap2.get("path", ""))).resolve()
    binary_digest = str(minimap2.get("sha256", ""))
    require(binary.is_file(), f"aggregate minimap2 binary is missing: {binary}")
    require(sha256_file(binary) == binary_digest, "aggregate minimap2 binary digest differs")
    runs_list = require_list(aggregate.get("runs"), "aggregate.runs")
    require(len(runs_list) == EXPECTED_CASE_COUNT, "aggregate run count differs")
    runs: dict[str, Mapping[str, Any]] = {}
    for run_value in runs_list:
        run = require_mapping(run_value, "aggregate run")
        case_id = str(run.get("case_id", ""))
        require(case_id not in runs, f"aggregate repeats case {case_id}")
        runs[case_id] = run
    require(set(runs) == expected_case_ids, "aggregate run case ids differ from manifest")
    dense_analysis_sources = validate_dense_analysis_sources(runs)

    shard_files = {path.name for path in shard_dir.iterdir() if path.is_file()}
    require(shard_files == {f"{case_id}.json" for case_id in expected_case_ids}, "shard file set differs from manifest")
    raw_case_dirs = {path.name for path in raw_dir.iterdir() if path.is_dir()}
    require(raw_case_dirs == expected_case_ids, "raw case directory set differs from manifest")

    metrics: list[CaseMetrics] = []
    original_analysis_source_digests = core_runner.analysis_source_digests
    core_runner.analysis_source_digests = lambda: dict(dense_analysis_sources)
    try:
        for case in cases:
            shard_path = shard_dir / f"{case.case_id}.json"
            shard = core_runner.validate_existing_result(
                case,
                raw_dir,
                shard_path,
                manifest_path,
                manifest_digest,
                binary,
                binary_digest,
            )
            require(canonical_json_sha256(shard) == canonical_json_sha256(runs[case.case_id]), f"aggregate run differs from shard for {case.case_id}")
            metrics.append(audit_case(case, shard, raw_dir / case.case_id))
    finally:
        core_runner.analysis_source_digests = original_analysis_source_digests

    levels, control = aggregate_levels(metrics, block_length, sequence_length)
    cross_check = cross_check_old_results(args.old_results, runs, binary_digest)
    latex_bytes = latex_document(levels, control, manifest_digest, aggregate_digest)
    coordinates_digest = atomic_write_bytes(args.latex_data.resolve(), (latex_bytes,))

    document = {
        "schema_version": SCHEMA_VERSION,
        "experiment_id": EXPERIMENT_ID,
        "status": "complete_pass",
        "generated_at_utc": utc_timestamp(),
        "inputs": {
            "manifest_path": str(manifest_path),
            "manifest_sha256": manifest_digest,
            "runner_aggregate_path": str(results_path),
            "runner_aggregate_sha256": aggregate_digest,
            "raw_directory": str(raw_dir),
            "shard_directory": str(shard_dir),
            "coordinates_path": str(args.latex_data.resolve()),
            "coordinates_sha256": coordinates_digest,
            "minimap2_path": str(binary),
            "minimap2_sha256": binary_digest,
        },
        "provenance": {
            "auditor_path": str(Path(__file__).resolve()),
            "auditor_sha256": sha256_file(Path(__file__).resolve()),
            "runner_analysis_source_sha256": dense_analysis_sources,
            "manifest_generator_sha256": require_mapping(manifest.get("generation"), "manifest.generation").get("generator_sha256"),
        },
        "design": {
            "replicate_count": REPLICATE_COUNT,
            "nested_level_count": LEVEL_COUNT,
            "independent_control_count": REPLICATE_COUNT,
            "accepted_case_count": EXPECTED_CASE_COUNT,
            "nominal_divergence_start_percent": 0.0,
            "nominal_divergence_stop_percent": 20.0,
            "nominal_divergence_step_percent": 0.25,
            "block_length": block_length,
            "sequence_length": sequence_length,
            "lane_width": LANES,
            "fixed_count_rounding": "ROUND_HALF_UP",
        },
        "validation": {
            "manifest_and_input_digests": "passed",
            "aggregate_shard_and_raw_bindings": "passed",
            "all_cases_accepted": True,
            "exact_full_diagonal_expectations": "passed",
            "qualified_paf_expectations": "passed",
            "query_and_reference_filter_expectations": "passed",
            "independent_Run_4_replay_from_predecessor_dumps": "passed",
            "nested_mutation_sets_verified_from_fasta": True,
            "replicates": replicate_checks,
        },
        "metric_definitions": {
            "B_4": "number of loop evaluations for which Run_4 is true",
            "E_4": "number of first-path loop evaluations",
            "P_4": "number of evaluations that pass both array bounds",
            "q": "number of anchors consumed on the first predecessor path",
            "fire_4_percent": "100 * B_4 / E_4",
            "contiguous_4_percent": "100 * B_4 / P_4",
            "coverage_4_percent": "100 * 4 * B_4 / q",
            "grouped_path_percent": "100 times the selected-path fraction whose same-target group has size above one",
        },
        "levels": levels,
        "selected_table_rows": selected_table_rows(levels),
        "independent_control": control,
        "old_coarse_cross_check": cross_check,
    }
    encoded = yaml.safe_dump(
        document,
        sort_keys=False,
        allow_unicode=False,
        width=110,
    ).encode("utf-8")
    analysis_digest = atomic_write_bytes(args.analysis_yaml.resolve(), (encoded,))
    print(
        json.dumps(
            {
                "status": "complete_pass",
                "accepted_cases": len(metrics),
                "levels": len(levels),
                "analysis_yaml": str(args.analysis_yaml.resolve()),
                "analysis_yaml_sha256": analysis_digest,
                "latex_data": str(args.latex_data.resolve()),
                "latex_data_sha256": coordinates_digest,
                "old_cross_check_performed": bool(cross_check["performed"]),
            },
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AuditError, OSError, ValueError, yaml.YAMLError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2) from error
