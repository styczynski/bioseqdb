#!/usr/bin/env python3
"""Validate one follow-up Run_L path against its planted reference copies.

The program reruns one manifest-defined minimap2 mapping with the experiment
dump and seed diagnostics enabled.  It reconstructs the ranked first
predecessor path from the complete ``f`` and ``p`` arrays, evaluates the exact
Run_L predicate for L=4, 8, and 16, and joins predecessor indices to the anchor
coordinates printed by minimap2.  No BioSeqDB binary or build is involved.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time
import uuid
from typing import Any, Iterable


ROOT = Path(__file__).resolve().parent
DEFAULT_MANIFEST = ROOT / "followup_data_pilot_compact" / "manifest.json"
DEFAULT_MINIMAP2 = ROOT / "minimap2"
LANE_WIDTHS = (4, 8, 16)
RUNL_PREFIX = "MM_RUNL\t"
SD_PREFIX = "SD\t"
FAILURE_REPORT_CONTEXT: dict[str, Any] | None = None


@dataclass(frozen=True)
class Anchor:
    """One seed-hit anchor in minimap2's pre-chaining array order."""

    index: int
    reference_name: str
    target_end: int
    strand: str
    query_end: int
    span: int
    printed_diagonal_delta: int


@dataclass(frozen=True)
class Dump:
    """Complete chaining state written by the experiment instrumentation."""

    path: Path
    stage: str
    call_id: int
    anchor_count: int
    candidate_end_count: int
    min_score: int
    candidates: tuple[tuple[int, int, int], ...]
    scores: tuple[int, ...]
    predecessors: tuple[int, ...]


def utc_now() -> str:
    """Return a stable UTC timestamp."""

    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def sha256_file(path: Path) -> str:
    """Return the streaming SHA-256 digest of a file."""

    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def canonical_json_sha256(value: object) -> str:
    """Match the generator's canonical JSON digest."""

    encoded = json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def atomic_json(path: Path, value: object) -> None:
    """Write JSON completely before replacing the destination."""

    encoded = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    temporary = path.with_name(f".{path.name}.tmp.{os.getpid()}.{uuid.uuid4().hex}")
    try:
        with temporary.open("xb") as output:
            output.write(encoded)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def resolve_declared_path(manifest_path: Path, value: str) -> Path:
    """Resolve a manifest artifact relative to its manifest."""

    path = Path(value)
    if path.is_absolute():
        raise ValueError(f"manifest artifact paths must be relative: {value!r}")
    root = manifest_path.parent.resolve()
    resolved = (root / path).resolve()
    try:
        resolved.relative_to(root)
    except ValueError as error:
        raise ValueError(f"manifest artifact escapes its directory: {value!r}") from error
    return resolved


def read_single_fasta(path: Path) -> tuple[str, bytes]:
    """Read one FASTA record and reject ambiguous structure or bases."""

    name: str | None = None
    sequence = bytearray()
    with path.open("rb") as source:
        for line_number, raw_line in enumerate(source, start=1):
            line = raw_line.rstrip(b"\r\n")
            if line.startswith(b">"):
                if name is not None:
                    raise ValueError(f"{path}:{line_number}: multiple FASTA records")
                token = line[1:].split(maxsplit=1)[0]
                if not token:
                    raise ValueError(f"{path}:{line_number}: empty FASTA name")
                name = token.decode("ascii")
            else:
                if name is None:
                    raise ValueError(f"{path}:{line_number}: sequence precedes FASTA header")
                compact = b"".join(line.split()).upper()
                if any(base not in b"ACGT" for base in compact):
                    raise ValueError(f"{path}:{line_number}: expected only A/C/G/T bases")
                sequence.extend(compact)
    if name is None or not sequence:
        raise ValueError(f"{path}: expected one nonempty FASTA record")
    return name, bytes(sequence)


def kmp_prefix(pattern: bytes) -> list[int]:
    """Build a prefix table for overlapping exact-match detection."""

    prefix = [0] * len(pattern)
    matched = 0
    for index in range(1, len(pattern)):
        while matched and pattern[index] != pattern[matched]:
            matched = prefix[matched - 1]
        if pattern[index] == pattern[matched]:
            matched += 1
        prefix[index] = matched
    return prefix


def audit_reference_fasta(
    path: Path, pattern: bytes, placements: list[dict[str, int]]
) -> dict[str, Any]:
    """Independently verify source slices and all overlapping source occurrences."""

    if not pattern:
        raise ValueError("source sequence must not be empty")
    ordered = sorted(placements, key=lambda value: value["start"])
    prefix = kmp_prefix(pattern)
    matches: list[int] = []
    name: str | None = None
    position = 0
    matched = 0
    placement_index = 0
    slice_mismatches: list[dict[str, int]] = []
    with path.open("rb") as source:
        for line_number, raw_line in enumerate(source, start=1):
            line = raw_line.rstrip(b"\r\n")
            if line.startswith(b">"):
                if name is not None:
                    raise ValueError(f"{path}:{line_number}: multiple FASTA records")
                token = line[1:].split(maxsplit=1)[0]
                if not token:
                    raise ValueError(f"{path}:{line_number}: empty FASTA name")
                name = token.decode("ascii")
                continue
            if name is None:
                raise ValueError(f"{path}:{line_number}: sequence precedes FASTA header")
            compact = b"".join(line.split()).upper()
            if any(base not in b"ACGT" for base in compact):
                raise ValueError(f"{path}:{line_number}: expected only A/C/G/T bases")
            for base in compact:
                while placement_index < len(ordered) and position >= ordered[placement_index]["end"]:
                    placement_index += 1
                if placement_index < len(ordered):
                    placement = ordered[placement_index]
                    if position >= placement["start"]:
                        expected = pattern[position - placement["start"]]
                        if base != expected and len(slice_mismatches) < 100:
                            slice_mismatches.append(
                                {
                                    "copy_rank": placement["rank"],
                                    "reference_position": position,
                                    "source_offset": position - placement["start"],
                                }
                            )
                while matched and base != pattern[matched]:
                    matched = prefix[matched - 1]
                if base == pattern[matched]:
                    matched += 1
                if matched == len(pattern):
                    matches.append(position - len(pattern) + 1)
                    matched = prefix[matched - 1]
                position += 1
    if name is None or position == 0:
        raise ValueError(f"{path}: expected one nonempty FASTA record")
    return {
        "reference_name": name,
        "reference_length": position,
        "overlapping_exact_source_occurrence_count": len(matches),
        "overlapping_exact_source_positions": matches,
        "overlapping_exact_source_positions_sha256": canonical_json_sha256(matches),
        "all_declared_copy_slices_equal_source": not slice_mismatches,
        "copy_slice_mismatch_count_capped": len(slice_mismatches),
        "copy_slice_mismatches_first_100": slice_mismatches,
    }


def require_file_hash(path: Path, expected: Any, label: str) -> dict[str, Any]:
    """Check one manifest-declared artifact before running minimap2."""

    if not path.is_file():
        raise ValueError(f"{label} does not exist: {path}")
    observed = sha256_file(path)
    if not isinstance(expected, str) or observed != expected:
        raise ValueError(
            f"{label} SHA-256 mismatch: expected {expected!r}, observed {observed}"
        )
    return {"path": str(path.resolve()), "sha256": observed, "size_bytes": path.stat().st_size}


def select_dataset(
    manifest: dict[str, Any], dataset_id: str | None, replicate_id: str | None
) -> dict[str, Any]:
    """Select exactly one manifest dataset."""

    datasets = manifest.get("datasets")
    if manifest.get("schema_version") != 2 or manifest.get("status") != "complete":
        raise ValueError("the input must be a complete schema-version-2 manifest")
    if not isinstance(datasets, list) or not datasets:
        raise ValueError("manifest.datasets must be a nonempty array")
    selected = [
        value
        for value in datasets
        if isinstance(value, dict)
        and (dataset_id is None or value.get("dataset_id") == dataset_id)
        and (replicate_id is None or value.get("replicate_id") == replicate_id)
    ]
    if len(selected) != 1:
        names = [value.get("dataset_id") for value in selected]
        raise ValueError(
            "dataset selection must identify exactly one dataset; "
            f"matched {len(selected)}: {names}"
        )
    return selected[0]


def select_arm_args(manifest: dict[str, Any], arm_id: str) -> tuple[list[str], list[str], float]:
    """Return the declared default and selected-arm mapping arguments."""

    mapping = manifest.get("mapping")
    if not isinstance(mapping, dict):
        raise ValueError("manifest.mapping must be an object")
    defaults = mapping.get("default_args")
    if not isinstance(defaults, list) or any(not isinstance(item, str) for item in defaults):
        raise ValueError("manifest.mapping.default_args must be an array of strings")
    if int(mapping.get("mapping_threads", 1)) != 1:
        raise ValueError("this validator requires manifest mapping_threads=1")
    raw_arms = mapping.get("arms")
    arms: list[dict[str, Any]]
    if isinstance(raw_arms, list):
        arms = [item for item in raw_arms if isinstance(item, dict)]
    elif isinstance(raw_arms, dict):
        arms = []
        for name, value in raw_arms.items():
            if isinstance(value, list):
                arms.append({"arm_id": name, "args": value})
            elif isinstance(value, dict):
                arms.append({"arm_id": name, **value})
    else:
        raise ValueError("manifest.mapping.arms must be an array or object")
    selected = [arm for arm in arms if str(arm.get("arm_id")) == arm_id]
    if len(selected) != 1:
        raise ValueError(f"mapping arm {arm_id!r} is not declared exactly once")
    arm_args = selected[0].get("args", [])
    if not isinstance(arm_args, list) or any(not isinstance(item, str) for item in arm_args):
        raise ValueError(f"mapping arm {arm_id!r} args must be an array of strings")
    timeout = float(mapping.get("timeout_seconds", 3600))
    if timeout <= 0:
        raise ValueError("mapping timeout must be positive")
    return list(defaults), list(arm_args), timeout


def validate_coordinate_model_args(arguments: Iterable[str]) -> None:
    """Reject mapping modes whose SD coordinates need another interpretation."""

    values = list(arguments)
    if any(
        value.startswith("-")
        and not value.startswith("--")
        and "H" in value[1:]
        for value in values
    ):
        raise ValueError("homopolymer-compressed indexing is not supported by this validator")
    if any(value == "-x" or value.startswith("-x") for value in values):
        raise ValueError(
            "preset arguments are not supported because they may enable a different "
            "coordinate model; declare k/w and other options explicitly"
        )
    if any(
        value.startswith("--")
        and value[2:]
        and "qstrand".startswith(value[2:].split("=", 1)[0])
        for value in values
    ):
        raise ValueError("--qstrand is not supported by this validator")


def parse_placements(path: Path) -> list[dict[str, int]]:
    """Parse and validate the generator's nested placement table."""

    placements: list[dict[str, int]] = []
    with path.open("r", encoding="ascii") as source:
        header = source.readline().rstrip("\n").split("\t")
        if header != ["rank", "cell_index", "start", "end"]:
            raise ValueError(f"{path}: unexpected placement header")
        for line_number, line in enumerate(source, start=2):
            fields = line.rstrip("\n").split("\t")
            if len(fields) != 4:
                raise ValueError(f"{path}:{line_number}: expected four columns")
            rank, cell_index, start, end = map(int, fields)
            if rank != len(placements) + 1 or start < 0 or end <= start:
                raise ValueError(f"{path}:{line_number}: invalid placement")
            placements.append(
                {"rank": rank, "cell_index": cell_index, "start": start, "end": end}
            )
    return placements


def parse_dump(path: Path) -> Dump:
    """Parse one complete experiment dump with strict row checks."""

    metadata: tuple[str, int, int, int, int] | None = None
    candidates: list[tuple[int, int, int]] = []
    scores: list[int] = []
    predecessors: list[int] = []
    with path.open("r", encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            fields = line.rstrip("\n").split("\t")
            try:
                if fields[0] == "META" and len(fields) == 6:
                    if metadata is not None:
                        raise ValueError("duplicate META row")
                    metadata = (
                        fields[1], int(fields[2]), int(fields[3]), int(fields[4]), int(fields[5])
                    )
                elif fields[0] == "Z" and len(fields) == 4:
                    candidates.append((int(fields[1]), int(fields[2]), int(fields[3])))
                elif fields[0] == "P" and len(fields) == 4:
                    index, score, predecessor = map(int, fields[1:])
                    if index != len(scores):
                        raise ValueError(f"nonconsecutive P index {index}")
                    scores.append(score)
                    predecessors.append(predecessor)
                else:
                    raise ValueError(f"unexpected row {fields[0]!r}")
            except (ValueError, IndexError) as error:
                raise ValueError(f"{path}:{line_number}: {error}") from error
    if metadata is None:
        raise ValueError(f"{path}: missing META row")
    stage, call_id, anchor_count, candidate_count, min_score = metadata
    if len(scores) != anchor_count or len(candidates) != candidate_count:
        raise ValueError(f"{path}: row counts disagree with META")
    return Dump(
        path=path,
        stage=stage,
        call_id=call_id,
        anchor_count=anchor_count,
        candidate_end_count=candidate_count,
        min_score=min_score,
        candidates=tuple(candidates),
        scores=tuple(scores),
        predecessors=tuple(predecessors),
    )


def dump_invariants(dump: Dump) -> dict[str, Any]:
    """Check predecessor domains and the experiment's ranked candidate order."""

    expected = sorted(
        (score, index)
        for index, score in enumerate(dump.scores)
        if score >= dump.min_score
    )
    observed = [(score, index) for _, score, index in dump.candidates]
    ranks = [rank for rank, _, _ in dump.candidates]
    predecessor_errors = [
        {"index": index, "predecessor": predecessor}
        for index, predecessor in enumerate(dump.predecessors)
        if predecessor != -1 and not 0 <= predecessor < index
    ]
    return {
        "candidate_set_and_score_then_position_order_valid": observed == expected,
        "candidate_ranks_consecutive": ranks == list(range(len(ranks))),
        "predecessor_domain_valid": not predecessor_errors,
        "predecessor_domain_errors": predecessor_errors[:100],
        "predecessor_domain_error_count": len(predecessor_errors),
    }


def predecessor_path(dump: Dump) -> list[int]:
    """Follow the highest-score, highest-position candidate to -1."""

    if not dump.candidates:
        return []
    current = dump.candidates[-1][2]
    path: list[int] = []
    while current >= 0:
        if current >= dump.anchor_count or len(path) >= dump.anchor_count:
            raise ValueError("ranked predecessor path leaves the anchor array or cycles")
        path.append(current)
        next_index = dump.predecessors[current]
        if next_index < -1 or next_index >= current:
            raise ValueError(
                f"invalid predecessor p[{current}]={next_index} on ranked path"
            )
        current = next_index
    return path


def reconstruct_run_l(dump: Dump, lanes: int) -> dict[str, Any]:
    """Evaluate the exact BioSeqDB first-path Run_L branch."""

    n = dump.anchor_count
    current = dump.candidates[-1][2] if dump.candidates else -1
    path_length = 0
    loop_evaluations = 0
    bounds_pass = 0
    bounds_fail_current = 0
    bounds_fail_capacity = 0
    bounds_fail_both = 0
    contiguous_mismatch = 0
    fast_batches = 0
    scalar_fallbacks = 0
    decisions: list[dict[str, Any]] = []
    while current >= 0 and path_length < n:
        position_ok = current >= lanes - 1
        capacity_ok = path_length <= n - lanes
        contiguous = False
        if position_ok and capacity_ok:
            bounds_pass += 1
            contiguous = all(
                dump.predecessors[current - offset] == current - offset - 1
                for offset in range(lanes)
            )
        elif not position_ok and not capacity_ok:
            bounds_fail_both += 1
        elif not position_ok:
            bounds_fail_current += 1
        else:
            bounds_fail_capacity += 1
        predicate = position_ok and capacity_ok and contiguous
        decisions.append(
            {
                "current_index": current,
                "path_length_before": path_length,
                "position_bound": position_ok,
                "capacity_bound": capacity_ok,
                "contiguous_predecessors": contiguous,
                "run_l": predicate,
            }
        )
        loop_evaluations += 1
        if predicate:
            fast_batches += 1
            current -= lanes
            path_length += lanes
        else:
            if position_ok and capacity_ok:
                contiguous_mismatch += 1
            scalar_fallbacks += 1
            current = dump.predecessors[current]
            path_length += 1
    scalar_path_length = len(predecessor_path(dump))
    return {
        "schema_version": 1,
        "call_id": dump.call_id,
        "stage": dump.stage,
        "lanes": lanes,
        "anchor_count": n,
        "candidate_end_count": dump.candidate_end_count,
        "start_index": dump.candidates[-1][2] if dump.candidates else -1,
        "start_score": dump.candidates[-1][1] if dump.candidates else -1,
        "loop_evaluations": loop_evaluations,
        "bounds_pass": bounds_pass,
        "bounds_fail_current": bounds_fail_current,
        "bounds_fail_capacity": bounds_fail_capacity,
        "bounds_fail_both": bounds_fail_both,
        "contiguous_mismatch": contiguous_mismatch,
        "fast_batches": fast_batches,
        "scalar_fallbacks": scalar_fallbacks,
        "primary_path_anchors": path_length,
        "fast_covered_anchors": lanes * fast_batches,
        "scalar_path_length": scalar_path_length,
        "scalar_validation_error": 0,
        "emulation_error": 0,
        "min_score": dump.min_score,
        "has_ranked_end": int(bool(dump.candidates)),
        "has_fast_batch": int(fast_batches > 0),
        "decisions": decisions,
    }


def parse_runl_telemetry(stderr_path: Path) -> tuple[list[dict[str, Any]], list[str]]:
    """Read machine telemetry without retaining the potentially large seed log."""

    records: list[dict[str, Any]] = []
    errors: list[str] = []
    with stderr_path.open("r", encoding="utf-8", errors="replace") as source:
        for line_number, line in enumerate(source, start=1):
            if line.startswith(RUNL_PREFIX):
                try:
                    value = json.loads(line[len(RUNL_PREFIX) :])
                except json.JSONDecodeError as error:
                    raise ValueError(
                        f"{stderr_path}:{line_number}: invalid MM_RUNL JSON"
                    ) from error
                if not isinstance(value, dict):
                    raise ValueError(f"{stderr_path}:{line_number}: MM_RUNL must be an object")
                records.append(value)
            elif line.startswith("MM_RUNL_ERROR\t"):
                errors.append(line.rstrip("\n"))
    return records, errors


def parse_selected_anchors(
    stderr_path: Path, selected_indices: set[int]
) -> tuple[dict[int, Anchor], int]:
    """Join selected predecessor indices to streamed SD records."""

    selected: dict[int, Anchor] = {}
    anchor_index = 0
    with stderr_path.open("r", encoding="utf-8", errors="replace") as source:
        for line_number, line in enumerate(source, start=1):
            if not line.startswith(SD_PREFIX):
                continue
            fields = line.rstrip("\n").split("\t")
            if len(fields) != 7:
                raise ValueError(f"{stderr_path}:{line_number}: malformed SD row")
            if anchor_index in selected_indices:
                try:
                    anchor = Anchor(
                        index=anchor_index,
                        reference_name=fields[1],
                        target_end=int(fields[2]),
                        strand=fields[3],
                        query_end=int(fields[4]),
                        span=int(fields[5]),
                        printed_diagonal_delta=int(fields[6]),
                    )
                except ValueError as error:
                    raise ValueError(
                        f"{stderr_path}:{line_number}: noninteger SD coordinate"
                    ) from error
                if anchor.strand not in {"+", "-"} or anchor.span <= 0:
                    raise ValueError(f"{stderr_path}:{line_number}: invalid SD strand/span")
                selected[anchor_index] = anchor
            anchor_index += 1
    missing = sorted(selected_indices - selected.keys())
    if missing:
        raise ValueError(f"seed diagnostics omit selected anchor indices: {missing[:20]}")
    return selected, anchor_index


def compare_telemetry(
    dump: Dump, reconstructions: list[dict[str, Any]], records: list[dict[str, Any]]
) -> dict[str, Any]:
    """Compare independent Run_L counters with minimap2's emitted counters."""

    compare_fields = (
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
    )
    reports: list[dict[str, Any]] = []
    for reconstructed in reconstructions:
        matching = [
            record
            for record in records
            if record.get("call_id") == dump.call_id
            and record.get("stage") == dump.stage
            and record.get("lanes") == reconstructed["lanes"]
        ]
        mismatches: dict[str, Any] = {}
        if len(matching) == 1:
            observed = matching[0]
            mismatches = {
                field: {
                    "reconstructed": reconstructed.get(field),
                    "instrumented": observed.get(field),
                }
                for field in compare_fields
                if reconstructed.get(field) != observed.get(field)
            }
        else:
            observed = None
            mismatches["record_count"] = {"expected": 1, "observed": len(matching)}
        reports.append(
            {
                "lanes": reconstructed["lanes"],
                "matching_telemetry_records": len(matching),
                "all_compared_fields_match": not mismatches,
                "mismatches": mismatches,
                "instrumented_record": observed,
            }
        )
    return {
        "lane_reports": reports,
        "all_compared_fields_match": all(
            item["all_compared_fields_match"] for item in reports
        ),
    }


def anchor_copy_report(
    anchor: Anchor,
    placements: list[dict[str, int]],
    expected_strand: str,
    diagonal_tolerance: int,
    query_length: int,
) -> dict[str, Any]:
    """Classify an anchor by copy interval, strand, and normalized diagonal."""

    target_begin = anchor.target_end - anchor.span + 1
    query_begin = anchor.query_end - anchor.span + 1
    interval_copy_ranks = [
        placement["rank"]
        for placement in placements
        if target_begin >= placement["start"] and anchor.target_end < placement["end"]
    ]
    diagonal_matches = [
        {
            "rank": placement["rank"],
            "start": placement["start"],
            "diagonal_error": anchor.target_end - anchor.query_end - placement["start"],
        }
        for placement in placements
        if abs(anchor.target_end - anchor.query_end - placement["start"])
        <= diagonal_tolerance
    ]
    valid_copy_ranks = sorted(
        set(interval_copy_ranks)
        & {match["rank"] for match in diagonal_matches}
    ) if anchor.strand == expected_strand else []
    return {
        "index": anchor.index,
        "reference_name": anchor.reference_name,
        "target_begin": target_begin,
        "target_end": anchor.target_end,
        "query_begin_normalized": query_begin,
        "query_end_normalized": anchor.query_end,
        "span": anchor.span,
        "strand": anchor.strand,
        "expected_strand": expected_strand,
        "strand_valid": anchor.strand == expected_strand,
        "query_interval_in_bounds": query_begin >= 0 and anchor.query_end < query_length,
        "interval_copy_ranks": interval_copy_ranks,
        "diagonal_matches": diagonal_matches,
        "valid_copy_ranks": valid_copy_ranks,
    }


def validate_path_copies(
    path: list[int],
    anchors: dict[int, Anchor],
    placements: list[dict[str, int]],
    dataset: dict[str, Any],
    extra_tolerance: int,
    expected_reference_name: str,
) -> dict[str, Any]:
    """Require every ranked path anchor to identify the same planted copy."""

    edits = dataset.get("realized_edits")
    if not isinstance(edits, dict):
        raise ValueError("dataset.realized_edits must be an object")
    orientation = edits.get("orientation")
    if orientation == "forward":
        expected_strand = "+"
    elif orientation == "reverse-complement":
        expected_strand = "-"
    else:
        raise ValueError(f"unsupported realized orientation: {orientation!r}")
    inserted = int(edits.get("inserted_bases", 0))
    deleted = int(edits.get("deleted_bases", 0))
    if inserted < 0 or deleted < 0 or extra_tolerance < 0:
        raise ValueError("indel counts and extra diagonal tolerance must be nonnegative")
    tolerance = inserted + deleted + extra_tolerance
    query_length = int(dataset.get("query_length", -1))
    reports = [
        anchor_copy_report(
            anchors[index], placements, expected_strand, tolerance, query_length
        )
        for index in path
    ]
    reference_names_valid = all(
        report["reference_name"] == expected_reference_name for report in reports
    )
    common: set[int] = set(placement["rank"] for placement in placements)
    for report in reports:
        common &= set(report["valid_copy_ranks"])
    path_present = bool(reports)
    unique_anchor_assignments = path_present and all(
        len(report["valid_copy_ranks"]) == 1 for report in reports
    )
    common_rank = next(iter(common)) if len(common) == 1 else None
    return {
        "orientation": orientation,
        "expected_anchor_strand": expected_strand,
        "expected_reference_name": expected_reference_name,
        "all_reference_names_valid": reference_names_valid,
        "diagonal_tolerance_bases": tolerance,
        "diagonal_tolerance_derivation": {
            "inserted_bases": inserted,
            "deleted_bases": deleted,
            "explicit_extra_bases": extra_tolerance,
            "rule": "inserted_bases + deleted_bases + explicit_extra_bases",
        },
        "ranked_path_anchor_count": len(path),
        "ranked_path_present": path_present,
        "each_anchor_has_one_valid_copy": unique_anchor_assignments,
        "common_valid_copy_ranks": sorted(common),
        "single_common_planted_copy": (
            len(common) == 1 and unique_anchor_assignments and reference_names_valid
        ),
        "common_copy_rank": common_rank,
        "all_query_intervals_in_bounds": all(
            report["query_interval_in_bounds"] for report in reports
        ),
        "anchors": reports,
    }


def choose_initial_dp_dump(dump_dir: Path) -> tuple[Dump, list[dict[str, Any]]]:
    """Select the initial DP call whose indices match the SD anchor array."""

    dumps = [parse_dump(path) for path in sorted(dump_dir.glob("runl_call_*.tsv"))]
    inventory = [
        {
            "path": str(dump.path.resolve()),
            "sha256": sha256_file(dump.path),
            "stage": dump.stage,
            "call_id": dump.call_id,
            "anchor_count": dump.anchor_count,
            "candidate_end_count": dump.candidate_end_count,
        }
        for dump in dumps
    ]
    matching = [dump for dump in dumps if dump.stage == "dp" and dump.call_id == 1]
    if len(matching) != 1:
        raise ValueError(
            "expected exactly one initial call_id=1 DP dump whose indices match "
            f"the SD anchor array; found {[(dump.call_id, dump.stage) for dump in dumps]}"
        )
    return matching[0], inventory


def run_minimap2(
    command: list[str], environment: dict[str, str], stderr_path: Path, timeout: float
) -> dict[str, Any]:
    """Run one isolated mapping and terminate its process group on timeout."""

    started_at = utc_now()
    started = time.monotonic()
    timed_out = False
    with stderr_path.open("xb") as stderr_output:
        process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=stderr_output,
            env=environment,
            start_new_session=True,
        )
        try:
            exit_code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                exit_code = process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                exit_code = process.wait()
        stderr_output.flush()
        os.fsync(stderr_output.fileno())
    return {
        "started_at_utc": started_at,
        "completed_at_utc": utc_now(),
        "observed_wall_seconds": time.monotonic() - started,
        "exit_code": exit_code,
        "timed_out": timed_out,
    }


def fixture() -> None:
    """Exercise path, Run_L, and copy checks without invoking minimap2."""

    with tempfile.TemporaryDirectory(prefix="runl_path_fixture_") as directory:
        root = Path(directory)
        dump_path = root / "runl_call_1_dp.tsv"
        rows = ["META\tdp\t1\t8\t5\t40"]
        rows.extend(f"Z\t{rank}\t{score}\t{index}" for rank, (score, index) in enumerate(
            [(40, 3), (50, 4), (60, 5), (70, 6), (80, 7)]
        ))
        rows.extend(
            f"P\t{index}\t{(index + 1) * 10}\t{index - 1}"
            for index in range(8)
        )
        dump_path.write_text("\n".join(rows) + "\n", encoding="utf-8")
        parsed = parse_dump(dump_path)
        path = predecessor_path(parsed)
        anchors = {
            index: Anchor(index, "fixture", 104 + index, "+", 4 + index, 5, 0)
            for index in path
        }
        copy_report = validate_path_copies(
            path,
            anchors,
            [{"rank": 1, "cell_index": 0, "start": 100, "end": 150}],
            {
                "query_length": 50,
                "realized_edits": {
                    "orientation": "forward",
                    "inserted_bases": 0,
                    "deleted_bases": 0,
                },
            },
            0,
            "fixture",
        )
        reconstructions = [reconstruct_run_l(parsed, lanes) for lanes in LANE_WIDTHS]
        stderr_path = root / "minimap2.stderr.log"
        stderr_rows: list[str] = []
        for index in range(8):
            stderr_rows.append(f"SD\tfixture\t{104 + index}\t+\t{4 + index}\t5\t0")
        for reconstructed in reconstructions:
            telemetry = {
                key: value
                for key, value in reconstructed.items()
                if key != "decisions"
            }
            telemetry.update({"call_id": 1, "stage": "dp"})
            stderr_rows.append(RUNL_PREFIX + json.dumps(telemetry, sort_keys=True))
        stderr_path.write_text("\n".join(stderr_rows) + "\n", encoding="utf-8")
        parsed_anchors, sd_count = parse_selected_anchors(stderr_path, set(path))
        fixture_telemetry, fixture_errors = parse_runl_telemetry(stderr_path)
        telemetry_report = compare_telemetry(parsed, reconstructions, fixture_telemetry)
        assert path == list(reversed(range(8)))
        assert dump_invariants(parsed)["predecessor_domain_valid"]
        assert copy_report["single_common_planted_copy"]
        assert parsed_anchors == anchors
        assert sd_count == 8
        assert telemetry_report["all_compared_fields_match"]
        assert not fixture_errors
        assert [item["fast_batches"] for item in reconstructions] == [2, 1, 0]
    print("self-test passed")


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments."""

    parser = argparse.ArgumentParser(
        description=(
            "Rerun and independently validate one manifest-defined minimap2 first path"
        )
    )
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--minimap2", type=Path, default=DEFAULT_MINIMAP2)
    parser.add_argument("--dataset-id")
    parser.add_argument("--replicate-id")
    parser.add_argument("--N", dest="occurrence_count", type=int)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--timeout-seconds", type=float)
    parser.add_argument("--diagonal-tolerance-extra", type=int, default=0)
    parser.add_argument("--self-test", action="store_true")
    return parser.parse_args()


def main() -> int:
    """Run the selected validation and publish validation.json."""

    global FAILURE_REPORT_CONTEXT
    args = parse_args()
    if args.self_test:
        fixture()
        return 0
    if args.occurrence_count is None or args.occurrence_count <= 0:
        raise ValueError("--N must be supplied as a positive integer")
    manifest_path = args.manifest.resolve()
    minimap2 = args.minimap2.resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict):
        raise ValueError("manifest root must be an object")
    dataset = select_dataset(manifest, args.dataset_id, args.replicate_id)
    arm_id = str(dataset.get("arm_id", ""))
    defaults, arm_args, declared_timeout = select_arm_args(manifest, arm_id)
    validate_coordinate_model_args([*defaults, *arm_args])
    references = dataset.get("references")
    if not isinstance(references, dict) or not isinstance(
        references.get(str(args.occurrence_count)), dict
    ):
        raise ValueError(f"selected dataset has no N={args.occurrence_count} reference")
    reference = references[str(args.occurrence_count)]
    query = dataset.get("query")
    source_entry = dataset.get("source")
    placements_entry = dataset.get("placements")
    if (
        not isinstance(query, dict)
        or not isinstance(source_entry, dict)
        or not isinstance(placements_entry, dict)
    ):
        raise ValueError("selected dataset lacks source, query, or placements metadata")
    reference_path = resolve_declared_path(manifest_path, str(reference.get("path")))
    query_path = resolve_declared_path(manifest_path, str(query.get("path")))
    source_path = resolve_declared_path(manifest_path, str(source_entry.get("path")))
    placements_path = resolve_declared_path(
        manifest_path, str(placements_entry.get("path"))
    )
    inputs = {
        "manifest": {
            "path": str(manifest_path),
            "sha256": sha256_file(manifest_path),
            "size_bytes": manifest_path.stat().st_size,
        },
        "minimap2": {
            "path": str(minimap2),
            "sha256": sha256_file(minimap2),
            "size_bytes": minimap2.stat().st_size,
        },
        "reference": require_file_hash(reference_path, reference.get("sha256"), "reference"),
        "query": require_file_hash(query_path, query.get("sha256"), "query"),
        "source": require_file_hash(source_path, source_entry.get("sha256"), "source"),
        "placements": require_file_hash(
            placements_path, placements_entry.get("sha256"), "placements"
        ),
    }
    placements_all = parse_placements(placements_path)
    if len(placements_all) != int(placements_entry.get("count", -1)):
        raise ValueError("placement row count disagrees with the manifest")
    all_position_hash = canonical_json_sha256(
        [placement["start"] for placement in placements_all]
    )
    if all_position_hash != placements_entry.get("positions_sha256"):
        raise ValueError("placement position digest disagrees with the manifest")
    source_length = int(dataset.get("source_length", -1))
    reference_length = int(dataset.get("reference_length", -1))
    if source_length <= 0 or reference_length <= 0:
        raise ValueError("dataset source and reference lengths must be positive")
    if reference_length > 2_147_483_647:
        raise ValueError("reference length exceeds the signed 32-bit SD coordinate range")
    if any(
        placement["end"] - placement["start"] != source_length
        or placement["end"] > reference_length
        for placement in placements_all
    ):
        raise ValueError("a placement interval has an invalid size or reference bound")
    ordered_all = sorted(placements_all, key=lambda value: value["start"])
    if any(
        left["end"] > right["start"]
        for left, right in zip(ordered_all, ordered_all[1:])
    ):
        raise ValueError("placement intervals overlap")
    placements = sorted(
        placements_all[: args.occurrence_count], key=lambda value: value["start"]
    )
    if len(placements) != args.occurrence_count:
        raise ValueError("placement table has fewer than N nested-prefix rows")
    intended_positions = [placement["start"] for placement in placements]
    if intended_positions != reference.get("intended_positions"):
        raise ValueError("nested placement prefix disagrees with intended reference positions")
    if canonical_json_sha256(intended_positions) != reference.get(
        "intended_positions_sha256"
    ):
        raise ValueError("intended reference position digest is invalid")
    if (
        reference.get("intended_occurrences") != args.occurrence_count
        or reference.get("verified_exact_source_occurrences_overlapping")
        != args.occurrence_count
        or reference.get("verified_positions_match_intended") is not True
        or reference.get("verified_positions_sha256")
        != reference.get("intended_positions_sha256")
    ):
        raise ValueError("reference occurrence claims are inconsistent with N")
    source_name, source_sequence = read_single_fasta(source_path)
    query_name, query_sequence = read_single_fasta(query_path)
    if len(source_sequence) != source_length or len(query_sequence) != int(
        dataset.get("query_length", -1)
    ):
        raise ValueError("source or query FASTA length disagrees with the manifest")
    reference_audit = audit_reference_fasta(reference_path, source_sequence, placements)
    if reference_audit["reference_length"] != reference_length:
        raise ValueError("reference FASTA length disagrees with the manifest")
    if (
        reference_audit["overlapping_exact_source_positions"] != intended_positions
        or not reference_audit["all_declared_copy_slices_equal_source"]
    ):
        raise ValueError(
            "independent FASTA audit did not recover exactly the declared planted copies"
        )

    output_dir = args.output_dir
    if output_dir is None:
        output_dir = ROOT / (
            "followup_path_validation_"
            + re.sub(r"[^A-Za-z0-9_.-]+", "_", str(dataset["dataset_id"]))
            + f"_N{args.occurrence_count}"
        )
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=False)
    dump_dir = output_dir / "dumps"
    dump_dir.mkdir()
    stderr_path = output_dir / "minimap2.stderr.log"
    command = [
        str(minimap2),
        *defaults,
        *arm_args,
        "-t",
        "1",
        "--dbg-seed-occ",
        "--print-seed",
        str(reference_path),
        str(query_path),
    ]
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    environment["MM_RUNL_EXPERIMENT"] = "1"
    environment["MM_RUNL_DUMP_DIR"] = str(dump_dir)
    timeout = args.timeout_seconds if args.timeout_seconds is not None else declared_timeout
    if timeout <= 0:
        raise ValueError("timeout must be positive")
    process = run_minimap2(command, environment, stderr_path, timeout)
    process["stderr_path"] = str(stderr_path)
    process["stderr_sha256"] = sha256_file(stderr_path)
    if process["timed_out"] or process["exit_code"] != 0:
        failure = {
            "schema_version": 1,
            "status": "mapping_failed",
            "created_at_utc": utc_now(),
            "inputs": inputs,
            "selection": {
                "dataset_id": dataset.get("dataset_id"),
                "replicate_id": dataset.get("replicate_id"),
                "arm_id": arm_id,
                "occurrence_count": args.occurrence_count,
            },
            "command": command,
            "process": process,
        }
        atomic_json(output_dir / "validation.json", failure)
        return 1

    FAILURE_REPORT_CONTEXT = {
        "output_path": output_dir / "validation.json",
        "report": {
            "schema_version": 1,
            "status": "validation_error",
            "created_at_utc": utc_now(),
            "inputs": inputs,
            "selection": {
                "dataset_id": dataset.get("dataset_id"),
                "replicate_id": dataset.get("replicate_id"),
                "arm_id": arm_id,
                "occurrence_count": args.occurrence_count,
                "required_stage": "dp",
                "required_call_id": 1,
            },
            "command": command,
            "process": process,
        },
    }

    dump, inventory = choose_initial_dp_dump(dump_dir)
    invariants = dump_invariants(dump)
    path = predecessor_path(dump)
    selected_anchors, sd_count = parse_selected_anchors(stderr_path, set(path))
    reconstructions = [reconstruct_run_l(dump, lanes) for lanes in LANE_WIDTHS]
    telemetry_records, instrumentation_errors = parse_runl_telemetry(stderr_path)
    telemetry = compare_telemetry(dump, reconstructions, telemetry_records)
    copy_report = validate_path_copies(
        path,
        selected_anchors,
        placements,
        dataset,
        args.diagonal_tolerance_extra,
        reference_audit["reference_name"],
    )
    seed_array_count_matches_dump = sd_count == dump.anchor_count
    all_checks = (
        invariants["candidate_set_and_score_then_position_order_valid"]
        and invariants["candidate_ranks_consecutive"]
        and invariants["predecessor_domain_valid"]
        and seed_array_count_matches_dump
        and telemetry["all_compared_fields_match"]
        and not instrumentation_errors
        and copy_report["single_common_planted_copy"]
        and copy_report["all_query_intervals_in_bounds"]
    )
    report = {
        "schema_version": 1,
        "status": "passed" if all_checks else "failed_validation",
        "created_at_utc": utc_now(),
        "validator": {
            "script": str(Path(__file__).resolve()),
            "script_sha256": sha256_file(Path(__file__).resolve()),
            "description": (
                "independent reconstruction from minimap2 f/p/z dumps joined to "
                "pre-chaining SD anchor coordinates"
            ),
        },
        "selection": {
            "dataset_id": dataset.get("dataset_id"),
            "replicate_id": dataset.get("replicate_id"),
            "arm_id": arm_id,
            "occurrence_count": args.occurrence_count,
            "stage": dump.stage,
            "call_id": dump.call_id,
        },
        "inputs": inputs,
        "independent_fasta_audit": {
            "source_name": source_name,
            "source_length": len(source_sequence),
            "query_name": query_name,
            "query_length": len(query_sequence),
            **reference_audit,
        },
        "command": command,
        "process": process,
        "dump_inventory": inventory,
        "selected_dump": {
            "path": str(dump.path.resolve()),
            "sha256": sha256_file(dump.path),
            "anchor_count": dump.anchor_count,
            "candidate_end_count": dump.candidate_end_count,
            "min_score": dump.min_score,
        },
        "dump_invariants": invariants,
        "ranked_first_path": {
            "start_rule": "maximum score, then maximum anchor-array position on ties",
            "indices_in_predecessor_order": path,
            "anchor_count": len(path),
        },
        "seed_coordinate_join": {
            "sd_record_count": sd_count,
            "dump_anchor_count": dump.anchor_count,
            "counts_match": seed_array_count_matches_dump,
            "array_order_basis": (
                "SD records are printed from a[] immediately before a[] is passed "
                "to the chaining implementation that emits the indexed P rows"
            ),
        },
        "run_l_definition": (
            "j >= L-1 and c <= n-L and all(p[j-u] == j-u-1 for u in [0,L))"
        ),
        "run_l_reconstruction": reconstructions,
        "instrumentation_comparison": telemetry,
        "instrumentation_errors": instrumentation_errors,
        "planted_copy_validation": copy_report,
        "all_checks_pass": all_checks,
        "limitations": [
            (
                "The validation covers the ranked first predecessor path of the "
                "initial call_id=1 DP chaining call. It does not classify later "
                "chains or the long-join RMQ call. RMQ receives a compacted and "
                "re-sorted anchor array whose indices cannot be joined to the "
                "pre-DP SD records."
            ),
            (
                "The validator independently scans the reference for every overlapping "
                "exact source occurrence and checks every declared copy slice. Path "
                "identity then uses those verified intervals, the expected strand, and "
                "a conservative diagonal tolerance equal to all realized inserted and "
                "deleted bases plus any explicit extra tolerance."
            ),
            (
                "The experiment mode sorts equal-score candidate ends by increasing "
                "anchor index before minimap2 backtracks them in reverse order. This "
                "implements the BioSeqDB tie rule but can change tied-chain extraction "
                "relative to an uninstrumented minimap2 run. The script does not "
                "independently recompute minimizers or chaining scores."
            ),
        ],
    }
    atomic_json(output_dir / "validation.json", report)
    FAILURE_REPORT_CONTEXT = None
    print(
        json.dumps(
            {
                "status": report["status"],
                "output": str(output_dir / "validation.json"),
                "path_anchor_count": len(path),
                "common_copy_rank": copy_report["common_copy_rank"],
                "run_l_fast_batches": {
                    str(item["lanes"]): item["fast_batches"]
                    for item in reconstructions
                },
            },
            sort_keys=True,
        )
    )
    return 0 if all_checks else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        if FAILURE_REPORT_CONTEXT is not None:
            failure_report = dict(FAILURE_REPORT_CONTEXT["report"])
            failure_report["error_type"] = type(error).__name__
            failure_report["error"] = str(error)
            try:
                atomic_json(FAILURE_REPORT_CONTEXT["output_path"], failure_report)
            except OSError as write_error:
                print(
                    f"unable to write machine-readable failure report: {write_error}",
                    file=sys.stderr,
                )
        print(f"validation error: {error}", file=sys.stderr)
        raise SystemExit(2) from error
