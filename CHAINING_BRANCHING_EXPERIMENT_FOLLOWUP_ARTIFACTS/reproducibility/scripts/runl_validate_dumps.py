#!/usr/bin/env python3
"""Independently reconstruct Run_L counters from dumped minimap2 f/p arrays."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
from typing import Any

import yaml


LANE_WIDTHS = [4, 8, 16]
COMPARE_FIELDS = [
    "schema_version",
    "call_id",
    "stage",
    "lanes",
    "anchor_count",
    "candidate_end_count",
    "start_index",
    "start_score",
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
    "scalar_path_length",
    "scalar_validation_error",
    "emulation_error",
    "min_score",
    "has_ranked_end",
    "has_fast_batch",
]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def parse_dump(path: Path) -> dict[str, Any]:
    metadata: dict[str, Any] | None = None
    candidates: list[tuple[int, int, int]] = []
    scores: list[int] = []
    predecessors: list[int] = []
    with path.open("r", encoding="utf-8") as source:
        for line in source:
            fields = line.rstrip("\n").split("\t")
            if fields[0] == "META":
                metadata = {
                    "stage": fields[1],
                    "call_id": int(fields[2]),
                    "anchor_count": int(fields[3]),
                    "candidate_end_count": int(fields[4]),
                    "min_score": int(fields[5]),
                }
            elif fields[0] == "Z":
                candidates.append((int(fields[1]), int(fields[2]), int(fields[3])))
            elif fields[0] == "P":
                index, score, predecessor = map(int, fields[1:])
                if index != len(scores):
                    raise ValueError(f"{path}: non-consecutive P index {index}")
                scores.append(score)
                predecessors.append(predecessor)
            else:
                raise ValueError(f"{path}: unexpected row type {fields[0]}")
    if metadata is None:
        raise ValueError(f"{path}: no META row")
    if len(scores) != metadata["anchor_count"]:
        raise ValueError(f"{path}: P row count does not match META")
    if len(candidates) != metadata["candidate_end_count"]:
        raise ValueError(f"{path}: Z row count does not match META")
    return {
        "metadata": metadata,
        "candidates": candidates,
        "scores": scores,
        "predecessors": predecessors,
    }


def reconstruct(parsed: dict[str, Any], lanes: int) -> dict[str, Any]:
    metadata = parsed["metadata"]
    candidates = parsed["candidates"]
    predecessors = parsed["predecessors"]
    n = metadata["anchor_count"]
    start = candidates[-1][2] if candidates else -1
    start_score = candidates[-1][1] if candidates else -1

    scalar_j = start
    scalar_path_length = 0
    scalar_validation_error = 0
    while scalar_j >= 0 and scalar_path_length < n:
        if scalar_j >= n:
            scalar_validation_error = 1
            break
        next_j = predecessors[scalar_j]
        scalar_path_length += 1
        if next_j < -1 or next_j >= scalar_j:
            scalar_validation_error = 1
            break
        scalar_j = next_j
    if scalar_j >= 0 and scalar_path_length >= n:
        scalar_validation_error = 1

    j = start
    c = 0
    loop_evaluations = bounds_pass = 0
    bounds_fail_current = bounds_fail_capacity = bounds_fail_both = 0
    contiguous_mismatch = fast_batches = scalar_fallbacks = 0
    emulation_error = 0
    while j >= 0 and c < n:
        loop_evaluations += 1
        position_ok = j >= lanes - 1
        capacity_ok = c <= n - lanes
        if not position_ok or not capacity_ok:
            if not position_ok and not capacity_ok:
                bounds_fail_both += 1
            elif not position_ok:
                bounds_fail_current += 1
            else:
                bounds_fail_capacity += 1
            contiguous = False
        else:
            bounds_pass += 1
            contiguous = all(
                predecessors[j - offset] == j - offset - 1
                for offset in range(lanes)
            )

        if position_ok and capacity_ok and contiguous:
            fast_batches += 1
            j -= lanes
            c += lanes
        else:
            if position_ok and capacity_ok:
                contiguous_mismatch += 1
            scalar_fallbacks += 1
            if j >= n:
                emulation_error = 1
                break
            next_j = predecessors[j]
            c += 1
            if next_j < -1 or next_j >= j:
                emulation_error = 1
                break
            j = next_j

    return {
        "schema_version": 1,
        "call_id": metadata["call_id"],
        "stage": metadata["stage"],
        "lanes": lanes,
        "anchor_count": n,
        "candidate_end_count": len(candidates),
        "start_index": start,
        "start_score": start_score,
        "loop_evaluations": loop_evaluations,
        "bounds_pass": bounds_pass,
        "bounds_fail_current": bounds_fail_current,
        "bounds_fail_capacity": bounds_fail_capacity,
        "bounds_fail_both": bounds_fail_both,
        "contiguous_mismatch": contiguous_mismatch,
        "fast_batches": fast_batches,
        "scalar_fallbacks": scalar_fallbacks,
        "primary_path_anchors": c,
        "fast_covered_anchors": lanes * fast_batches,
        "scalar_path_length": scalar_path_length,
        "scalar_validation_error": scalar_validation_error,
        "emulation_error": emulation_error,
        "min_score": metadata["min_score"],
        "has_ranked_end": int(bool(candidates)),
        "has_fast_batch": int(fast_batches > 0),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--results", type=Path, default=Path("experiment_results.yaml"))
    parser.add_argument("--output", type=Path, default=Path("experiment_dump_validation.yaml"))
    args = parser.parse_args()
    results = yaml.safe_load(args.results.read_text(encoding="utf-8"))
    reports: list[dict[str, Any]] = []
    all_errors: list[str] = []

    for run in results["runs"]:
        if not run["dump_directory"]:
            continue
        dump_dir = Path(run["dump_directory"])
        for dump_path in sorted(dump_dir.glob("runl_call_*.tsv")):
            parsed = parse_dump(dump_path)
            metadata = parsed["metadata"]
            expected_candidates = sorted(
                (score, index)
                for index, score in enumerate(parsed["scores"])
                if score >= metadata["min_score"]
            )
            observed_candidates = [
                (score, index) for rank, score, index in parsed["candidates"]
            ]
            candidate_ranks = [rank for rank, _, _ in parsed["candidates"]]
            candidate_order_valid = (
                observed_candidates == expected_candidates
                and candidate_ranks == list(range(len(candidate_ranks)))
            )
            predecessor_domain_valid = all(
                predecessor == -1 or 0 <= predecessor < index
                for index, predecessor in enumerate(parsed["predecessors"])
            )
            lane_reports: list[dict[str, Any]] = []
            for lanes in LANE_WIDTHS:
                reconstructed = reconstruct(parsed, lanes)
                measured = next(
                    record
                    for record in run["backtrack_records"]
                    if record["stage"] == metadata["stage"]
                    and record["call_id"] == metadata["call_id"]
                    and record["lanes"] == lanes
                )
                mismatches = {
                    field: {"reconstructed": reconstructed[field], "measured": measured[field]}
                    for field in COMPARE_FIELDS
                    if reconstructed[field] != measured[field]
                }
                if mismatches:
                    all_errors.append(
                        f"{run['run_id']} {dump_path.name} L={lanes}: {mismatches}"
                    )
                lane_reports.append(
                    {
                        "lanes": lanes,
                        "all_fields_match": not mismatches,
                        "mismatches": mismatches,
                    }
                )
            if not candidate_order_valid:
                all_errors.append(f"{run['run_id']} {dump_path.name}: candidate order mismatch")
            if not predecessor_domain_valid:
                all_errors.append(f"{run['run_id']} {dump_path.name}: invalid predecessor domain")
            reports.append(
                {
                    "run_id": run["run_id"],
                    "dump_path": str(dump_path),
                    "dump_sha256": sha256(dump_path),
                    "stage": metadata["stage"],
                    "call_id": metadata["call_id"],
                    "anchor_count": metadata["anchor_count"],
                    "candidate_end_count": metadata["candidate_end_count"],
                    "min_score": metadata["min_score"],
                    "candidate_set_and_order_valid": candidate_order_valid,
                    "predecessor_domain_valid": predecessor_domain_valid,
                    "lane_reconstructions": lane_reports,
                }
            )

    report = {
        "schema_version": 1,
        "validator": "independent Python reconstruction from full f/p and sorted-candidate dumps",
        "validated_dump_count": len(reports),
        "validated_lane_record_count": len(reports) * len(LANE_WIDTHS),
        "all_checks_pass": not all_errors,
        "errors": all_errors,
        "dumps": reports,
    }
    args.output.write_text(
        yaml.safe_dump(report, sort_keys=False, width=120), encoding="utf-8"
    )
    print(yaml.safe_dump({key: report[key] for key in report if key != "dumps"}, sort_keys=False))
    if all_errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
