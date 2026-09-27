#!/usr/bin/env python3
"""Preserve and merge the accepted Run_L pilot and comb-control sweep.

The source experiment directory is disposable.  This finalizer therefore
copies every input and raw output into a workspace-level artifact tree before
publishing a canonical YAML document and compact CSV tables.  It reparses the
raw telemetry so later DP and RMQ calls retain every counter.
"""

from __future__ import annotations

import argparse
import csv
import ctypes
import errno
import fcntl
import hashlib
import io
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import tempfile
from typing import Any, Iterable, Sequence
import uuid

import yaml


SOURCE_ROOT = Path(__file__).resolve().parent
WORKSPACE_ROOT = SOURCE_ROOT.parent
DEFAULT_PILOT_MANIFEST = SOURCE_ROOT / "repeat_disruption_data_pilot" / "manifest.json"
DEFAULT_PILOT_RESULTS = SOURCE_ROOT / "repeat_disruption_results.yaml"
DEFAULT_PILOT_SHARDS = SOURCE_ROOT / "repeat_disruption_shards"
DEFAULT_PILOT_RAW = SOURCE_ROOT / "repeat_disruption_raw"
DEFAULT_PILOT_PROGRESS = SOURCE_ROOT / "repeat_disruption_progress.jsonl"
DEFAULT_COMB_ROOT = SOURCE_ROOT / "comb_sweep_final_20260902T123232Z"
DEFAULT_ARTIFACT_DIR = (
    WORKSPACE_ROOT / "CHAINING_BRANCHING_REPEAT_EXPERIMENT_ARTIFACTS" / "v1"
)
DEFAULT_RESULTS_YAML = WORKSPACE_ROOT / "research_benchmarks/paper/chaining/CHAINING_BRANCHING_REPEAT_EXPERIMENT_RESULTS.yaml"
LANE_WIDTHS = (2, 4, 8, 16, 32, 64)
RUNL_PREFIX = "MM_RUNL\t"
MAP_PREFIX = "MM_RUNL_MAP\t"
ERROR_PREFIX = "MM_RUNL_ERROR\t"

EXPECTED_COMB_GENERATOR_SHA256 = (
    "e9262fdf19c8ae5d48ba35158a91bec1c2736d299e332e2dc79dacd6283a37b3"
)
EXPECTED_COMB_ADAPTER_SHA256 = (
    "05cc882b8131c8b3740d8a0c893476cc7ecfd29714ab25626cc3346311464d76"
)
EXPECTED_CURRENT_RUNNER_SHA256 = (
    "2d3aeb83dd70cb835bcbbd4feeecfc4740c76555e4b812431bd774dafc04a591"
)
EXPECTED_PILOT_RUNNER_SHA256 = (
    "d7d054f48fe51f81c58b2b8d1f1b201d9b30e35b01b5194eabfb009e9a271e7d"
)
EXPECTED_COMMON_SHA256 = (
    "c55c00f4a87a17de450b4e95c8db3afa01fbf947cdff7d99c81ce4fc45d80aa2"
)
EXPECTED_LCHAIN_SHA256 = (
    "879e032527fcfcccdab69ceda588a9bb7a106a94c0e880b0710bdf2b2ae215f3"
)
EXPECTED_BINARY_SHA256 = (
    "6533837743e94e3ec22db01b874e75efbc8b2c6402f232cde0d5709924efd6c3"
)
EXPECTED_COMB_CHECKSUMS_SHA256 = (
    "6d62529ba409cb85e675d7e98f0b26f86bfd96b8d9631c3f5561c23b9e56375e"
)
EXPECTED_COMB_PROVENANCE_SHA256 = (
    "993da31f4193c80ec62d3eab0d5dd0b69684aa3fd7c183a0921b6bd3a1ecdbb5"
)
EXPECTED_STANDALONE_SOURCE_MANIFEST_SHA256 = (
    "76caa0a203d90295107260cb553430aed171ff3b58f20069def2c9c112e338fc"
)
EXPECTED_MAKEFILE_SHA256 = (
    "e7b970f150adab377fe1d5d32a2d374f64a109efbc4df25a8b3094c69d3742c1"
)
EXPECTED_MAP_C_SHA256 = (
    "21fd9ec17bddd252dfc756ca99e89d7f6ea0c907c586affcbd0d83c9eab68ad3"
)
EXPECTED_PILOT_MANIFEST_SHA256 = (
    "90c46e2a8dbb065dff117f6d65d64b83bfebf7a55c335585051bba78d2fd0c20"
)
EXPECTED_PILOT_RESULTS_SHA256 = (
    "fe03c6a168216bffacaf97ec810d750ba62045852fcff25da0a8cfd3530300e0"
)
EXPECTED_PILOT_GENERATOR_SHA256 = (
    "0c360a2860e566282b22eb4e8bd97d3fdc02ceeea3dc75a0042688a9c6a07396"
)
EXPECTED_PILOT_PROGRESS_SHA256 = (
    "dcabac45f3f51e5216fb89d0766bf85643ec5ab66f862f58b9933e98427d6390"
)
EXPECTED_PILOT_PROGRESS_SIZE_BYTES = 20_097
EXPECTED_PILOT_PROGRESS_RECORD_COUNT = 102
EXPECTED_MINIMAP_SOURCE_TREE_SHA256 = (
    "77d2a45e558459110d7f0032b464fa84e10767a14769a7ef0ab9320c028d1eee"
)
EXPECTED_MINIMAP_SOURCE_FILE_COUNT = 45
EXPERIMENT_ID = "runl_repeat_disruption_and_comb_control"

RUNL_INTEGER_FIELDS = (
    "schema_version",
    "call_id",
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
)

RUNL_CSV_FIELDS = (
    "source_profile",
    "case_id",
    "replicate_id",
    "seed",
    "group",
    "parameters_json",
    "similarity_level",
    "copy_count",
    "m",
    "initial_dp_realized_duplicate_fraction",
    "stage",
    "stage_call_ordinal",
    "call_role",
    *RUNL_INTEGER_FIELDS,
    "predicate_firing_percent",
    "bounds_conditional_percent",
    "path_coverage_percent",
    "line_number",
    "raw_line_sha256",
    "extra_fields_json",
)

CASE_CSV_FIELDS = (
    "source_profile",
    "case_id",
    "replicate_id",
    "seed",
    "group",
    "status",
    "similarity_level",
    "substitution_count",
    "copy_count",
    "m",
    "reference_length",
    "query_length",
    "initial_anchor_count",
    "initial_candidate_end_count",
    "initial_path_anchor_count",
    "initial_dp_realized_duplicate_fraction",
    "initial_dp_maximum_same_target_group_size",
    "raw_query_minimizers",
    "query_minimizers_after_query_filter",
    "retained_anchor_count",
    "paf_record_count",
    "qualified_paf_record_count",
    "wall_seconds",
    "peak_rss_kib",
    "manifest_sha256",
    "minimap2_sha256",
)

PAF_CSV_FIELDS = (
    "source_profile",
    "case_id",
    "line_number",
    "query_name",
    "query_length",
    "query_start",
    "query_end",
    "strand",
    "target_name",
    "target_length",
    "target_start",
    "target_end",
    "residue_matches",
    "alignment_block_length",
    "mapping_quality",
    "optional_fields_json",
    "raw_line_sha256",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pilot-manifest", type=Path, default=DEFAULT_PILOT_MANIFEST)
    parser.add_argument("--pilot-results", type=Path, default=DEFAULT_PILOT_RESULTS)
    parser.add_argument("--pilot-shards", type=Path, default=DEFAULT_PILOT_SHARDS)
    parser.add_argument("--pilot-raw", type=Path, default=DEFAULT_PILOT_RAW)
    parser.add_argument("--pilot-progress", type=Path, default=DEFAULT_PILOT_PROGRESS)
    parser.add_argument("--comb-root", type=Path, default=DEFAULT_COMB_ROOT)
    parser.add_argument("--artifact-dir", type=Path, default=DEFAULT_ARTIFACT_DIR)
    parser.add_argument("--results-yaml", type=Path, default=DEFAULT_RESULTS_YAML)
    parser.add_argument(
        "--validate-only",
        action="store_true",
        help="validate every source and build the in-memory merge without writing",
    )
    return parser.parse_args()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def canonical_json_sha256(value: Any) -> str:
    encoded = json.dumps(
        value, ensure_ascii=True, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return sha256_bytes(encoded)


def load_json(path: Path) -> dict[str, Any]:
    require(path.is_file(), f"missing JSON file: {path}")
    with path.open("r", encoding="utf-8") as source:
        value = json.load(source)
    require(isinstance(value, dict), f"JSON root must be an object: {path}")
    return value


def load_yaml(path: Path) -> dict[str, Any]:
    require(path.is_file(), f"missing YAML file: {path}")
    with path.open("r", encoding="utf-8") as source:
        value = yaml.safe_load(source)
    require(isinstance(value, dict), f"YAML root must be an object: {path}")
    return value


def path_below(path: Path, parent: Path, label: str) -> Path:
    resolved = path.resolve()
    try:
        resolved.relative_to(parent.resolve())
    except ValueError as error:
        raise ValueError(f"{label} must resolve below {parent.resolve()}: {resolved}") from error
    return resolved


def artifact_reference(artifact_dir: Path, relative: Path) -> str:
    absolute = artifact_dir.resolve() / relative
    return str(absolute.relative_to(WORKSPACE_ROOT.resolve()))


def parse_paf(path: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as source:
        for line_number, raw_with_newline in enumerate(source, start=1):
            raw = raw_with_newline.rstrip("\r\n")
            if not raw:
                continue
            fields = raw.split("\t")
            require(
                len(fields) >= 12,
                f"{path}:{line_number}: PAF has {len(fields)} fields",
            )
            try:
                query_length = int(fields[1])
                query_start = int(fields[2])
                query_end = int(fields[3])
                target_length = int(fields[6])
                target_start = int(fields[7])
                target_end = int(fields[8])
                residue_matches = int(fields[9])
                alignment_block_length = int(fields[10])
                mapping_quality = int(fields[11])
            except ValueError as error:
                raise ValueError(f"{path}:{line_number}: noninteger PAF field") from error
            require(fields[0] and fields[5], f"{path}:{line_number}: empty PAF name")
            require(fields[4] in {"+", "-"}, f"{path}:{line_number}: invalid strand")
            require(
                query_length > 0
                and 0 <= query_start <= query_end <= query_length
                and target_length > 0
                and 0 <= target_start <= target_end <= target_length,
                f"{path}:{line_number}: invalid PAF interval",
            )
            require(
                0 <= residue_matches <= alignment_block_length
                and 0 <= mapping_quality <= 255,
                f"{path}:{line_number}: invalid PAF counts",
            )
            records.append(
                {
                    "line_number": line_number,
                    "query_name": fields[0],
                    "query_length": query_length,
                    "query_start": query_start,
                    "query_end": query_end,
                    "strand": fields[4],
                    "target_name": fields[5],
                    "target_length": target_length,
                    "target_start": target_start,
                    "target_end": target_end,
                    "residue_matches": residue_matches,
                    "alignment_block_length": alignment_block_length,
                    "mapping_quality": mapping_quality,
                    "optional_fields": fields[12:],
                    "raw_line": raw,
                    "raw_line_sha256": sha256_bytes(raw.encode("utf-8")),
                }
            )
    return records


def parse_telemetry(path: Path) -> dict[str, list[dict[str, Any]]]:
    parsed = {"map": [], "runl": [], "errors": []}
    prefixes = ((MAP_PREFIX, "map"), (RUNL_PREFIX, "runl"), (ERROR_PREFIX, "errors"))
    with path.open("r", encoding="utf-8") as source:
        for line_number, raw_with_newline in enumerate(source, start=1):
            raw = raw_with_newline.rstrip("\r\n")
            for prefix, destination in prefixes:
                if not raw.startswith(prefix):
                    continue
                try:
                    record = json.loads(raw[len(prefix) :])
                except json.JSONDecodeError as error:
                    raise ValueError(
                        f"{path}:{line_number}: invalid {prefix.strip()} JSON"
                    ) from error
                require(
                    isinstance(record, dict),
                    f"{path}:{line_number}: telemetry payload is not an object",
                )
                parsed[destination].append(
                    {
                        "line_number": line_number,
                        "raw_line_sha256": sha256_bytes(raw.encode("utf-8")),
                        "record": record,
                    }
                )
                break
    return parsed


def validate_pilot_progress(
    path: Path, expected_case_ids: set[str]
) -> dict[str, Any]:
    require(path.is_file(), "pilot progress log is missing")
    require(not path.is_symlink(), "pilot progress log is a symlink")
    require(
        sha256_file(path) == EXPECTED_PILOT_PROGRESS_SHA256,
        "pilot progress log differs from the accepted run",
    )
    require(
        path.stat().st_size == EXPECTED_PILOT_PROGRESS_SIZE_BYTES,
        "pilot progress log size differs",
    )
    records: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as source:
        for line_number, raw in enumerate(source, start=1):
            require(raw.endswith("\n"), f"pilot progress line {line_number} lacks newline")
            try:
                record = json.loads(raw)
            except json.JSONDecodeError as error:
                raise ValueError(
                    f"pilot progress line {line_number} is invalid JSON"
                ) from error
            require(
                isinstance(record, dict),
                f"pilot progress line {line_number} is not an object",
            )
            require(
                record.get("event")
                in {"runner_start", "case_start", "case_complete", "runner_stop"},
                f"pilot progress line {line_number} has an unknown event",
            )
            records.append(record)
    require(
        len(records) == EXPECTED_PILOT_PROGRESS_RECORD_COUNT,
        "pilot progress record count differs",
    )
    event_counts = {
        event: sum(record["event"] == event for record in records)
        for event in ("runner_start", "case_start", "case_complete", "runner_stop")
    }
    require(
        event_counts
        == {
            "runner_start": 3,
            "case_start": 48,
            "case_complete": 48,
            "runner_stop": 3,
        },
        "pilot progress event counts differ",
    )
    for event in ("case_start", "case_complete"):
        observed = [str(record.get("case_id")) for record in records if record["event"] == event]
        require(
            len(observed) == len(set(observed))
            and set(observed) == expected_case_ids,
            f"pilot progress {event} case identities differ",
        )
    complete = [record for record in records if record["event"] == "case_complete"]
    require(
        all(record.get("status") == "accepted" for record in complete),
        "pilot progress contains a nonaccepted case",
    )
    starts = [record for record in records if record["event"] == "runner_start"]
    require(
        all(
            record.get("manifest_sha256") == EXPECTED_PILOT_MANIFEST_SHA256
            and record.get("minimap2_sha256") == EXPECTED_BINARY_SHA256
            and isinstance(record.get("resource_policy"), dict)
            for record in starts
        ),
        "pilot progress runner provenance differs",
    )
    stops = [record for record in records if record["event"] == "runner_stop"]
    require(
        all(record.get("interrupted") is False for record in stops)
        and sum(int(record.get("completed_this_run", -1)) for record in stops) == 48,
        "pilot progress runner completion differs",
    )
    timestamps = [record.get("timestamp_utc") for record in records]
    require(
        all(isinstance(value, str) for value in timestamps)
        and timestamps == sorted(timestamps),
        "pilot progress timestamps are invalid or out of order",
    )
    return {
        "sha256": EXPECTED_PILOT_PROGRESS_SHA256,
        "size_bytes": EXPECTED_PILOT_PROGRESS_SIZE_BYTES,
        "record_count": len(records),
        "event_counts": event_counts,
        "runner_start_resource_policies": [
            record["resource_policy"] for record in starts
        ],
    }


def validate_runl_records(
    wrapped_records: Sequence[dict[str, Any]], context: str
) -> dict[tuple[str, int], list[dict[str, Any]]]:
    groups: dict[tuple[str, int], list[dict[str, Any]]] = {}
    identities: set[tuple[str, int, int]] = set()
    for wrapped in wrapped_records:
        record = wrapped["record"]
        require(record.get("stage") in {"dp", "rmq"}, f"{context}: unknown stage")
        for field in RUNL_INTEGER_FIELDS:
            require(
                isinstance(record.get(field), int) and not isinstance(record.get(field), bool),
                f"{context}: {field} is missing or not an integer",
            )
        require(record["schema_version"] == 1, f"{context}: unsupported schema")
        require(record["lanes"] in LANE_WIDTHS, f"{context}: unexpected lane width")
        require(record["call_id"] >= 1, f"{context}: invalid call id")
        require(record["anchor_count"] >= 0, f"{context}: negative anchor count")
        require(record["candidate_end_count"] >= 0, f"{context}: negative candidate count")
        require(
            record["candidate_end_count"] <= record["anchor_count"],
            f"{context}: candidate count exceeds anchor count",
        )
        require(
            record["has_ranked_end"] in {0, 1}
            and record["has_fast_batch"] in {0, 1},
            f"{context}: boolean telemetry flag is outside zero/one",
        )
        require(
            record["has_ranked_end"] == int(record["candidate_end_count"] > 0),
            f"{context}: ranked-end flag differs from candidate count",
        )
        if record["has_ranked_end"]:
            require(
                0 <= record["start_index"] < record["anchor_count"],
                f"{context}: ranked start index is outside the anchor array",
            )
            require(
                record["start_score"] >= record["min_score"],
                f"{context}: ranked start score is below min_score",
            )
            require(
                1 <= record["primary_path_anchors"] <= record["anchor_count"],
                f"{context}: ranked path length is outside the anchor array",
            )
        else:
            require(
                record["start_index"] == -1
                and record["start_score"] == -1
                and record["primary_path_anchors"] == 0,
                f"{context}: no-ranked-end sentinels differ",
            )
        for field in RUNL_INTEGER_FIELDS[7:]:
            if field not in {"start_index", "start_score"}:
                require(record[field] >= 0, f"{context}: negative {field}")
        identity = (record["stage"], record["call_id"], record["lanes"])
        require(identity not in identities, f"{context}: duplicate telemetry identity {identity}")
        identities.add(identity)
        require(
            record["loop_evaluations"]
            == record["fast_batches"] + record["scalar_fallbacks"],
            f"{context}: loop partition identity failed at {identity}",
        )
        require(
            record["loop_evaluations"]
            == record["bounds_pass"]
            + record["bounds_fail_current"]
            + record["bounds_fail_capacity"]
            + record["bounds_fail_both"],
            f"{context}: bounds partition identity failed at {identity}",
        )
        require(
            record["bounds_pass"]
            == record["fast_batches"] + record["contiguous_mismatch"],
            f"{context}: contiguity partition identity failed at {identity}",
        )
        require(
            record["primary_path_anchors"]
            == record["fast_covered_anchors"] + record["scalar_fallbacks"],
            f"{context}: path coverage identity failed at {identity}",
        )
        require(
            record["fast_covered_anchors"]
            == record["lanes"] * record["fast_batches"],
            f"{context}: vector-covered anchor identity failed at {identity}",
        )
        require(
            record["scalar_path_length"] == record["primary_path_anchors"],
            f"{context}: scalar path length differs at {identity}",
        )
        require(record["scalar_validation_error"] == 0, f"{context}: scalar validation failed")
        require(record["emulation_error"] == 0, f"{context}: C emulation failed")
        require(
            record["has_fast_batch"] == int(record["fast_batches"] > 0),
            f"{context}: has_fast_batch flag differs at {identity}",
        )
        groups.setdefault(identity[:2], []).append(wrapped)
    for call, group in groups.items():
        observed = sorted(item["record"]["lanes"] for item in group)
        require(observed == list(LANE_WIDTHS), f"{context}: call {call} widths are {observed}")
        identity_fields = (
            "schema_version",
            "anchor_count",
            "candidate_end_count",
            "start_index",
            "start_score",
            "primary_path_anchors",
            "scalar_path_length",
            "scalar_validation_error",
            "emulation_error",
            "min_score",
            "has_ranked_end",
        )
        first = group[0]["record"]
        for wrapped in group[1:]:
            record = wrapped["record"]
            for field in identity_fields:
                require(
                    record[field] == first[field],
                    f"{context}: call {call} changes {field} across lane widths",
                )
    return groups


def parse_dump_meta(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="ascii") as source:
        first = source.readline().rstrip("\n").split("\t")
    require(len(first) == 6 and first[0] == "META", f"{path}: invalid dump META")
    try:
        return {
            "stage": first[1],
            "call_id": int(first[2]),
            "anchor_count": int(first[3]),
            "candidate_end_count": int(first[4]),
            "min_score": int(first[5]),
        }
    except ValueError as error:
        raise ValueError(f"{path}: noninteger dump META") from error


def verify_declared_file(path: Path, declaration: dict[str, Any], context: str) -> None:
    require(path.is_file(), f"{context}: missing file {path}")
    expected_sha = declaration.get("sha256") or declaration.get("file_sha256")
    require(isinstance(expected_sha, str), f"{context}: missing SHA-256 declaration")
    require(sha256_file(path) == expected_sha, f"{context}: SHA-256 mismatch for {path}")
    if "size_bytes" in declaration:
        require(
            path.stat().st_size == int(declaration["size_bytes"]),
            f"{context}: size mismatch for {path}",
        )


def collect_declared_paths(value: Any) -> set[Path]:
    """Collect relative artifact paths from nested manifest declarations."""

    result: set[Path] = set()
    if isinstance(value, dict):
        declared = value.get("path")
        if isinstance(declared, str):
            relative = Path(declared)
            require(
                relative.parts
                and not relative.is_absolute()
                and ".." not in relative.parts,
                f"unsafe declared path: {relative}",
            )
            result.add(relative)
        for nested in value.values():
            result.update(collect_declared_paths(nested))
    elif isinstance(value, list):
        for nested in value:
            result.update(collect_declared_paths(nested))
    return result


def percent(numerator: int, denominator: int) -> float | None:
    return None if denominator == 0 else 100.0 * numerator / denominator


def metric_values(record: dict[str, Any]) -> dict[str, float | None]:
    if not record["has_ranked_end"]:
        return {
            "predicate_firing_percent": None,
            "bounds_conditional_percent": None,
            "path_coverage_percent": None,
        }
    return {
        "predicate_firing_percent": percent(
            record["fast_batches"], record["loop_evaluations"]
        ),
        "bounds_conditional_percent": percent(
            record["fast_batches"], record["bounds_pass"]
        ),
        "path_coverage_percent": percent(
            record["fast_covered_anchors"], record["primary_path_anchors"]
        ),
    }


def call_ordinals(wrapped_records: Sequence[dict[str, Any]]) -> dict[tuple[str, int], int]:
    stages: dict[str, list[int]] = {}
    for wrapped in wrapped_records:
        record = wrapped["record"]
        stages.setdefault(record["stage"], []).append(record["call_id"])
    result: dict[tuple[str, int], int] = {}
    for stage, call_ids in stages.items():
        for ordinal, call_id in enumerate(sorted(set(call_ids)), start=1):
            result[(stage, call_id)] = ordinal
    return result


def case_artifact_paths(
    artifact_dir: Path, source_profile: str, case_id: str, comb_m: int | None = None
) -> dict[str, str]:
    if source_profile == "pilot":
        base = Path("pilot") / "raw" / case_id
        return {
            "raw_directory": artifact_reference(artifact_dir, base),
            "paf": artifact_reference(artifact_dir, base / "stdout.paf"),
            "stderr": artifact_reference(artifact_dir, base / "stderr.log"),
            "invocation": artifact_reference(artifact_dir, base / "invocation.json"),
            "case_result": artifact_reference(artifact_dir, base / "case_result.json"),
            "dumps": artifact_reference(artifact_dir, base / "dumps"),
            "shard": artifact_reference(
                artifact_dir, Path("pilot") / "shards" / f"{case_id}.json"
            ),
        }
    require(comb_m is not None, "comb artifact path requires m")
    base = Path("comb") / "sweep" / f"m_{comb_m:04d}"
    return {
        "raw_directory": artifact_reference(artifact_dir, base),
        "paf": artifact_reference(artifact_dir, base / "mapping.paf"),
        "stderr": artifact_reference(artifact_dir, base / "mapping.stderr"),
        "case_manifest": artifact_reference(artifact_dir, base / "manifest.json"),
        "reference": artifact_reference(artifact_dir, base / "reference.fa"),
        "query": artifact_reference(artifact_dir, base / "query.fa"),
        "dumps": artifact_reference(artifact_dir, base / "dumps"),
    }


def validate_pilot(
    manifest_path: Path,
    results_path: Path,
    shards_dir: Path,
    raw_dir: Path,
    progress_path: Path,
    artifact_dir: Path,
    generator_path: Path | None = None,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    manifest = load_json(manifest_path)
    aggregate = load_yaml(results_path)
    require(
        sha256_file(manifest_path) == EXPECTED_PILOT_MANIFEST_SHA256,
        "pilot manifest differs from the accepted three-seed pilot",
    )
    require(
        sha256_file(results_path) == EXPECTED_PILOT_RESULTS_SHA256,
        "pilot aggregate differs from the accepted three-seed pilot",
    )
    require(manifest.get("status") == "complete", "pilot manifest is not complete")
    require(aggregate.get("experiment_status") == "complete_pass", "pilot did not pass")
    manifest_digest = sha256_file(manifest_path)
    require(
        aggregate.get("manifest", {}).get("sha256") == manifest_digest,
        "pilot aggregate manifest digest differs",
    )
    require(
        aggregate.get("minimap2", {}).get("sha256") == EXPECTED_BINARY_SHA256,
        "pilot aggregate binary digest differs",
    )
    manifest_cases = manifest.get("cases")
    aggregate_runs = aggregate.get("runs")
    require(isinstance(manifest_cases, list), "pilot manifest lacks cases")
    require(isinstance(aggregate_runs, list), "pilot aggregate lacks runs")
    require(len(manifest_cases) == 48, f"pilot case count is {len(manifest_cases)}, expected 48")
    require(len(aggregate_runs) == 48, "pilot aggregate does not have 48 runs")
    manifest_case_ids = {str(case["case_id"]) for case in manifest_cases}
    require(len(manifest_case_ids) == 48, "pilot manifest has duplicate case ids")
    progress_summary = validate_pilot_progress(progress_path, manifest_case_ids)
    declared_input_paths = collect_declared_paths(manifest)
    declared_input_paths.update({Path("manifest.json"), Path("manifest.sha256")})
    actual_input_paths = {
        path.relative_to(manifest_path.parent)
        for path in manifest_path.parent.rglob("*")
        if path.is_file()
    }
    require(
        actual_input_paths == declared_input_paths,
        "pilot input tree differs from the manifest-declared file set",
    )
    require(
        (manifest_path.parent / "manifest.sha256").read_text(encoding="ascii")
        == f"{manifest_digest}  manifest.json\n",
        "pilot manifest checksum companion differs",
    )
    run_by_id = {str(run["case_id"]): run for run in aggregate_runs}
    require(len(run_by_id) == len(aggregate_runs), "pilot aggregate has duplicate case ids")
    shard_paths = sorted(shards_dir.glob("*.json"))
    require(len(shard_paths) == 48, f"pilot shard count is {len(shard_paths)}, expected 48")
    require(
        {path for path in shards_dir.rglob("*") if path.is_file()} == set(shard_paths),
        "pilot shard directory contains undeclared files",
    )
    shard_by_id = {path.stem: path for path in shard_paths}
    cases: list[dict[str, Any]] = []
    source_digest_sets: set[tuple[tuple[str, str], ...]] = set()
    expected_raw_tree_files: set[Path] = set()
    for manifest_case in manifest_cases:
        case_id = str(manifest_case["case_id"])
        require(case_id in run_by_id, f"pilot aggregate misses {case_id}")
        require(case_id in shard_by_id, f"pilot shards miss {case_id}")
        shard = load_json(shard_by_id[case_id])
        require(shard == run_by_id[case_id], f"pilot aggregate differs from shard {case_id}")
        for field in ("case_id", "seed", "replicate_id", "group", "parameters"):
            require(
                shard.get(field) == manifest_case.get(field),
                f"pilot manifest/shard {field} differs for {case_id}",
            )
        require(shard.get("status") == "accepted", f"pilot case {case_id} is not accepted")
        require(
            shard.get("provenance", {}).get("manifest_sha256") == manifest_digest,
            f"pilot case {case_id} has a different manifest digest",
        )
        require(
            shard.get("provenance", {}).get("minimap2_sha256") == EXPECTED_BINARY_SHA256,
            f"pilot case {case_id} has a different binary digest",
        )
        source_digests = shard.get("provenance", {}).get("analysis_source_sha256")
        require(isinstance(source_digests, dict), f"pilot case {case_id} lacks source digests")
        source_digest_sets.add(tuple(sorted(source_digests.items())))
        raw_case = raw_dir / case_id
        embedded = load_json(raw_case / "case_result.json")
        require(embedded == shard, f"pilot raw case_result differs for {case_id}")
        for declaration in shard.get("raw_artifacts", {}).values():
            relative = Path(str(declaration["path"]))
            path = path_below(raw_case / relative, raw_case, f"pilot raw {case_id}")
            verify_declared_file(path, declaration, f"pilot raw {case_id}")
        declared_dumps = shard.get("validation", {}).get("dump_artifacts")
        require(isinstance(declared_dumps, list), f"pilot case {case_id} lacks dump inventory")
        declared_raw_paths = {
            Path(str(declaration["path"]))
            for declaration in shard.get("raw_artifacts", {}).values()
        }
        declared_raw_paths.update(
            Path(str(declaration["path"])) for declaration in declared_dumps
        )
        declared_raw_paths.add(Path("case_result.json"))
        actual_raw_paths = {
            path.relative_to(raw_case)
            for path in raw_case.rglob("*")
            if path.is_file()
        }
        require(
            actual_raw_paths == declared_raw_paths,
            f"pilot raw file set differs from declarations for {case_id}",
        )
        expected_raw_tree_files.update(
            Path(case_id) / relative for relative in declared_raw_paths
        )
        dump_metas: dict[tuple[str, int], dict[str, Any]] = {}
        for declaration in declared_dumps:
            path = path_below(
                raw_case / str(declaration["path"]), raw_case, f"pilot dump {case_id}"
            )
            verify_declared_file(path, declaration, f"pilot dump {case_id}")
            meta = parse_dump_meta(path)
            key = (meta["stage"], meta["call_id"])
            require(key not in dump_metas, f"pilot case {case_id} has duplicate dump {key}")
            dump_metas[key] = meta
        stderr_path = raw_case / "stderr.log"
        telemetry = parse_telemetry(stderr_path)
        require(not telemetry["errors"], f"pilot case {case_id} emitted instrumentation errors")
        groups = validate_runl_records(telemetry["runl"], f"pilot {case_id}")
        require(set(groups) == set(dump_metas), f"pilot {case_id} telemetry/dump calls differ")
        for key, group in groups.items():
            record = group[0]["record"]
            meta = dump_metas[key]
            for field in ("anchor_count", "candidate_end_count", "min_score"):
                require(record[field] == meta[field], f"pilot {case_id} dump differs on {field}")
        stored_telemetry = shard["validation"]["telemetry"]
        require(
            len(telemetry["runl"]) == int(stored_telemetry["runl_record_count"]),
            f"pilot {case_id} raw Run_L count differs from shard",
        )
        initial_call_id = int(stored_telemetry["initial_dp_call_id"])
        initial_raw = [
            wrapped["record"]
            for wrapped in telemetry["runl"]
            if wrapped["record"]["stage"] == "dp"
            and wrapped["record"]["call_id"] == initial_call_id
        ]
        require(
            initial_raw == stored_telemetry["initial_dp_records"],
            f"pilot {case_id} initial records differ from raw telemetry",
        )
        require(
            [wrapped["record"] for wrapped in telemetry["map"]]
            == stored_telemetry["map_records"],
            f"pilot {case_id} map records differ from raw telemetry",
        )
        paf_path = raw_case / "stdout.paf"
        paf_records = parse_paf(paf_path)
        require(
            len(paf_records) == int(shard["validation"]["paf"]["nonempty_line_count"]),
            f"pilot {case_id} PAF count differs from shard",
        )
        manifest_case_inputs = ("reference", "query", "construction_artifact")
        for key in manifest_case_inputs:
            declaration = manifest_case[key]
            input_path = path_below(
                manifest_path.parent / str(declaration["path"]),
                manifest_path.parent,
                f"pilot input {case_id}",
            )
            verify_declared_file(input_path, declaration, f"pilot input {case_id}")
        first_path = shard["validation"]["first_path"]
        repeat_statistics = first_path.get("realized_repeat_statistics") or {}
        query_filter = stored_telemetry.get("query_filter") or {}
        first_record = initial_raw[0]
        persistent_artifacts = case_artifact_paths(
            artifact_dir, "pilot", case_id
        )
        for source_key, output_key in (
            ("reference", "reference"),
            ("query", "query"),
            ("construction_artifact", "construction"),
        ):
            persistent_artifacts[output_key] = artifact_reference(
                artifact_dir,
                Path("pilot")
                / "inputs"
                / Path(str(manifest_case[source_key]["path"])),
            )
        cases.append(
            {
                "source_profile": "pilot",
                "case_id": case_id,
                "replicate_id": shard.get("replicate_id"),
                "seed": shard.get("seed"),
                "group": shard.get("group"),
                "parameters": shard.get("parameters", {}),
                "status": "accepted",
                "reference_length": int(manifest_case["reference"]["length"]),
                "query_length": int(manifest_case["query"]["length"]),
                "initial_anchor_count": first_record["anchor_count"],
                "initial_candidate_end_count": first_record["candidate_end_count"],
                "initial_path_anchor_count": first_record["primary_path_anchors"],
                "initial_dp_realized_duplicate_fraction": repeat_statistics.get(
                    "fraction_in_group_larger_than_one"
                ),
                "initial_dp_maximum_same_target_group_size": repeat_statistics.get(
                    "maximum_same_target_group_size"
                ),
                "raw_query_minimizers": query_filter.get("raw_query_minimizers"),
                "query_minimizers_after_query_filter": query_filter.get(
                    "query_minimizers_after_query_filter"
                ),
                "retained_anchor_count": query_filter.get("retained_anchor_count"),
                "paf_record_count": len(paf_records),
                "qualified_paf_record_count": shard["validation"]["paf"].get(
                    "matching_qualified_record_count"
                ),
                "wall_seconds": shard.get("process", {}).get("wall_seconds"),
                "peak_rss_kib": shard.get("process", {}).get("peak_rss_kib"),
                "manifest_sha256": manifest_digest,
                "minimap2_sha256": EXPECTED_BINARY_SHA256,
                "artifacts": persistent_artifacts,
                "full_paf_records": paf_records,
                "raw_telemetry": telemetry,
                "source_record": shard,
            }
        )
    actual_raw_tree_files = {
        path.relative_to(raw_dir) for path in raw_dir.rglob("*") if path.is_file()
    }
    require(
        actual_raw_tree_files == expected_raw_tree_files,
        "pilot raw root contains an undeclared case or file",
    )
    require(len(source_digest_sets) == 1, "pilot shards contain multiple source digest sets")
    source_digest_mapping = dict(next(iter(source_digest_sets)))
    require(
        source_digest_mapping
        == {
            "runl_repeat_common.py": EXPECTED_COMMON_SHA256,
            "runl_repeat_run_experiment.py": EXPECTED_PILOT_RUNNER_SHA256,
        },
        "pilot shard source digests are not the reviewed pair",
    )
    generator_path = generator_path or (SOURCE_ROOT / "runl_repeat_generate_data.py")
    require(
        manifest.get("generation", {}).get("generator_sha256")
        == EXPECTED_PILOT_GENERATOR_SHA256
        == sha256_file(generator_path),
        "pilot generator does not match the accepted manifest",
    )
    return cases, {
        "profile": "pilot",
        "manifest_sha256": manifest_digest,
        "source_aggregate_sha256": sha256_file(results_path),
        "case_count": len(cases),
        "resource_policy": aggregate.get("resource_policy"),
        "analysis_source_sha256": source_digest_mapping,
        "minimap2_sha256": EXPECTED_BINARY_SHA256,
        "progress_log": progress_summary,
        "source_manifest": manifest,
    }


def validate_comb_integrity_root(comb_root: Path) -> dict[str, Any]:
    """Verify the immutable checksum root and reviewed sweep provenance."""

    checksum_path = comb_root / "SHA256SUMS"
    provenance_path = comb_root / "provenance.json"
    require(
        sha256_file(checksum_path) == EXPECTED_COMB_CHECKSUMS_SHA256,
        "comb SHA256SUMS digest differs from the reviewed sweep",
    )
    require(
        sha256_file(provenance_path) == EXPECTED_COMB_PROVENANCE_SHA256,
        "comb provenance digest differs from the reviewed sweep",
    )
    declarations: dict[Path, str] = {}
    with checksum_path.open("r", encoding="ascii") as source:
        for line_number, raw_line in enumerate(source, start=1):
            line = raw_line.rstrip("\n")
            require(
                len(line) >= 68 and line[64:68] == "  ./",
                f"SHA256SUMS:{line_number}: invalid checksum syntax",
            )
            digest = line[:64]
            require(
                len(digest) == 64 and all(character in "0123456789abcdef" for character in digest),
                f"SHA256SUMS:{line_number}: invalid digest",
            )
            relative = Path(line[68:])
            require(
                relative.parts
                and not relative.is_absolute()
                and ".." not in relative.parts
                and relative != Path("SHA256SUMS"),
                f"SHA256SUMS:{line_number}: unsafe path {relative}",
            )
            require(relative not in declarations, f"duplicate checksum path: {relative}")
            declarations[relative] = digest
    actual_files: set[Path] = set()
    for path in comb_root.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"comb sweep contains a symlink: {path}")
        if path.is_file() and path != checksum_path:
            actual_files.add(path.relative_to(comb_root))
    require(
        set(declarations) == actual_files,
        "comb SHA256SUMS covered file set differs from the actual sweep tree",
    )
    for relative, digest in declarations.items():
        require(
            sha256_file(comb_root / relative) == digest,
            f"comb checksum mismatch for {relative}",
        )

    provenance = load_json(provenance_path)
    expected_grid = [1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024]
    expected_command = [
        "python3",
        "original-minimap2-code/runl_comb_generate.py",
        "--m",
        ",".join(str(value) for value in expected_grid),
        "--mapping-timeout-seconds",
        "300",
        "--output-root",
        "original-minimap2-code/comb_sweep_final_20260902T123232Z",
    ]
    expected_facts = {
        "schema_version": 1,
        "experiment": "repaired_P_G_C_R_Run_L_adversarial_control",
        "command": expected_command,
        "case_values": expected_grid,
        "lane_widths": list(LANE_WIDTHS),
        "accepted_cases": 11,
        "total_cases": 11,
        "accepted_run_l_records": 66,
        "generator_sha256": EXPECTED_COMB_GENERATOR_SHA256,
        "lchain_sha256": EXPECTED_LCHAIN_SHA256,
        "makefile_sha256": EXPECTED_MAKEFILE_SHA256,
        "standalone_source_manifest_sha256": EXPECTED_STANDALONE_SOURCE_MANIFEST_SHA256,
        "minimap2_binary_sha256": EXPECTED_BINARY_SHA256,
        "minimap2_version": "2.31-r1302",
        "rebuild_command": "make -B -j2 minimap2",
    }
    for field, expected in expected_facts.items():
        require(
            provenance.get(field) == expected,
            f"comb provenance field {field} differs from the reviewed sweep",
        )
    require(
        isinstance(provenance.get("started_utc"), str)
        and isinstance(provenance.get("completed_utc"), str)
        and provenance["started_utc"] < provenance["completed_utc"],
        "comb provenance timestamps are invalid",
    )
    resource = provenance.get("resource_usage")
    require(isinstance(resource, dict), "comb provenance lacks resource usage")
    for field in (
        "wall_seconds",
        "user_seconds",
        "system_seconds",
        "maximum_rss_kib",
        "swap_events",
        "pre_sweep_swap_used_kib",
        "post_sweep_swap_used_kib",
    ):
        require(
            isinstance(resource.get(field), (int, float))
            and not isinstance(resource.get(field), bool)
            and math.isfinite(float(resource[field]))
            and float(resource[field]) >= 0,
            f"comb provenance resource field {field} is invalid",
        )
    require(resource["swap_events"] == 0, "comb sweep reports swap events")
    gates = provenance.get("acceptance_gates")
    require(isinstance(gates, list) and len(gates) >= 10, "comb acceptance gates are incomplete")
    return {
        "checksum_manifest_sha256": EXPECTED_COMB_CHECKSUMS_SHA256,
        "provenance_sha256": EXPECTED_COMB_PROVENANCE_SHA256,
        "covered_file_count": len(declarations),
        "provenance": provenance,
    }


def validate_comb(
    comb_root: Path, artifact_dir: Path
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    integrity_root = validate_comb_integrity_root(comb_root)
    summary_path = comb_root / "runner_summary.json"
    summary = load_json(summary_path)
    summary_cases = summary.get("cases")
    require(isinstance(summary_cases, list), "comb summary lacks cases")
    require(len(summary_cases) == 11, f"comb summary has {len(summary_cases)} cases")
    expected_m = (1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024)
    require(
        tuple(int(case["m"]) for case in summary_cases) == expected_m,
        "comb summary m grid differs",
    )
    case_manifest_paths = [comb_root / f"m_{m:04d}" / "manifest.json" for m in expected_m]
    cases: list[dict[str, Any]] = []
    for summary_case, manifest_path in zip(summary_cases, case_manifest_paths, strict=True):
        manifest = load_json(manifest_path)
        m = int(manifest["m"])
        case_id = f"comb_m{m:04d}"
        require(m == int(summary_case["m"]), f"comb summary/manifest m differs for {m}")
        require(
            manifest.get("control_role")
            == "unique_minimizer_coordinate_island_adversarial_control",
            f"comb {m} lacks its adversarial-control label",
        )
        require(manifest.get("internal_sketch_validation") == "passed", f"comb {m} sketch failed")
        real = manifest.get("real_minimap2_validation")
        require(isinstance(real, dict) and real.get("status") == "passed", f"comb {m} failed")
        require(int(real.get("returncode", -1)) == 0, f"comb {m} minimap2 return code differs")
        require(
            real.get("minimap2_binary_sha256") == EXPECTED_BINARY_SHA256,
            f"comb {m} binary digest differs",
        )
        case_root = manifest_path.parent
        reference_path = case_root / "reference.fa"
        query_path = case_root / "query.fa"
        verify_declared_file(
            reference_path,
            {"sha256": manifest["reference_fasta_sha256"]},
            f"comb {m} reference",
        )
        verify_declared_file(
            query_path,
            {"sha256": manifest["query_fasta_sha256"]},
            f"comb {m} query",
        )
        stderr_path = case_root / "mapping.stderr"
        telemetry = parse_telemetry(stderr_path)
        require(not telemetry["errors"], f"comb {m} emitted instrumentation errors")
        groups = validate_runl_records(telemetry["runl"], f"comb {m}")
        dump_paths = sorted((case_root / "dumps").glob("runl_call_*.tsv"))
        dump_metas = {(
            (meta := parse_dump_meta(path))["stage"], meta["call_id"]
        ): meta for path in dump_paths}
        require(len(dump_metas) == len(dump_paths), f"comb {m} duplicate dump identity")
        require(set(groups) == set(dump_metas), f"comb {m} telemetry/dump calls differ")
        for key, group in groups.items():
            record = group[0]["record"]
            meta = dump_metas[key]
            for field in ("anchor_count", "candidate_end_count", "min_score"):
                require(record[field] == meta[field], f"comb {m} dump differs on {field}")
        initial_raw = [
            wrapped["record"]
            for wrapped in telemetry["runl"]
            if wrapped["record"]["stage"] == "dp"
            and wrapped["record"]["call_id"] == 1
        ]
        require(initial_raw == real.get("run_l"), f"comb {m} stored initial telemetry differs")
        require(len(initial_raw) == len(LANE_WIDTHS), f"comb {m} initial call is incomplete")
        for record in initial_raw:
            expected = manifest["expected_run_l"][str(record["lanes"])]
            observed_without_identity = {
                key: value
                for key, value in record.items()
                if key not in {"call_id", "stage", "lanes"}
            }
            require(
                observed_without_identity == expected,
                f"comb {m}, L={record['lanes']} differs from the predeclared oracle",
            )
        paf_path = case_root / "mapping.paf"
        paf_records = parse_paf(paf_path)
        require(len(paf_records) == int(real["paf_record_count"]), f"comb {m} PAF count differs")
        intended = [
            record
            for record in paf_records
            if record["query_name"] == f"comb_query_m{m}"
            and record["target_name"] == f"comb_reference_m{m}"
            and record["query_length"] == int(manifest["query_length"])
            and record["target_length"] == int(manifest["reference_length"])
            and record["strand"] == "+"
        ]
        require(len(intended) == len(real["intended_paf_records"]), f"comb {m} intended PAF count differs")
        target_span = real["required_target_span"]
        query_span = real["required_query_span"]
        qualifying = [
            record
            for record in intended
            if record["query_start"] <= int(query_span["first_anchor_endpoint"])
            and record["query_end"] > int(query_span["last_anchor_endpoint"])
            and record["target_start"] <= int(target_span["first_anchor_endpoint"])
            and record["target_end"] > int(target_span["last_anchor_endpoint"])
        ]
        require(
            len(qualifying) == int(real["qualifying_paf_record_count"]) and qualifying,
            f"comb {m} selected-branch PAF gate differs",
        )
        first_record = initial_raw[0]
        map_records = [wrapped["record"] for wrapped in telemetry["map"]]
        require(len(map_records) == 1, f"comb {m} must have one map record")
        map_record = map_records[0]
        require(
            int(map_record["retained_anchor_count"]) == first_record["anchor_count"],
            f"comb {m} map/initial anchor counts differ",
        )
        top_tie = real["top_score_tie"]
        require(int(top_tie["score"]) == 615, f"comb {m} top score differs")
        require(int(top_tie["multiplicity"]) == m, f"comb {m} top multiplicity differs")
        require(
            int(top_tie["selected_anchor_index"]) == int(manifest["anchor_count"]) - 1,
            f"comb {m} selected start index differs",
        )
        require(
            top_tie["selected_anchor_label"] == f"r{m:03d}_39",
            f"comb {m} selected start label differs",
        )
        require(
            int(summary_case["anchor_count"]) == int(manifest["anchor_count"])
            and int(summary_case["reference_length"]) == int(manifest["reference_length"])
            and int(summary_case["query_length"]) == int(manifest["query_length"])
            and summary_case["validation"] == "passed",
            f"comb {m} summary fields differ",
        )
        cases.append(
            {
                "source_profile": "comb",
                "case_id": case_id,
                "replicate_id": "comb_exact_geometry",
                "seed": int(manifest["seed"]),
                "group": "unique_minimizer_island_adversarial_control",
                "parameters": {
                    "construction": manifest["construction"],
                    "m": m,
                    "interpretation": manifest["interpretation"],
                },
                "status": "accepted",
                "reference_length": int(manifest["reference_length"]),
                "query_length": int(manifest["query_length"]),
                "initial_anchor_count": first_record["anchor_count"],
                "initial_candidate_end_count": first_record["candidate_end_count"],
                "initial_path_anchor_count": first_record["primary_path_anchors"],
                "initial_dp_realized_duplicate_fraction": 0.0,
                "initial_dp_maximum_same_target_group_size": 1,
                "raw_query_minimizers": int(map_record["raw_query_minimizers"]),
                "query_minimizers_after_query_filter": int(
                    map_record["query_minimizers_after_query_filter"]
                ),
                "retained_anchor_count": int(map_record["retained_anchor_count"]),
                "paf_record_count": len(paf_records),
                "qualified_paf_record_count": len(qualifying),
                "wall_seconds": None,
                "peak_rss_kib": None,
                "manifest_sha256": sha256_file(manifest_path),
                "minimap2_sha256": EXPECTED_BINARY_SHA256,
                "artifacts": case_artifact_paths(artifact_dir, "comb", case_id, m),
                "full_paf_records": paf_records,
                "raw_telemetry": telemetry,
                "source_record": manifest,
            }
        )
    resource_text = (comb_root / "resource_usage.txt").read_text(encoding="utf-8")
    require("Exit status: 0" in resource_text, "comb resource record lacks exit status 0")
    require((comb_root / "runner.stderr").stat().st_size == 0, "comb runner stderr is nonempty")
    return cases, {
        "profile": "comb_adversarial_control",
        "runner_summary_sha256": sha256_file(summary_path),
        "case_count": len(cases),
        "resource_usage_text": resource_text,
        "pre_resource_snapshot_text": (
            comb_root / "pre_resource_snapshot.txt"
        ).read_text(encoding="utf-8"),
        "minimap2_sha256": EXPECTED_BINARY_SHA256,
        "source_summary": summary,
        "integrity_root": integrity_root,
    }


def reconstruct_pilot_runner() -> bytes:
    current_path = SOURCE_ROOT / "runl_repeat_run_experiment.py"
    current = current_path.read_text(encoding="utf-8")
    start_marker = '        tie_policy = case.expectation.get("candidate_tie_policy")\n'
    end_marker = "        path, path_errors = reconstruct_path(top_index, initial_dump.predecessors)\n"
    require(current.count(start_marker) == 1, "current runner tie-policy marker differs")
    require(current.count(end_marker) == 1, "current runner path marker differs")
    start = current.index(start_marker)
    end = current.index(end_marker, start)
    reconstructed = (current[:start] + current[end:]).encode("utf-8")
    require(
        sha256_bytes(reconstructed) == EXPECTED_PILOT_RUNNER_SHA256,
        "reconstructed pilot runner does not match its recorded SHA-256",
    )
    return reconstructed


def expected_source_state(
    pilot_summary: dict[str, Any], finalizer_sha256: str
) -> dict[str, Any]:
    expected = {
        "runl_repeat_generate_data.py": EXPECTED_PILOT_GENERATOR_SHA256,
        "runl_repeat_common.py": EXPECTED_COMMON_SHA256,
        "runl_repeat_run_experiment.py": EXPECTED_CURRENT_RUNNER_SHA256,
        "runl_comb_generate.py": EXPECTED_COMB_GENERATOR_SHA256,
        "runl_comb_adapt_manifest.py": EXPECTED_COMB_ADAPTER_SHA256,
        "lchain.c": EXPECTED_LCHAIN_SHA256,
        "map.c": EXPECTED_MAP_C_SHA256,
        "Makefile": EXPECTED_MAKEFILE_SHA256,
        "minimap2": EXPECTED_BINARY_SHA256,
        Path(__file__).name: finalizer_sha256,
    }
    require(
        pilot_summary["source_manifest"]["generation"]["generator_sha256"]
        == EXPECTED_PILOT_GENERATOR_SHA256,
        "pilot summary names a different generator",
    )
    return {
        "observed_sha256": dict(expected),
        "expected_sha256": dict(expected),
        "reconstructed_pilot_runner_sha256": EXPECTED_PILOT_RUNNER_SHA256,
        "reconstructed_pilot_runner_method": (
            "remove the later reviewed comb candidate-tie validation block from the "
            "current runner and require the exact SHA-256 stored in all pilot shards"
        ),
    }


def validate_source_state(pilot_summary: dict[str, Any]) -> dict[str, Any]:
    paths = {
        "runl_repeat_generate_data.py": SOURCE_ROOT / "runl_repeat_generate_data.py",
        "runl_repeat_common.py": SOURCE_ROOT / "runl_repeat_common.py",
        "runl_repeat_run_experiment.py": SOURCE_ROOT / "runl_repeat_run_experiment.py",
        "runl_comb_generate.py": SOURCE_ROOT / "runl_comb_generate.py",
        "runl_comb_adapt_manifest.py": SOURCE_ROOT / "runl_comb_adapt_manifest.py",
        "lchain.c": SOURCE_ROOT / "lchain.c",
        "map.c": SOURCE_ROOT / "map.c",
        "Makefile": SOURCE_ROOT / "Makefile",
        "minimap2": SOURCE_ROOT / "minimap2",
        Path(__file__).name: Path(__file__).resolve(),
    }
    observed = {name: sha256_file(path) for name, path in paths.items()}
    expected_state = expected_source_state(
        pilot_summary, observed[Path(__file__).name]
    )
    expected = expected_state["expected_sha256"]
    for name, digest in expected.items():
        require(observed[name] == digest, f"reviewed source digest differs for {name}")
    reconstructed = reconstruct_pilot_runner()
    require(
        sha256_bytes(reconstructed)
        == expected_state["reconstructed_pilot_runner_sha256"],
        "reconstructed pilot runner digest differs",
    )
    require(observed == expected, "observed source state differs from reviewed state")
    return expected_state


def make_call_rows(cases: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    known_fields = set(RUNL_INTEGER_FIELDS) | {"stage"}
    rows: list[dict[str, Any]] = []
    for case in cases:
        ordinals = call_ordinals(case["raw_telemetry"]["runl"])
        for wrapped in case["raw_telemetry"]["runl"]:
            record = wrapped["record"]
            ordinal = ordinals[(record["stage"], record["call_id"])]
            call_role = (
                "initial_dp"
                if record["stage"] == "dp" and ordinal == 1
                else f"{record['stage']}_ordinal_{ordinal}"
            )
            parameters = case["parameters"]
            row = {
                "source_profile": case["source_profile"],
                "case_id": case["case_id"],
                "replicate_id": case["replicate_id"],
                "seed": case["seed"],
                "group": case["group"],
                "parameters_json": json.dumps(parameters, sort_keys=True, separators=(",", ":")),
                "similarity_level": parameters.get("similarity_level"),
                "copy_count": parameters.get("copy_count"),
                "m": parameters.get("m"),
                "initial_dp_realized_duplicate_fraction": case[
                    "initial_dp_realized_duplicate_fraction"
                ],
                "stage": record["stage"],
                "stage_call_ordinal": ordinal,
                "call_role": call_role,
                **{field: record[field] for field in RUNL_INTEGER_FIELDS},
                **metric_values(record),
                "line_number": wrapped["line_number"],
                "raw_line_sha256": wrapped["raw_line_sha256"],
                "extra_fields_json": json.dumps(
                    {key: value for key, value in record.items() if key not in known_fields},
                    sort_keys=True,
                    separators=(",", ":"),
                ),
            }
            rows.append(row)
    return sorted(
        rows,
        key=lambda row: (
            row["source_profile"],
            row["case_id"],
            row["stage"],
            row["call_id"],
            row["lanes"],
        ),
    )


def make_case_rows(cases: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for case in cases:
        parameters = case["parameters"]
        rows.append(
            {
                "source_profile": case["source_profile"],
                "case_id": case["case_id"],
                "replicate_id": case["replicate_id"],
                "seed": case["seed"],
                "group": case["group"],
                "status": case["status"],
                "similarity_level": parameters.get("similarity_level"),
                "substitution_count": parameters.get("substitution_count"),
                "copy_count": parameters.get("copy_count"),
                "m": parameters.get("m"),
                "reference_length": case["reference_length"],
                "query_length": case["query_length"],
                "initial_anchor_count": case["initial_anchor_count"],
                "initial_candidate_end_count": case["initial_candidate_end_count"],
                "initial_path_anchor_count": case["initial_path_anchor_count"],
                "initial_dp_realized_duplicate_fraction": case[
                    "initial_dp_realized_duplicate_fraction"
                ],
                "initial_dp_maximum_same_target_group_size": case[
                    "initial_dp_maximum_same_target_group_size"
                ],
                "raw_query_minimizers": case["raw_query_minimizers"],
                "query_minimizers_after_query_filter": case[
                    "query_minimizers_after_query_filter"
                ],
                "retained_anchor_count": case["retained_anchor_count"],
                "paf_record_count": case["paf_record_count"],
                "qualified_paf_record_count": case["qualified_paf_record_count"],
                "wall_seconds": case["wall_seconds"],
                "peak_rss_kib": case["peak_rss_kib"],
                "manifest_sha256": case["manifest_sha256"],
                "minimap2_sha256": case["minimap2_sha256"],
            }
        )
    return sorted(rows, key=lambda row: (row["source_profile"], row["case_id"]))


def make_paf_rows(cases: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for case in cases:
        for record in case["full_paf_records"]:
            rows.append(
                {
                    "source_profile": case["source_profile"],
                    "case_id": case["case_id"],
                    **{
                        field: record[field]
                        for field in PAF_CSV_FIELDS
                        if field in record and field not in {"source_profile", "case_id"}
                    },
                    "optional_fields_json": json.dumps(
                        record["optional_fields"], separators=(",", ":")
                    ),
                }
            )
    return sorted(rows, key=lambda row: (row["source_profile"], row["case_id"], row["line_number"]))


def finite_values(rows: Sequence[dict[str, Any]], field: str) -> list[float]:
    return [
        float(row[field])
        for row in rows
        if row.get(field) is not None and math.isfinite(float(row[field]))
    ]


def metric_summary(rows: Sequence[dict[str, Any]], field: str) -> dict[str, float | None]:
    values = finite_values(rows, field)
    return {
        f"{field}_median": statistics.median(values) if values else None,
        f"{field}_minimum": min(values) if values else None,
        f"{field}_maximum": max(values) if values else None,
    }


def make_tikz_rows(
    call_rows: Sequence[dict[str, Any]], construction: str
) -> list[dict[str, Any]]:
    selected = [
        row
        for row in call_rows
        if (
            construction == "mosaic" and row["group"] == "mosaic"
        )
        or (
            construction == "spaced"
            and row["group"] == "spaced_copy_control"
        )
        or (
            construction == "comb"
            and row["group"] == "unique_minimizer_island_adversarial_control"
        )
    ]
    grouped: dict[tuple[Any, ...], list[dict[str, Any]]] = {}
    for row in selected:
        if construction == "mosaic":
            x_label = row["similarity_level"]
            nominal_x = None if x_label == "independent_control" else float(x_label)
        elif construction == "spaced":
            x_label = str(row["copy_count"])
            nominal_x = int(row["copy_count"])
        else:
            x_label = str(row["m"])
            nominal_x = int(row["m"])
        key = (
            x_label,
            nominal_x,
            row["stage"],
            row["stage_call_ordinal"],
            row["call_role"],
            row["lanes"],
        )
        grouped.setdefault(key, []).append(row)
    output: list[dict[str, Any]] = []
    for key, rows in grouped.items():
        x_label, nominal_x, stage, ordinal, call_role, lanes = key
        eligible = [row for row in rows if int(row["has_ranked_end"]) == 1]
        duplicate_summary = metric_summary(
            rows, "initial_dp_realized_duplicate_fraction"
        )
        empirical_x = duplicate_summary[
            "initial_dp_realized_duplicate_fraction_median"
        ]
        totals = {
            field: sum(int(row[field]) for row in eligible)
            for field in (
                "loop_evaluations",
                "bounds_pass",
                "contiguous_mismatch",
                "fast_batches",
                "scalar_fallbacks",
                "primary_path_anchors",
                "fast_covered_anchors",
            )
        }
        row = {
            "construction": construction,
            "x_label": x_label,
            "x_numeric": empirical_x if construction == "mosaic" else nominal_x,
            "x_axis_semantic": (
                "median initial-DP selected-path same-target multiplicity fraction"
                if construction == "mosaic"
                else ("exact query copy count R" if construction == "spaced" else "comb group count m")
            ),
            "nominal_internal_divergence": nominal_x if construction == "mosaic" else None,
            "stage": stage,
            "stage_call_ordinal": ordinal,
            "call_role": call_role,
            "lanes": lanes,
            "case_count": len(rows),
            "eligible_case_count": len(eligible),
            "replicate_count": len({item["replicate_id"] for item in rows}),
            "initial_dp_duplicate_fraction_scope": (
                "case-level covariate measured only on the initial DP first path. "
                "it is not an RMQ-path measurement"
            ),
            **duplicate_summary,
            **metric_summary(eligible, "predicate_firing_percent"),
            **metric_summary(eligible, "bounds_conditional_percent"),
            **metric_summary(eligible, "path_coverage_percent"),
            **{f"sum_{field}": value for field, value in totals.items()},
            "micro_predicate_firing_percent": percent(
                totals["fast_batches"], totals["loop_evaluations"]
            ),
            "micro_bounds_conditional_percent": percent(
                totals["fast_batches"], totals["bounds_pass"]
            ),
            "micro_path_coverage_percent": percent(
                totals["fast_covered_anchors"], totals["primary_path_anchors"]
            ),
        }
        output.append(row)
    return sorted(
        output,
        key=lambda row: (
            row["stage"],
            row["stage_call_ordinal"],
            row["lanes"],
            -1 if row["x_numeric"] is None else row["x_numeric"],
            str(row["x_label"]),
        ),
    )


def render_csv(
    fieldnames: Sequence[str], rows: Iterable[dict[str, Any]]
) -> bytes:
    destination = io.StringIO(newline="")
    writer = csv.DictWriter(
        destination,
        fieldnames=list(fieldnames),
        extrasaction="ignore",
        lineterminator="\n",
    )
    writer.writeheader()
    writer.writerows(rows)
    return destination.getvalue().encode("utf-8")


def write_csv(path: Path, fieldnames: Sequence[str], rows: Iterable[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(render_csv(fieldnames, rows))


def copy_file_verified(source: Path, destination: Path) -> dict[str, Any]:
    require(source.is_file(), f"copy source is not a file: {source}")
    require(not source.is_symlink(), f"refusing source symlink: {source}")
    before = sha256_file(source)
    size = source.stat().st_size
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    after = sha256_file(source)
    copied = sha256_file(destination)
    require(before == after == copied, f"file changed during copy: {source}")
    require(destination.stat().st_size == size, f"copied size differs: {source}")
    return {"sha256": copied, "size_bytes": size}


def copy_tree_verified(source: Path, destination: Path) -> None:
    require(source.is_dir(), f"copy source is not a directory: {source}")
    require(not source.is_symlink(), f"refusing source directory symlink: {source}")
    files = sorted(path for path in source.rglob("*") if path.is_file() or path.is_symlink())
    for path in files:
        require(not path.is_symlink(), f"refusing source symlink: {path}")
        copy_file_verified(path, destination / path.relative_to(source))
    copied = sorted(path.relative_to(destination) for path in destination.rglob("*") if path.is_file())
    expected = [path.relative_to(source) for path in files]
    require(copied == expected, f"copied tree file set differs for {source}")


def atomic_write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp.{uuid.uuid4().hex}")
    try:
        with temporary.open("xb") as destination:
            destination.write(data)
            destination.flush()
            os.fsync(destination.fileno())
        rename_noreplace(temporary, path)
        fsync_directory(path.parent)
    finally:
        if temporary.exists():
            temporary.unlink()


def prepare_root_temp(path: Path, data: bytes) -> Path:
    """Create and fsync a root-result temporary file before bundle publication."""

    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.prepared.{uuid.uuid4().hex}")
    with temporary.open("xb") as destination:
        destination.write(data)
        destination.flush()
        os.fsync(destination.fileno())
    return temporary


def fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def rename_noreplace(source: Path, destination: Path) -> None:
    """Atomically publish a file or directory while refusing any destination."""

    at_fdcwd = -100
    rename_noreplace_flag = 1
    libc = ctypes.CDLL(None, use_errno=True)
    function = getattr(libc, "renameat2", None)
    require(function is not None, "renameat2 is unavailable on this platform")
    function.argtypes = (
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_uint,
    )
    function.restype = ctypes.c_int
    result = function(
        at_fdcwd,
        os.fsencode(source),
        at_fdcwd,
        os.fsencode(destination),
        rename_noreplace_flag,
    )
    if result == 0:
        return
    error_number = ctypes.get_errno()
    if error_number == errno.EEXIST:
        raise ValueError(f"refusing to overwrite {destination}")
    raise OSError(
        error_number,
        os.strerror(error_number),
        str(source),
        str(destination),
    )


def fsync_tree(root: Path) -> None:
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        with path.open("rb") as source:
            os.fsync(source.fileno())
    directories = sorted(
        (item for item in root.rglob("*") if item.is_dir()),
        key=lambda item: len(item.parts),
        reverse=True,
    )
    for directory in directories:
        fsync_directory(directory)
    fsync_directory(root)


def acquire_publication_lock() -> int:
    descriptor = os.open(
        WORKSPACE_ROOT, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
    )
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as error:
        os.close(descriptor)
        raise ValueError("another finalizer publication is already active") from error
    return descriptor


def validate_persisted_artifact_bundle(
    artifact_dir: Path, results_yaml: Path
) -> dict[str, Any] | None:
    """Regenerate and authenticate an earlier publication before accepting it."""

    artifact_exists = artifact_dir.exists()
    results_exist = results_yaml.exists()
    if not artifact_exists and not results_exist:
        return None
    if results_exist and not artifact_exists:
        raise ValueError(
            "results YAML exists without its artifact bundle. Refusing inconsistent destination"
        )
    require(artifact_dir.is_dir(), "existing artifact destination is not a directory")
    require(not artifact_dir.is_symlink(), "existing artifact destination is a symlink")
    special = {
        Path("inventory.csv"),
        Path("inventory.sha256"),
        Path("canonical_results.yaml"),
        Path("canonical_results.yaml.sha256"),
    }
    for path in artifact_dir.rglob("*"):
        require(not path.is_symlink(), f"persisted artifact contains symlink: {path}")
    inventory_path = artifact_dir / "inventory.csv"
    inventory_sha_path = artifact_dir / "inventory.sha256"
    canonical_path = artifact_dir / "canonical_results.yaml"
    canonical_sha_path = artifact_dir / "canonical_results.yaml.sha256"
    for path in (inventory_path, inventory_sha_path, canonical_path, canonical_sha_path):
        require(path.is_file(), f"persisted artifact lacks {path.name}")
    inventory_digest = sha256_file(inventory_path)
    require(
        inventory_sha_path.read_text(encoding="ascii")
        == f"{inventory_digest}  inventory.csv\n",
        "persisted inventory checksum companion differs",
    )
    declarations: dict[Path, tuple[str, int]] = {}
    with inventory_path.open("r", encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source)
        require(
            reader.fieldnames == ["path", "sha256", "size_bytes"],
            "persisted inventory header differs",
        )
        for row in reader:
            relative = Path(str(row["path"]))
            require(
                relative.parts and not relative.is_absolute() and ".." not in relative.parts,
                f"unsafe persisted inventory path: {relative}",
            )
            require(relative not in declarations, f"duplicate persisted inventory path: {relative}")
            digest = str(row["sha256"])
            require(
                len(digest) == 64 and all(character in "0123456789abcdef" for character in digest),
                f"invalid persisted inventory digest for {relative}",
            )
            declarations[relative] = (digest, int(row["size_bytes"]))
    actual = {
        path.relative_to(artifact_dir)
        for path in artifact_dir.rglob("*")
        if path.is_file() and path.relative_to(artifact_dir) not in special
    }
    require(actual == set(declarations), "persisted artifact file set differs from inventory")
    for relative, (digest, size) in declarations.items():
        path = artifact_dir / relative
        require(path.stat().st_size == size, f"persisted artifact size differs: {relative}")
        require(sha256_file(path) == digest, f"persisted artifact digest differs: {relative}")
    canonical_bytes = canonical_path.read_bytes()
    canonical_digest = sha256_bytes(canonical_bytes)
    require(
        canonical_sha_path.read_text(encoding="ascii")
        == f"{canonical_digest}  canonical_results.yaml\n",
        "persisted canonical YAML checksum companion differs",
    )
    canonical = yaml.safe_load(canonical_bytes)
    require(isinstance(canonical, dict), "persisted canonical YAML root is not an object")
    finalizer_sha256 = sha256_file(Path(__file__).resolve())
    source_artifacts = expected_source_artifacts(finalizer_sha256)
    validate_persisted_sources(artifact_dir, source_artifacts)
    derived = derive_dataset(
        artifact_dir / "pilot" / "inputs" / "manifest.json",
        artifact_dir
        / "pilot"
        / "source_aggregate"
        / DEFAULT_PILOT_RESULTS.name,
        artifact_dir / "pilot" / "shards",
        artifact_dir / "pilot" / "raw",
        artifact_dir / "pilot" / "progress" / DEFAULT_PILOT_PROGRESS.name,
        artifact_dir / "comb" / "sweep",
        artifact_dir,
        artifact_dir / "pilot" / "sources" / "runl_repeat_generate_data.py",
    )
    source_state = expected_source_state(derived["pilot_summary"], finalizer_sha256)
    expected = build_document(
        derived,
        artifact_dir,
        inventory_digest,
        len(declarations),
        validate_generated_at(canonical.get("generated_at_utc")),
        source_state,
        source_artifacts,
    )
    require(
        canonical == expected,
        "persisted canonical YAML does not equal the result regenerated from raw evidence",
    )
    expected_canonical_bytes = yaml.safe_dump(
        expected,
        sort_keys=False,
        allow_unicode=False,
        width=120,
    ).encode("utf-8")
    require(
        canonical_bytes == expected_canonical_bytes,
        "persisted canonical YAML serialization differs from the reviewed format",
    )
    for relative, expected_bytes in expected_table_bytes(derived).items():
        table_path = artifact_dir / "tables" / relative
        require(table_path.is_file(), f"persisted table is missing: {relative}")
        require(
            table_path.read_bytes() == expected_bytes,
            f"persisted table differs from regenerated raw evidence: {relative}",
        )
    validate_exact_artifact_layout(artifact_dir, source_artifacts)
    if results_exist:
        require(results_yaml.is_file(), "existing results YAML is not a regular file")
        require(not results_yaml.is_symlink(), "existing results YAML is a symlink")
        require(
            results_yaml.read_bytes() == canonical_bytes,
            "root results YAML differs from persisted canonical YAML",
        )
        action = "already_complete"
    else:
        atomic_write_bytes(results_yaml, canonical_bytes)
        action = "recovered_root_yaml"
    return {
        "status": "complete_pass",
        "publication_action": action,
        "artifact_dir": str(artifact_dir),
        "results_yaml": str(results_yaml),
        "results_yaml_sha256": canonical_digest,
        "inventory_sha256": inventory_digest,
        "inventory_entry_count": len(declarations),
        "case_count": len(derived["cases"]),
        "runl_record_count": len(derived["call_rows"]),
        "paf_record_count": len(derived["paf_rows"]),
    }


def minimap_source_paths(root: Path) -> list[Path]:
    return sorted(
        path
        for path in root.iterdir()
        if path.is_file()
        and (
            path.suffix in {".c", ".h"}
            or path.name
            in {
                ".gitignore",
                ".gitmodules",
                "LICENSE.txt",
                "Makefile",
                "Makefile.simde",
                "README.md",
            }
        )
    )


def source_tree_fingerprint(
    root: Path, *, require_exact_file_set: bool = False
) -> dict[str, Any]:
    require(root.is_dir(), f"source tree is not a directory: {root}")
    require(not root.is_symlink(), f"source tree is a symlink: {root}")
    selected = set(minimap_source_paths(root))
    require(
        all(not path.is_symlink() for path in selected),
        f"source tree contains a selected symlink: {root}",
    )
    if require_exact_file_set:
        all_files = {
            path for path in root.rglob("*") if path.is_file() or path.is_symlink()
        }
        require(
            all(not path.is_symlink() for path in all_files),
            f"source tree contains a symlink: {root}",
        )
        require(
            all_files == selected,
            f"source tree contains an unexpected file: {root}",
        )
    rows = [
        {
            "path": path.name,
            "sha256": sha256_file(path),
            "size_bytes": path.stat().st_size,
        }
        for path in minimap_source_paths(root)
    ]
    return {
        "path": "sources/minimap2_source",
        "file_count": len(rows),
        "canonical_manifest_sha256": canonical_json_sha256(rows),
    }


def expected_source_artifacts(finalizer_sha256: str) -> dict[str, Any]:
    files = {
        "pilot_generator": {
            "path": "pilot/sources/runl_repeat_generate_data.py",
            "sha256": EXPECTED_PILOT_GENERATOR_SHA256,
        },
        "pilot_common": {
            "path": "pilot/sources/runl_repeat_common.py",
            "sha256": EXPECTED_COMMON_SHA256,
        },
        "pilot_runner": {
            "path": "pilot/sources/runl_repeat_run_experiment.py",
            "sha256": EXPECTED_PILOT_RUNNER_SHA256,
            "reconstructed_and_hash_verified": True,
        },
        "pilot_lchain": {
            "path": "pilot/sources/lchain.c",
            "sha256": EXPECTED_LCHAIN_SHA256,
        },
        "comb_generator": {
            "path": "comb/sources/runl_comb_generate.py",
            "sha256": EXPECTED_COMB_GENERATOR_SHA256,
        },
        "comb_adapter": {
            "path": "comb/sources/runl_comb_adapt_manifest.py",
            "sha256": EXPECTED_COMB_ADAPTER_SHA256,
        },
        "current_runner": {
            "path": "comb/sources/runl_repeat_run_experiment.py",
            "sha256": EXPECTED_CURRENT_RUNNER_SHA256,
        },
        "comb_common": {
            "path": "comb/sources/runl_repeat_common.py",
            "sha256": EXPECTED_COMMON_SHA256,
        },
        "comb_lchain": {
            "path": "comb/sources/lchain.c",
            "sha256": EXPECTED_LCHAIN_SHA256,
        },
        "map_instrumentation": {
            "path": "sources/minimap2_source/map.c",
            "sha256": EXPECTED_MAP_C_SHA256,
        },
        "makefile": {
            "path": "sources/minimap2_source/Makefile",
            "sha256": EXPECTED_MAKEFILE_SHA256,
        },
        "pilot_binary": {
            "path": "pilot/binary/minimap2",
            "sha256": EXPECTED_BINARY_SHA256,
        },
        "comb_binary": {
            "path": "comb/binary/minimap2",
            "sha256": EXPECTED_BINARY_SHA256,
        },
        "finalizer": {
            "path": f"sources/{Path(__file__).name}",
            "sha256": finalizer_sha256,
        },
    }
    return {
        "path_scope": "relative_to_artifact_root",
        "files": files,
        "minimap2_source_tree": {
            "path": "sources/minimap2_source",
            "file_count": EXPECTED_MINIMAP_SOURCE_FILE_COUNT,
            "canonical_manifest_sha256": EXPECTED_MINIMAP_SOURCE_TREE_SHA256,
        },
    }


def validate_persisted_sources(
    artifact_root: Path, declarations: dict[str, Any]
) -> None:
    require(
        declarations == expected_source_artifacts(sha256_file(Path(__file__).resolve())),
        "persisted source declaration differs from this reviewed finalizer revision",
    )
    for role, declaration in declarations["files"].items():
        path = path_below(
            artifact_root / str(declaration["path"]),
            artifact_root,
            f"persisted source {role}",
        )
        require(path.is_file(), f"persisted source is missing: {role}")
        require(not path.is_symlink(), f"persisted source is a symlink: {role}")
        require(
            sha256_file(path) == declaration["sha256"],
            f"persisted source digest differs: {role}",
        )
    archive = artifact_root / declarations["minimap2_source_tree"]["path"]
    observed_tree = source_tree_fingerprint(archive, require_exact_file_set=True)
    require(
        observed_tree == declarations["minimap2_source_tree"],
        "persisted minimap2 source tree fingerprint differs",
    )
    expected_scoped_files = {
        Path(declaration["path"])
        for declaration in declarations["files"].values()
    }
    expected_scoped_files.update(
        Path(declarations["minimap2_source_tree"]["path"]) / path.name
        for path in minimap_source_paths(archive)
    )
    scoped_roots = (
        artifact_root / "pilot" / "sources",
        artifact_root / "pilot" / "binary",
        artifact_root / "comb" / "sources",
        artifact_root / "comb" / "binary",
        artifact_root / "sources",
    )
    actual_scoped_files = {
        path.relative_to(artifact_root)
        for root in scoped_roots
        for path in root.rglob("*")
        if path.is_file()
    }
    require(
        actual_scoped_files == expected_scoped_files,
        "persisted source/binary file set differs from its exact declaration",
    )


def validate_exact_artifact_layout(
    artifact_root: Path, source_declarations: dict[str, Any]
) -> None:
    expected = {
        Path("inventory.csv"),
        Path("inventory.sha256"),
        Path("canonical_results.yaml"),
        Path("canonical_results.yaml.sha256"),
        Path("pilot/source_aggregate") / DEFAULT_PILOT_RESULTS.name,
        Path("pilot/progress") / DEFAULT_PILOT_PROGRESS.name,
        *(Path("tables") / name for name in (
            "cases.csv",
            "runl_calls.csv",
            "paf_records.csv",
            "tikz_mosaic.csv",
            "tikz_spaced.csv",
            "tikz_comb.csv",
        )),
    }
    for relative_root in (
        Path("pilot/inputs"),
        Path("pilot/shards"),
        Path("pilot/raw"),
        Path("comb/sweep"),
    ):
        root = artifact_root / relative_root
        expected.update(
            relative_root / path.relative_to(root)
            for path in root.rglob("*")
            if path.is_file()
        )
    expected.update(
        Path(declaration["path"])
        for declaration in source_declarations["files"].values()
    )
    archive_relative = Path(source_declarations["minimap2_source_tree"]["path"])
    archive = artifact_root / archive_relative
    expected.update(archive_relative / path.name for path in minimap_source_paths(archive))
    actual = {
        path.relative_to(artifact_root)
        for path in artifact_root.rglob("*")
        if path.is_file()
    }
    require(actual == expected, "persisted artifact tree has missing or unexpected files")


def write_sources(staging: Path) -> dict[str, Any]:
    pilot_sources = staging / "pilot" / "sources"
    comb_sources = staging / "comb" / "sources"
    shared_sources = staging / "sources"
    for name in ("runl_repeat_generate_data.py", "runl_repeat_common.py", "lchain.c"):
        copy_file_verified(SOURCE_ROOT / name, pilot_sources / name)
    reconstructed = reconstruct_pilot_runner()
    (pilot_sources / "runl_repeat_run_experiment.py").parent.mkdir(
        parents=True, exist_ok=True
    )
    (pilot_sources / "runl_repeat_run_experiment.py").write_bytes(reconstructed)
    require(
        sha256_file(pilot_sources / "runl_repeat_run_experiment.py")
        == EXPECTED_PILOT_RUNNER_SHA256,
        "persisted pilot runner digest differs",
    )
    for name in (
        "runl_comb_generate.py",
        "runl_comb_adapt_manifest.py",
        "runl_repeat_run_experiment.py",
        "runl_repeat_common.py",
        "lchain.c",
    ):
        copy_file_verified(SOURCE_ROOT / name, comb_sources / name)
    minimap_sources = minimap_source_paths(SOURCE_ROOT)
    require(
        source_tree_fingerprint(SOURCE_ROOT)
        == {
            "path": "sources/minimap2_source",
            "file_count": EXPECTED_MINIMAP_SOURCE_FILE_COUNT,
            "canonical_manifest_sha256": EXPECTED_MINIMAP_SOURCE_TREE_SHA256,
        },
        "current minimap2 source tree differs from the reviewed tree",
    )
    for path in sorted(minimap_sources):
        copy_file_verified(path, shared_sources / "minimap2_source" / path.name)
    copy_file_verified(Path(__file__).resolve(), shared_sources / Path(__file__).name)
    copy_file_verified(SOURCE_ROOT / "minimap2", staging / "pilot" / "binary" / "minimap2")
    copy_file_verified(SOURCE_ROOT / "minimap2", staging / "comb" / "binary" / "minimap2")
    declarations = expected_source_artifacts(sha256_file(Path(__file__).resolve()))
    validate_persisted_sources(staging, declarations)
    return declarations


def inventory_rows(staging: Path) -> list[dict[str, Any]]:
    excluded = {Path("inventory.csv"), Path("inventory.sha256"), Path("canonical_results.yaml"), Path("canonical_results.yaml.sha256")}
    rows: list[dict[str, Any]] = []
    for path in sorted(item for item in staging.rglob("*") if item.is_file()):
        relative = path.relative_to(staging)
        if relative in excluded:
            continue
        rows.append(
            {
                "path": str(relative),
                "sha256": sha256_file(path),
                "size_bytes": path.stat().st_size,
            }
        )
    return rows


def utc_timestamp() -> str:
    from datetime import datetime, timezone

    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def construction_definitions() -> dict[str, Any]:
    return {
        "mosaic": {
            "actual_reference_and_query_layout": "U0[512]+A[32768]+U1[512]+A_prime[32768]+U2[512]",
            "reference_query_relation": "byte-identical complete composite sequences with different names",
            "total_length": 67_072,
            "a_to_a_prime_offset": 33_280,
            "default_bw_long": 20_000,
            "nested_substitution_counts": {
                "0.20": 6_554,
                "0.10": 3_277,
                "0.05": 1_638,
                "0.02": 655,
                "0.01": 328,
                "0.005": 164,
                "0.002": 66,
                "0": 0,
            },
            "independent_control": "A_prime is an independently generated DNA block",
            "expected_reference_occurrence_multiplicities": [1, 2],
            "paf_gate": "exact names and sequence lengths, plus strand, at least 0.99 query and target coverage",
            "path_gate": "all selected first-path anchors lie on the exact diagonal",
            "interpretation_warning": "the level is internal A/A_prime divergence, not sequencing error",
        },
        "spaced_copy": {
            "actual_reference_layout": "S[4096]+N[16384]",
            "actual_query_layout": "(S[4096]+N[16384])^R",
            "copy_counts": [1, 2, 4, 8, 16, 32, 64],
            "copy_pitch": 20_480,
            "default_max_chain_gap": 5_000,
            "default_bw_long": 20_000,
            "boundary_context": "the reference and every query copy, including the final copy, end with the same N guard",
            "expected_reference_occurrence_multiplicity": 1,
            "expected_top_score_multiplicity": "R",
            "paf_gate": "exact names and lengths, plus strand, at least 90 percent of S on both axes",
            "path_gate": "the complete selected path stays on one inferred declared copy diagonal",
            "interpretation_warning": (
                "the reference multiplicity is fixed at one, so reference-frequency filtering is bypassed. "
                "the R>=2 saturation is a constructed zero-use control, not a scaling law or prevalence estimate"
            ),
        },
        "comb": {
            "role": "unique-minimizer coordinate/island adversarial control, not repeat-frequency evidence",
            "m_values": [1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024],
            "anchor_count": "40*m+51",
            "first_path_length": "m+90",
            "top_score": 615,
            "top_score_multiplicity": "m",
            "tie_policy": "candidates sorted by increasing (score,index) and consumed from the end",
            "selected_start": "r_m_39 at anchor index n-1",
            "paf_gate": "one exact-name, exact-length, plus-strand record spans r_m_02 through r_m_39 on both axes",
        },
    }


def add_persisted_paths(
    pilot_summary: dict[str, Any],
    comb_summary: dict[str, Any],
    artifact_dir: Path,
    pilot_results_name: str,
) -> None:
    pilot_summary["persisted_paths"] = {
        "manifest": artifact_reference(
            artifact_dir, Path("pilot") / "inputs" / "manifest.json"
        ),
        "source_aggregate": artifact_reference(
            artifact_dir,
            Path("pilot") / "source_aggregate" / pilot_results_name,
        ),
        "raw_root": artifact_reference(artifact_dir, Path("pilot") / "raw"),
        "shard_root": artifact_reference(artifact_dir, Path("pilot") / "shards"),
        "progress_log": artifact_reference(
            artifact_dir,
            Path("pilot") / "progress" / DEFAULT_PILOT_PROGRESS.name,
        ),
    }
    comb_summary["persisted_paths"] = {
        "sweep_root": artifact_reference(artifact_dir, Path("comb") / "sweep"),
        "checksum_manifest": artifact_reference(
            artifact_dir, Path("comb") / "sweep" / "SHA256SUMS"
        ),
        "provenance": artifact_reference(
            artifact_dir, Path("comb") / "sweep" / "provenance.json"
        ),
    }


def derive_dataset(
    pilot_manifest: Path,
    pilot_results: Path,
    pilot_shards: Path,
    pilot_raw: Path,
    pilot_progress: Path,
    comb_root: Path,
    artifact_dir: Path,
    pilot_generator: Path,
) -> dict[str, Any]:
    pilot_cases, pilot_summary = validate_pilot(
        pilot_manifest,
        pilot_results,
        pilot_shards,
        pilot_raw,
        pilot_progress,
        artifact_dir,
        generator_path=pilot_generator,
    )
    comb_cases, comb_summary = validate_comb(comb_root, artifact_dir)
    add_persisted_paths(
        pilot_summary, comb_summary, artifact_dir, pilot_results.name
    )
    cases = pilot_cases + comb_cases
    require(len(cases) == 59, f"merged case count is {len(cases)}, expected 59")
    require(
        len({(case["source_profile"], case["case_id"]) for case in cases})
        == len(cases),
        "duplicate merged case identity",
    )
    call_rows = make_call_rows(cases)
    case_rows = make_case_rows(cases)
    paf_rows = make_paf_rows(cases)
    require(
        len(call_rows)
        == sum(len(case["raw_telemetry"]["runl"]) for case in cases),
        "call table lost a raw telemetry record",
    )
    require(
        len(paf_rows) == sum(len(case["full_paf_records"]) for case in cases),
        "PAF table lost a record",
    )
    tikz_tables = {
        construction: make_tikz_rows(call_rows, construction)
        for construction in ("mosaic", "spaced", "comb")
    }
    return {
        "pilot_cases": pilot_cases,
        "comb_cases": comb_cases,
        "pilot_summary": pilot_summary,
        "comb_summary": comb_summary,
        "cases": cases,
        "call_rows": call_rows,
        "case_rows": case_rows,
        "paf_rows": paf_rows,
        "tikz_tables": tikz_tables,
    }


def metric_definitions() -> dict[str, Any]:
    return {
        "predicate_firing_percent": "100*fast_batches/loop_evaluations",
        "bounds_conditional_percent": "100*fast_batches/bounds_pass",
        "path_coverage_percent": "100*fast_covered_anchors/primary_path_anchors",
        "undefined_policy": (
            "each rate is null whenever its own denominator is zero. No-ranked-end calls therefore have null rates. "
            "a zero numerator with a positive denominator is zero"
        ),
        "initial_dp_realized_duplicate_fraction": (
            "case-level fraction measured on the initial DP selected path only. It is carried beside later calls "
            "only as an explicitly named input covariate and is not an RMQ-path measurement"
        ),
        "initial_dp_maximum_same_target_group_size": (
            "maximum same-target group size measured on the initial DP selected path only"
        ),
        "aggregation_policy": (
            "DP and RMQ are separate. Replicate median/min/max are accompanied by ratios of summed raw counters"
        ),
    }


def table_references(artifact_dir: Path) -> dict[str, str]:
    root = str(artifact_dir.relative_to(WORKSPACE_ROOT))
    return {
        "cases": f"{root}/tables/cases.csv",
        "runl_calls": f"{root}/tables/runl_calls.csv",
        "paf_records": f"{root}/tables/paf_records.csv",
        "tikz_mosaic": f"{root}/tables/tikz_mosaic.csv",
        "tikz_spaced": f"{root}/tables/tikz_spaced.csv",
        "tikz_comb": f"{root}/tables/tikz_comb.csv",
    }


def expected_table_bytes(derived: dict[str, Any]) -> dict[Path, bytes]:
    rendered = {
        Path("cases.csv"): render_csv(CASE_CSV_FIELDS, derived["case_rows"]),
        Path("runl_calls.csv"): render_csv(RUNL_CSV_FIELDS, derived["call_rows"]),
        Path("paf_records.csv"): render_csv(PAF_CSV_FIELDS, derived["paf_rows"]),
    }
    for construction, rows in derived["tikz_tables"].items():
        fieldnames = list(rows[0]) if rows else ["construction"]
        rendered[Path(f"tikz_{construction}.csv")] = render_csv(fieldnames, rows)
    return rendered


def build_document(
    derived: dict[str, Any],
    artifact_dir: Path,
    inventory_sha256: str,
    inventory_entry_count: int,
    generated_at_utc: str,
    source_state: dict[str, Any],
    source_artifacts: dict[str, Any],
) -> dict[str, Any]:
    cases = derived["cases"]
    call_rows = derived["call_rows"]
    paf_rows = derived["paf_rows"]
    return {
        "schema_version": 1,
        "experiment_id": EXPERIMENT_ID,
        "experiment_status": "complete_pass",
        "generated_at_utc": generated_at_utc,
        "artifact_root": {
            "path": str(artifact_dir.relative_to(WORKSPACE_ROOT)),
            "inventory_path": (
                f"{artifact_dir.relative_to(WORKSPACE_ROOT)}/inventory.csv"
            ),
            "inventory_sha256": inventory_sha256,
            "inventory_entry_count": inventory_entry_count,
            "copy_policy": "independent verified copies. No symlinks and no moves",
        },
        "summary": {
            "case_count": len(cases),
            "accepted": len(cases),
            "rejected": 0,
            "pilot_case_count": len(derived["pilot_cases"]),
            "comb_case_count": len(derived["comb_cases"]),
            "runl_record_count": len(call_rows),
            "paf_record_count": len(paf_rows),
            "lane_widths": list(LANE_WIDTHS),
        },
        "metric_definitions": metric_definitions(),
        "construction_definitions": construction_definitions(),
        "provenance": {
            "pilot": derived["pilot_summary"],
            "comb": derived["comb_summary"],
            "source_state": source_state,
            "persisted_source_artifacts": source_artifacts,
        },
        "tables": table_references(artifact_dir),
        "tikz_aggregates": derived["tikz_tables"],
        "cases": cases,
    }


def validate_generated_at(value: Any) -> str:
    from datetime import datetime

    require(isinstance(value, str) and value.endswith("Z"), "invalid generated_at_utc")
    try:
        parsed = datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as error:
        raise ValueError("invalid generated_at_utc") from error
    require(parsed.utcoffset() is not None, "generated_at_utc lacks timezone")
    return value


def main() -> int:
    arguments = parse_args()
    pilot_manifest = arguments.pilot_manifest.resolve()
    pilot_results = arguments.pilot_results.resolve()
    pilot_shards = arguments.pilot_shards.resolve()
    pilot_raw = arguments.pilot_raw.resolve()
    pilot_progress = arguments.pilot_progress.resolve()
    comb_root = arguments.comb_root.resolve()
    artifact_dir = arguments.artifact_dir.resolve()
    results_yaml = arguments.results_yaml.resolve()
    path_below(artifact_dir, WORKSPACE_ROOT, "--artifact-dir")
    path_below(results_yaml, WORKSPACE_ROOT, "--results-yaml")
    require(not artifact_dir.is_relative_to(SOURCE_ROOT), "artifact destination must be outside disposable source root")
    require(not results_yaml.is_relative_to(SOURCE_ROOT), "results YAML must be outside disposable source root")
    require(artifact_dir != WORKSPACE_ROOT, "artifact destination must not be workspace root")
    require(results_yaml.parent == WORKSPACE_ROOT, "results YAML must be a workspace-root file")
    require(artifact_dir != results_yaml, "artifact directory and results YAML must differ")
    publication_lock_fd: int | None = None
    if not arguments.validate_only:
        publication_lock_fd = acquire_publication_lock()
        recovered = validate_persisted_artifact_bundle(artifact_dir, results_yaml)
        if recovered is not None:
            os.close(publication_lock_fd)
            print(json.dumps(recovered, sort_keys=True))
            return 0
    for path, label in (
        (pilot_manifest, "--pilot-manifest"),
        (pilot_results, "--pilot-results"),
        (pilot_shards, "--pilot-shards"),
        (pilot_raw, "--pilot-raw"),
        (pilot_progress, "--pilot-progress"),
        (comb_root, "--comb-root"),
    ):
        path_below(path, SOURCE_ROOT, label)

    derived = derive_dataset(
        pilot_manifest,
        pilot_results,
        pilot_shards,
        pilot_raw,
        pilot_progress,
        comb_root,
        artifact_dir,
        SOURCE_ROOT / "runl_repeat_generate_data.py",
    )
    source_state = validate_source_state(derived["pilot_summary"])
    if arguments.validate_only:
        print(
            json.dumps(
                {
                    "status": "validation_passed",
                    "case_count": len(derived["cases"]),
                    "pilot_case_count": len(derived["pilot_cases"]),
                    "comb_case_count": len(derived["comb_cases"]),
                    "runl_record_count": len(derived["call_rows"]),
                    "paf_record_count": len(derived["paf_rows"]),
                    "source_state": source_state,
                },
                sort_keys=True,
            )
        )
        return 0

    require(not artifact_dir.exists(), f"refusing existing artifact destination: {artifact_dir}")
    require(not results_yaml.exists(), f"refusing existing results YAML: {results_yaml}")
    artifact_dir.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(
        tempfile.mkdtemp(prefix=f".{artifact_dir.name}.tmp.", dir=artifact_dir.parent)
    )
    prepared_root: Path | None = None
    try:
        copy_tree_verified(pilot_manifest.parent, staging / "pilot" / "inputs")
        copy_tree_verified(pilot_shards, staging / "pilot" / "shards")
        copy_tree_verified(pilot_raw, staging / "pilot" / "raw")
        copy_file_verified(
            pilot_progress,
            staging / "pilot" / "progress" / pilot_progress.name,
        )
        copy_file_verified(pilot_results, staging / "pilot" / "source_aggregate" / pilot_results.name)
        copy_tree_verified(comb_root, staging / "comb" / "sweep")
        source_artifacts = write_sources(staging)

        staged_derived = derive_dataset(
            staging / "pilot" / "inputs" / "manifest.json",
            staging / "pilot" / "source_aggregate" / pilot_results.name,
            staging / "pilot" / "shards",
            staging / "pilot" / "raw",
            staging / "pilot" / "progress" / pilot_progress.name,
            staging / "comb" / "sweep",
            artifact_dir,
            staging / "pilot" / "sources" / "runl_repeat_generate_data.py",
        )
        require(
            staged_derived == derived,
            "staged evidence differs from the validation-time dataset",
        )
        staged_finalizer_sha256 = sha256_file(
            staging / "sources" / Path(__file__).name
        )
        require(
            source_state
            == expected_source_state(
                staged_derived["pilot_summary"], staged_finalizer_sha256
            ),
            "staged source state differs from the validation-time state",
        )
        require(
            source_artifacts == expected_source_artifacts(staged_finalizer_sha256),
            "staged source declarations differ",
        )
        validate_persisted_sources(staging, source_artifacts)
        derived = staged_derived

        tables = staging / "tables"
        for relative, content in expected_table_bytes(derived).items():
            destination = tables / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(content)

        inventory = inventory_rows(staging)
        write_csv(
            staging / "inventory.csv",
            ("path", "sha256", "size_bytes"),
            inventory,
        )
        inventory_digest = sha256_file(staging / "inventory.csv")
        (staging / "inventory.sha256").write_text(
            f"{inventory_digest}  inventory.csv\n", encoding="ascii"
        )
        document = build_document(
            derived,
            artifact_dir,
            inventory_digest,
            len(inventory),
            utc_timestamp(),
            source_state,
            source_artifacts,
        )
        yaml_bytes = yaml.safe_dump(
            document,
            sort_keys=False,
            allow_unicode=False,
            width=120,
        ).encode("utf-8")
        canonical_path = staging / "canonical_results.yaml"
        canonical_path.write_bytes(yaml_bytes)
        canonical_digest = sha256_bytes(yaml_bytes)
        (staging / "canonical_results.yaml.sha256").write_text(
            f"{canonical_digest}  canonical_results.yaml\n", encoding="ascii"
        )
        validate_exact_artifact_layout(staging, source_artifacts)
        require(not artifact_dir.exists(), f"artifact destination appeared: {artifact_dir}")
        require(not results_yaml.exists(), f"results destination appeared: {results_yaml}")
        fsync_tree(staging)
        prepared_root = prepare_root_temp(results_yaml, yaml_bytes)
        rename_noreplace(staging, artifact_dir)
        fsync_directory(artifact_dir.parent)
        rename_noreplace(prepared_root, results_yaml)
        fsync_directory(results_yaml.parent)
        prepared_root = None
        require(publication_lock_fd is not None, "publication lock was not acquired")
        os.close(publication_lock_fd)
        publication_lock_fd = None
        print(
            json.dumps(
                {
                    "status": "complete_pass",
                    "artifact_dir": str(artifact_dir),
                    "results_yaml": str(results_yaml),
                    "results_yaml_sha256": canonical_digest,
                    "inventory_sha256": inventory_digest,
                    "inventory_entry_count": len(inventory),
                    "case_count": len(derived["cases"]),
                    "runl_record_count": len(derived["call_rows"]),
                    "paf_record_count": len(derived["paf_rows"]),
                },
                sort_keys=True,
            )
        )
    except BaseException:
        if staging.exists():
            shutil.rmtree(staging)
        if prepared_root is not None and prepared_root.exists():
            prepared_root.unlink()
        raise
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
