#!/usr/bin/env python3
"""Run a manifest-defined repeat-disruption Run_L experiment.

The runner is independent of the FASTA construction.  It verifies every input
digest, captures minimap2's complete PAF, stderr, seed-coordinate stream, and
f/p/z dumps, then reconstructs the first predecessor path.  One atomic result
shard is published per case, which makes interrupted sweeps safely resumable.
"""

from __future__ import annotations

import argparse
from concurrent.futures import FIRST_COMPLETED, Future, ThreadPoolExecutor, wait
from dataclasses import asdict, dataclass
import hashlib
import json
import fcntl
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import uuid
from typing import Any, Iterable

import yaml

from runl_repeat_common import (
    CaseSpec,
    DEFAULT_LANE_WIDTHS,
    ROOT,
    atomic_write_bytes,
    atomic_write_json,
    canonical_json_sha256,
    load_manifest,
    sha256_file,
    utc_timestamp,
)


DEFAULT_MANIFEST = ROOT / "repeat_disruption_data_pilot" / "manifest.json"
DEFAULT_MINIMAP2 = ROOT / "minimap2"
DEFAULT_RAW_DIR = ROOT / "repeat_disruption_raw"
DEFAULT_SHARD_DIR = ROOT / "repeat_disruption_shards"
DEFAULT_RESULTS = ROOT / "repeat_disruption_results.yaml"
DEFAULT_PROGRESS_LOG = ROOT / "repeat_disruption_progress.jsonl"
RUNL_PREFIX = "MM_RUNL\t"
MAP_PREFIX = "MM_RUNL_MAP\t"
ERROR_PREFIX = "MM_RUNL_ERROR\t"
SD_PREFIX = "SD\t"
SF_PREFIX = "SF\t"
MIN_FREE_MEMORY_MIB = 2048
MEMORY_MIB_PER_JOB = 1024
MAX_AUTOMATIC_JOBS = 8
PROCESS_POLL_SECONDS = 0.2


@dataclass(frozen=True)
class SeedAnchor:
    """One anchor printed by minimap2 in the pre-chaining array order."""

    index: int
    target_name: str
    target_end: int
    strand: str
    query_end: int
    span: int
    printed_diagonal_delta: int


@dataclass(frozen=True)
class PafRecord:
    """The twelve mandatory fields of one PAF alignment record."""

    query_name: str
    query_length: int
    query_start: int
    query_end: int
    strand: str
    target_name: str
    target_length: int
    target_start: int
    target_end: int
    residue_matches: int
    alignment_block_length: int
    mapping_quality: int


@dataclass(frozen=True)
class ChainDump:
    """One complete experiment-only chaining dump."""

    path: Path
    stage: str
    call_id: int
    anchor_count: int
    candidate_end_count: int
    min_score: int
    candidates: tuple[tuple[int, int, int], ...]
    scores: tuple[int, ...]
    predecessors: tuple[int, ...]


@dataclass(frozen=True)
class ProcessOutcome:
    """Observed process status and sampled resource use."""

    exit_code: int | None
    timed_out: bool
    interrupted: bool
    wall_seconds: float
    peak_rss_kib: int | None
    samples: int


class ActiveProcesses:
    """Track child process groups for bounded interruption cleanup."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._groups: set[int] = set()

    def add(self, process_group: int) -> None:
        with self._lock:
            self._groups.add(process_group)

    def discard(self, process_group: int) -> None:
        with self._lock:
            self._groups.discard(process_group)

    def terminate_all(self) -> None:
        with self._lock:
            groups = tuple(self._groups)
        for process_group in groups:
            try:
                os.killpg(process_group, signal.SIGTERM)
            except ProcessLookupError:
                pass


ACTIVE_PROCESSES = ActiveProcesses()
STOP_REQUESTED = threading.Event()
RUNNER_LOCK_DESCRIPTOR: int | None = None


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--minimap2", type=Path, default=DEFAULT_MINIMAP2)
    parser.add_argument("--raw-dir", type=Path, default=DEFAULT_RAW_DIR)
    parser.add_argument("--shard-dir", type=Path, default=DEFAULT_SHARD_DIR)
    parser.add_argument("--results", type=Path, default=DEFAULT_RESULTS)
    parser.add_argument("--progress-log", type=Path, default=DEFAULT_PROGRESS_LOG)
    parser.add_argument(
        "--jobs",
        default="auto",
        help="parallel jobs, or 'auto' for a memory-aware choice",
    )
    parser.add_argument(
        "--only-case",
        action="append",
        default=[],
        help="case id, comma-separated ids, or repeatable option",
    )
    parser.add_argument(
        "--only-group",
        action="append",
        default=[],
        help="case group, comma-separated groups, or repeatable option",
    )
    parser.add_argument(
        "--rerun-failed",
        action="store_true",
        help="rerun cases whose existing shard is not accepted",
    )
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def expanded_filter(values: list[str]) -> set[str] | None:
    """Expand repeatable comma-separated filters."""

    expanded = {
        item.strip()
        for value in values
        for item in value.split(",")
        if item.strip()
    }
    return expanded or None


def require_under_experiment_root(path: Path, label: str) -> Path:
    """Require every experiment path to remain inside the disposable source tree."""

    resolved = path.resolve()
    try:
        resolved.relative_to(ROOT.resolve())
    except ValueError as error:
        raise ValueError(
            f"{label} must resolve below {ROOT.resolve()}, observed {resolved}"
        ) from error
    return resolved


def path_contains(parent: Path, child: Path) -> bool:
    """Return true when ``child`` is equal to or below ``parent``."""

    try:
        child.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def validate_output_namespace(
    manifest_path: Path,
    binary: Path,
    cases: list[CaseSpec],
    raw_root: Path,
    shard_dir: Path,
    results_path: Path,
    progress_log: Path,
) -> None:
    """Prevent output aliases and overlapping experiment namespaces."""

    if raw_root == shard_dir or path_contains(raw_root, shard_dir) or path_contains(
        shard_dir, raw_root
    ):
        raise ValueError("raw and shard directories must be disjoint")
    if results_path == progress_log:
        raise ValueError("results and progress paths must be distinct")
    for label, output in (("results", results_path), ("progress", progress_log)):
        if (
            path_contains(raw_root, output)
            or path_contains(shard_dir, output)
            or path_contains(output, raw_root)
            or path_contains(output, shard_dir)
        ):
            raise ValueError(f"{label} path must be outside raw and shard directories")
        if output.exists() and output.is_dir():
            raise ValueError(f"{label} path must not be a directory: {output}")

    protected_files = {
        manifest_path.resolve(),
        binary.resolve(),
        Path(__file__).resolve(),
        (ROOT / "runl_repeat_common.py").resolve(),
        (ROOT / "lchain.c").resolve(),
    }
    for source in ROOT.glob("runl_repeat*.py"):
        protected_files.add(source.resolve())
    for source in manifest_path.parent.rglob("*"):
        if source.is_file():
            protected_files.add(source.resolve())
    for case in cases:
        protected_files.update(
            {
                Path(case.reference["resolved_path"]).resolve(),
                Path(case.query["resolved_path"]).resolve(),
                Path(case.construction_artifact["resolved_path"]).resolve(),
            }
        )
    for output in (results_path, progress_log):
        if output in protected_files:
            raise ValueError(f"output path aliases a protected input/source: {output}")
    for directory in (raw_root, shard_dir):
        overlapping = [path for path in protected_files if path_contains(directory, path)]
        if overlapping:
            raise ValueError(
                f"output directory {directory} contains protected inputs/sources: "
                f"{overlapping[:3]}"
            )


def case_identity(case: CaseSpec) -> dict[str, Any]:
    """Return the manifest-controlled case declaration without resolved paths."""

    return {
        "order": case.order,
        "case_id": case.case_id,
        "replicate_id": case.replicate_id,
        "seed": case.seed,
        "group": case.group,
        "parameters": case.parameters,
        "reference": {
            key: value for key, value in case.reference.items() if key != "resolved_path"
        },
        "query": {
            key: value for key, value in case.query.items() if key != "resolved_path"
        },
        "construction_artifact": {
            key: value
            for key, value in case.construction_artifact.items()
            if key != "resolved_path"
        },
        "expectation": case.expectation,
    }


def analysis_source_digests() -> dict[str, str]:
    """Bind persisted validation results to the exact runner and common helper."""

    return {
        Path(__file__).name: sha256_file(Path(__file__).resolve()),
        "runl_repeat_common.py": sha256_file(ROOT / "runl_repeat_common.py"),
    }


def read_meminfo() -> dict[str, int]:
    """Read Linux memory counters as KiB, returning an empty mapping elsewhere."""

    values: dict[str, int] = {}
    try:
        with Path("/proc/meminfo").open("r", encoding="ascii") as source:
            for line in source:
                match = re.match(r"^([A-Za-z_()]+):\s+(\d+)\s+kB$", line.rstrip())
                if match:
                    values[match.group(1)] = int(match.group(2))
    except OSError:
        return {}
    return values


def choose_jobs(requested: str) -> tuple[int, dict[str, Any]]:
    """Choose bounded parallelism from CPU count and currently available memory."""

    cpu_count = os.cpu_count() or 1
    meminfo = read_meminfo()
    available_mib = meminfo.get("MemAvailable", 0) // 1024
    if requested != "auto":
        try:
            jobs = int(requested)
        except ValueError as error:
            raise ValueError("--jobs must be 'auto' or a positive integer") from error
        if jobs < 1:
            raise ValueError("--jobs must be positive")
        if jobs > MAX_AUTOMATIC_JOBS:
            raise ValueError(f"--jobs must not exceed {MAX_AUTOMATIC_JOBS}")
        mode = "explicit"
    else:
        memory_jobs = (
            max(1, (available_mib - MIN_FREE_MEMORY_MIB) // MEMORY_MIB_PER_JOB)
            if available_mib
            else 1
        )
        jobs = max(1, min(cpu_count, memory_jobs, MAX_AUTOMATIC_JOBS))
        mode = "automatic"
    swap_total_mib = meminfo.get("SwapTotal", 0) // 1024
    swap_free_mib = meminfo.get("SwapFree", 0) // 1024
    return jobs, {
        "mode": mode,
        "requested": requested,
        "selected_jobs": jobs,
        "cpu_count": cpu_count,
        "mem_available_mib_at_start": available_mib or None,
        "minimum_mem_available_mib_observed": available_mib or None,
        "swap_total_mib": swap_total_mib,
        "swap_used_mib_at_start": max(0, swap_total_mib - swap_free_mib),
        "maximum_swap_used_mib_observed": max(0, swap_total_mib - swap_free_mib),
        "minimum_free_memory_mib": MIN_FREE_MEMORY_MIB,
        "estimated_memory_mib_per_job": MEMORY_MIB_PER_JOB,
        "automatic_job_ceiling": MAX_AUTOMATIC_JOBS,
    }


def sample_host_resources(resource_policy: dict[str, Any]) -> dict[str, int | None]:
    """Sample host memory/swap and update extrema stored in the result policy."""

    meminfo = read_meminfo()
    available_mib = (
        meminfo["MemAvailable"] // 1024 if "MemAvailable" in meminfo else None
    )
    swap_total_mib = meminfo.get("SwapTotal", 0) // 1024
    swap_free_mib = meminfo.get("SwapFree", 0) // 1024
    swap_used_mib = max(0, swap_total_mib - swap_free_mib)
    if available_mib is not None:
        prior = resource_policy.get("minimum_mem_available_mib_observed")
        resource_policy["minimum_mem_available_mib_observed"] = (
            available_mib if prior is None else min(int(prior), available_mib)
        )
    resource_policy["maximum_swap_used_mib_observed"] = max(
        int(resource_policy.get("maximum_swap_used_mib_observed", 0)),
        swap_used_mib,
    )
    return {"available_mib": available_mib, "swap_used_mib": swap_used_mib}


def memory_allows_launch(
    in_flight: int, resource_policy: dict[str, Any]
) -> bool:
    """Reserve one estimated job's memory beyond all currently in-flight jobs."""

    sample = sample_host_resources(resource_policy)
    available_mib = sample["available_mib"]
    if available_mib is None:
        return in_flight == 0
    projected_reserve = MIN_FREE_MEMORY_MIB + (in_flight + 1) * MEMORY_MIB_PER_JOB
    return available_mib >= projected_reserve


def process_rss_kib(pid: int) -> int | None:
    """Read the current resident size of one minimap2 process."""

    try:
        with Path(f"/proc/{pid}/status").open("r", encoding="ascii") as source:
            for line in source:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except (OSError, ValueError, IndexError):
        return None
    return None


def append_progress(path: Path, event: str, **fields: Any) -> None:
    """Append one durable, machine-readable progress event."""

    path.parent.mkdir(parents=True, exist_ok=True)
    record = {"timestamp_utc": utc_timestamp(), "event": event, **fields}
    encoded = (json.dumps(record, sort_keys=True) + "\n").encode("utf-8")
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
    try:
        os.write(descriptor, encoded)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def parse_json_record(line: str, prefix: str, line_number: int) -> dict[str, Any]:
    """Parse one prefixed JSON telemetry record."""

    try:
        value = json.loads(line[len(prefix) :])
    except json.JSONDecodeError as error:
        raise ValueError(f"line {line_number}: invalid {prefix.strip()} JSON: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"line {line_number}: {prefix.strip()} value must be an object")
    return value


def parse_stderr(path: Path) -> dict[str, Any]:
    """Parse experiment records while retaining their complete raw file."""

    maps: list[dict[str, Any]] = []
    runl: list[dict[str, Any]] = []
    anchors: list[SeedAnchor] = []
    seed_frequencies: list[dict[str, int | str]] = []
    instrumentation_errors: list[str] = []
    parse_errors: list[str] = []
    with path.open("r", encoding="utf-8", errors="replace") as source:
        for line_number, raw_line in enumerate(source, start=1):
            line = raw_line.rstrip("\r\n")
            try:
                if line.startswith(MAP_PREFIX):
                    maps.append(parse_json_record(line, MAP_PREFIX, line_number))
                elif line.startswith(RUNL_PREFIX):
                    runl.append(parse_json_record(line, RUNL_PREFIX, line_number))
                elif line.startswith(ERROR_PREFIX):
                    instrumentation_errors.append(line[len(ERROR_PREFIX) :])
                elif line.startswith(SD_PREFIX):
                    fields = line.split("\t")
                    if len(fields) != 7:
                        raise ValueError(
                            f"line {line_number}: SD record has {len(fields)} fields"
                        )
                    anchors.append(
                        SeedAnchor(
                            index=len(anchors),
                            target_name=fields[1],
                            target_end=int(fields[2]),
                            strand=fields[3],
                            query_end=int(fields[4]),
                            span=int(fields[5]),
                            printed_diagonal_delta=int(fields[6]),
                        )
                    )
                elif line.startswith(SF_PREFIX):
                    fields = line.split("\t")
                    if len(fields) != 4:
                        raise ValueError(
                            f"line {line_number}: SF record has {len(fields)} fields"
                        )
                    query_position, reference_occurrences, filtered = map(int, fields[1:])
                    if query_position < 0 or reference_occurrences < 1 or filtered not in {0, 1}:
                        raise ValueError(f"line {line_number}: SF record has invalid values")
                    seed_frequencies.append(
                        {
                            "query_position": query_position,
                            "reference_occurrences": reference_occurrences,
                            "filtered": filtered,
                            "collection_pass": (
                                "initial" if not maps else "post_map_rechain"
                            ),
                        }
                    )
            except (ValueError, IndexError) as error:
                parse_errors.append(str(error))
    return {
        "map_records": maps,
        "runl_records": runl,
        "anchors": anchors,
        "seed_frequencies": seed_frequencies,
        "instrumentation_errors": instrumentation_errors,
        "parse_errors": parse_errors,
    }


def parse_paf(path: Path) -> tuple[list[PafRecord], list[str], int]:
    """Parse the mandatory PAF columns and report malformed nonempty lines."""

    records: list[PafRecord] = []
    errors: list[str] = []
    nonempty_lines = 0
    with path.open("r", encoding="utf-8", errors="replace") as source:
        for line_number, line in enumerate(source, start=1):
            if not line.strip():
                continue
            nonempty_lines += 1
            fields = line.rstrip("\n").split("\t")
            if len(fields) < 12:
                errors.append(f"PAF line {line_number} has fewer than 12 fields")
                continue
            try:
                record = PafRecord(
                    query_name=fields[0],
                    query_length=int(fields[1]),
                    query_start=int(fields[2]),
                    query_end=int(fields[3]),
                    strand=fields[4],
                    target_name=fields[5],
                    target_length=int(fields[6]),
                    target_start=int(fields[7]),
                    target_end=int(fields[8]),
                    residue_matches=int(fields[9]),
                    alignment_block_length=int(fields[10]),
                    mapping_quality=int(fields[11]),
                )
                if not record.query_name or not record.target_name:
                    raise ValueError("empty query or target name")
                if record.strand not in {"+", "-"}:
                    raise ValueError("strand is not '+' or '-'")
                if not (
                    record.query_length > 0
                    and 0 <= record.query_start <= record.query_end <= record.query_length
                    and record.target_length > 0
                    and 0 <= record.target_start <= record.target_end <= record.target_length
                    and 0 <= record.residue_matches <= record.alignment_block_length
                    and record.alignment_block_length >= 0
                    and 0 <= record.mapping_quality <= 255
                ):
                    raise ValueError("mandatory numeric fields are out of range")
                records.append(record)
            except ValueError as error:
                errors.append(f"PAF line {line_number} is invalid: {error}")
    return records, errors, nonempty_lines


def validate_paf_expectation(
    expectation: dict[str, Any],
    records: list[PafRecord],
    expected_query_length: int,
    expected_target_length: int,
) -> tuple[dict[str, Any], list[str]]:
    """Check a generic nonempty or declared full-diagonal PAF requirement."""

    declared = expectation.get("paf", {})
    if not isinstance(declared, dict):
        return {}, ["expectation.paf must be an object"]
    errors: list[str] = []
    minimum_count = int(declared.get("minimum_record_count", 1))
    if len(records) < minimum_count:
        errors.append(
            f"PAF has {len(records)} valid records, below required {minimum_count}"
        )
    identity_fields = (
        "required_target_name",
        "required_query_name",
        "required_strand",
    )
    coverage_fields = (
        "minimum_query_coverage_fraction",
        "minimum_target_coverage_fraction",
    )
    span_fields = ("minimum_query_span", "minimum_target_span")
    envelope_fields = (
        "maximum_query_start",
        "minimum_query_end",
        "maximum_target_start",
        "minimum_target_end",
    )
    requires_qualified_record = any(
        field in declared
        for field in identity_fields + coverage_fields + span_fields + envelope_fields
    )
    matching: list[PafRecord] = []
    if requires_qualified_record:
        missing = [field for field in identity_fields if field not in declared]
        if any(field in declared for field in coverage_fields):
            missing.extend(field for field in coverage_fields if field not in declared)
        if missing:
            errors.append(f"qualified PAF expectation lacks {missing}")
        else:
            for record in records:
                query_coverage = (
                    (record.query_end - record.query_start) / record.query_length
                    if record.query_length > 0
                    else 0.0
                )
                target_coverage = (
                    (record.target_end - record.target_start) / record.target_length
                    if record.target_length > 0
                    else 0.0
                )
                if (
                    record.target_name == declared["required_target_name"]
                    and record.query_name == declared["required_query_name"]
                    and record.query_length == expected_query_length
                    and record.target_length == expected_target_length
                    and record.strand == declared["required_strand"]
                    and query_coverage
                    >= float(declared.get("minimum_query_coverage_fraction", 0.0))
                    and target_coverage
                    >= float(declared.get("minimum_target_coverage_fraction", 0.0))
                    and record.query_end - record.query_start
                    >= int(declared.get("minimum_query_span", 0))
                    and record.target_end - record.target_start
                    >= int(declared.get("minimum_target_span", 0))
                    and record.query_start
                    <= int(declared.get("maximum_query_start", record.query_start))
                    and record.query_end
                    >= int(declared.get("minimum_query_end", record.query_end))
                    and record.target_start
                    <= int(declared.get("maximum_target_start", record.target_start))
                    and record.target_end
                    >= int(declared.get("minimum_target_end", record.target_end))
                ):
                    matching.append(record)
            if not matching:
                errors.append("PAF lacks a record satisfying the declared identity and span")
    return {
        "valid_record_count": len(records),
        "minimum_required_record_count": minimum_count,
        "qualified_record_requirement_declared": requires_qualified_record,
        "matching_qualified_record_count": len(matching),
        "matching_records": [asdict(record) for record in matching[:10]],
        "expected_query_length": expected_query_length,
        "expected_target_length": expected_target_length,
    }, errors


def parse_dump(path: Path) -> ChainDump:
    """Parse and validate one complete f/p/z chaining dump."""

    stage: str | None = None
    call_id: int | None = None
    anchor_count: int | None = None
    candidate_count: int | None = None
    min_score: int | None = None
    candidates: list[tuple[int, int, int]] = []
    scores: list[int] = []
    predecessors: list[int] = []
    with path.open("r", encoding="ascii") as source:
        for line_number, line in enumerate(source, start=1):
            fields = line.rstrip("\n").split("\t")
            if fields[0] == "META":
                if len(fields) != 6 or stage is not None:
                    raise ValueError(f"{path}:{line_number}: invalid META record")
                stage = fields[1]
                call_id = int(fields[2])
                anchor_count = int(fields[3])
                candidate_count = int(fields[4])
                min_score = int(fields[5])
            elif fields[0] == "Z":
                if len(fields) != 4:
                    raise ValueError(f"{path}:{line_number}: invalid Z record")
                candidates.append((int(fields[1]), int(fields[2]), int(fields[3])))
            elif fields[0] == "P":
                if len(fields) != 4:
                    raise ValueError(f"{path}:{line_number}: invalid P record")
                index, score, predecessor = map(int, fields[1:])
                if index != len(scores):
                    raise ValueError(f"{path}:{line_number}: nonconsecutive P index")
                scores.append(score)
                predecessors.append(predecessor)
            else:
                raise ValueError(f"{path}:{line_number}: unknown record type")
    if None in (stage, call_id, anchor_count, candidate_count, min_score):
        raise ValueError(f"{path}: missing META record")
    filename_match = re.fullmatch(r"runl_call_(\d+)_(dp|rmq)\.tsv", path.name)
    if filename_match is None:
        raise ValueError(f"{path}: unexpected dump filename")
    if stage not in {"dp", "rmq"}:
        raise ValueError(f"{path}: unknown chaining stage {stage!r}")
    if int(filename_match.group(1)) != call_id or filename_match.group(2) != stage:
        raise ValueError(f"{path}: filename identity differs from META")
    if len(scores) != anchor_count or len(predecessors) != anchor_count:
        raise ValueError(f"{path}: P record count differs from META anchor count")
    if len(candidates) != candidate_count:
        raise ValueError(f"{path}: Z record count differs from META candidate count")
    if any(rank != index for index, (rank, _, _) in enumerate(candidates)):
        raise ValueError(f"{path}: nonconsecutive Z ranks")
    for index, predecessor in enumerate(predecessors):
        if predecessor < -1 or predecessor >= index:
            raise ValueError(
                f"{path}: invalid predecessor p[{index}]={predecessor}"
            )
    observed_candidate_pairs: list[tuple[int, int]] = []
    for _, score, anchor_index in candidates:
        if anchor_index < 0 or anchor_index >= anchor_count:
            raise ValueError(f"{path}: candidate anchor index is out of range")
        if score != scores[anchor_index]:
            raise ValueError(
                f"{path}: candidate score {score} differs from f[{anchor_index}]={scores[anchor_index]}"
            )
        if score < min_score:
            raise ValueError(f"{path}: candidate score is below META min_score")
        observed_candidate_pairs.append((score, anchor_index))
    expected_candidate_pairs = sorted(
        (score, index) for index, score in enumerate(scores) if score >= min_score
    )
    if observed_candidate_pairs != expected_candidate_pairs:
        raise ValueError(
            f"{path}: Z is not exactly sorted {{(f[i],i) | f[i]>=min_score}}"
        )
    return ChainDump(
        path=path,
        stage=str(stage),
        call_id=int(call_id),
        anchor_count=int(anchor_count),
        candidate_end_count=int(candidate_count),
        min_score=int(min_score),
        candidates=tuple(candidates),
        scores=tuple(scores),
        predecessors=tuple(predecessors),
    )


def reconstruct_path(start: int, predecessors: tuple[int, ...]) -> tuple[list[int], list[str]]:
    """Follow predecessor indices and report malformed or cyclic states."""

    path: list[int] = []
    errors: list[str] = []
    current = start
    seen: set[int] = set()
    while current >= 0:
        if current >= len(predecessors):
            errors.append(f"path index {current} exceeds anchor count")
            break
        if current in seen:
            errors.append(f"predecessor cycle at index {current}")
            break
        seen.add(current)
        path.append(current)
        predecessor = predecessors[current]
        if predecessor < -1 or predecessor >= current:
            errors.append(
                f"invalid predecessor p[{current}]={predecessor}, expected -1..{current - 1}"
            )
            break
        current = predecessor
    return path, errors


def emulate_runl(
    start: int, predecessors: tuple[int, ...], lanes: int
) -> dict[str, int]:
    """Evaluate the exact thesis Run_L predicate over one predecessor path."""

    n = len(predecessors)
    j = start
    c = 0
    counters = {
        "loop_evaluations": 0,
        "bounds_pass": 0,
        "bounds_fail_current": 0,
        "bounds_fail_capacity": 0,
        "bounds_fail_both": 0,
        "contiguous_mismatch": 0,
        "fast_batches": 0,
        "scalar_fallbacks": 0,
        "primary_path_anchors": 0,
        "fast_covered_anchors": 0,
        "emulation_error": 0,
    }
    while j >= 0 and c < n:
        position_ok = j >= lanes - 1
        capacity_ok = c <= n - lanes
        counters["loop_evaluations"] += 1
        if not position_ok or not capacity_ok:
            if not position_ok and not capacity_ok:
                counters["bounds_fail_both"] += 1
            elif not position_ok:
                counters["bounds_fail_current"] += 1
            else:
                counters["bounds_fail_capacity"] += 1
        else:
            counters["bounds_pass"] += 1
        contiguous = position_ok and capacity_ok and all(
            predecessors[j - offset] == j - offset - 1
            for offset in range(lanes)
        )
        if contiguous:
            counters["fast_batches"] += 1
            counters["fast_covered_anchors"] += lanes
            j -= lanes
            c += lanes
        else:
            if position_ok and capacity_ok:
                counters["contiguous_mismatch"] += 1
            counters["scalar_fallbacks"] += 1
            if j >= n:
                counters["emulation_error"] = 1
                break
            predecessor = predecessors[j]
            c += 1
            if predecessor < -1 or predecessor >= j:
                counters["emulation_error"] = 1
                break
            j = predecessor
    counters["primary_path_anchors"] = c
    return counters


def percent(numerator: int, denominator: int) -> float | None:
    """Return a percentage with an explicit null for a zero denominator."""

    return None if denominator == 0 else 100.0 * numerator / denominator


def percentile(values: list[int], quantile: float) -> float | None:
    """Return a linearly interpolated quantile for a nonempty integer sample."""

    if not values:
        return None
    ordered = sorted(values)
    location = (len(ordered) - 1) * quantile
    lower = math.floor(location)
    upper = math.ceil(location)
    if lower == upper:
        return float(ordered[lower])
    weight = location - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def summarize_index_gaps(path: list[int], predecessors: tuple[int, ...]) -> dict[str, Any]:
    """Describe the index distance of every nonterminal selected predecessor."""

    gaps = [index - predecessors[index] for index in path if predecessors[index] >= 0]
    contiguous = sum(gap == 1 for gap in gaps)
    skipped = sum(max(0, gap - 1) for gap in gaps)
    return {
        "link_count": len(gaps),
        "contiguous_link_count": contiguous,
        "noncontiguous_link_count": len(gaps) - contiguous,
        "contiguous_link_percent": percent(contiguous, len(gaps)),
        "skipped_array_entries_total": skipped,
        "minimum": min(gaps) if gaps else None,
        "median": percentile(gaps, 0.5),
        "p90": percentile(gaps, 0.9),
        "p99": percentile(gaps, 0.99),
        "maximum": max(gaps) if gaps else None,
        "mean": (sum(gaps) / len(gaps)) if gaps else None,
    }


def selected_path_unit_runs(
    path: list[int], predecessors: tuple[int, ...]
) -> dict[str, Any]:
    """Find maximal path runs whose outgoing predecessor links have unit stride."""

    runs: list[dict[str, int]] = []
    current_start: int | None = None
    current_length = 0
    for path_offset, anchor_index in enumerate(path):
        if predecessors[anchor_index] == anchor_index - 1:
            if current_start is None:
                current_start = path_offset
            current_length += 1
        elif current_start is not None:
            runs.append(
                {
                    "path_offset_start": current_start,
                    "path_offset_end": path_offset,
                    "length": current_length,
                }
            )
            current_start = None
            current_length = 0
    if current_start is not None:
        runs.append(
            {
                "path_offset_start": current_start,
                "path_offset_end": len(path),
                "length": current_length,
            }
        )
    lengths = [run["length"] for run in runs]
    return {
        "run_count": len(runs),
        "lengths": lengths,
        "runs": runs,
        "maximum_length": max(lengths) if lengths else 0,
        "total_unit_link_anchors": sum(lengths),
        "runs_sha256": canonical_json_sha256(runs),
    }


def closed_form_runl_from_unit_runs(
    path_length: int, unit_run_lengths: list[int], lanes: int
) -> dict[str, int]:
    """Compute vector work from maximal unit runs without evaluating Run_L windows."""

    fast_batches = sum(length // lanes for length in unit_run_lengths)
    fast_covered = lanes * fast_batches
    return {
        "fast_batches": fast_batches,
        "fast_covered_anchors": fast_covered,
        "scalar_fallbacks": path_length - fast_covered,
        "primary_path_anchors": path_length,
        "loop_evaluations": fast_batches + path_length - fast_covered,
    }


def resolve_primary_path_declaration(
    expectation: dict[str, Any], anchors: list[SeedAnchor], start_index: int
) -> dict[str, Any] | None:
    """Resolve a fixed path declaration or the alternative containing the ranked start."""

    declared = expectation.get("primary_path")
    if isinstance(declared, dict):
        return declared
    alternatives = expectation.get("primary_path_alternatives")
    if not isinstance(alternatives, list) or not (0 <= start_index < len(anchors)):
        return None
    start = anchors[start_index]
    matches: list[dict[str, Any]] = []
    for alternative in alternatives:
        if not isinstance(alternative, dict):
            continue
        target_interval = alternative.get("target_interval")
        query_interval = alternative.get("query_interval")
        if not (
            isinstance(target_interval, list)
            and len(target_interval) == 2
            and isinstance(query_interval, list)
            and len(query_interval) == 2
        ):
            continue
        if (
            start.target_name == alternative.get("target_name")
            and start.strand == alternative.get("strand")
            and start.query_end - start.target_end == int(alternative.get("diagonal", 0))
            and int(target_interval[0]) <= start.target_end < int(target_interval[1])
            and int(query_interval[0]) <= start.query_end < int(query_interval[1])
        ):
            matches.append(alternative)
    return matches[0] if len(matches) == 1 else None


def independent_coordinate_path(
    expectation: dict[str, Any], anchors: list[SeedAnchor], start_index: int
) -> list[int] | None:
    """Derive the declared diagonal path from SD coordinates, without using p[]."""

    declared = resolve_primary_path_declaration(expectation, anchors, start_index)
    if declared is None:
        return None
    target_interval = declared.get("target_interval")
    query_interval = declared.get("query_interval")
    if not (
        isinstance(target_interval, list)
        and len(target_interval) == 2
        and isinstance(query_interval, list)
        and len(query_interval) == 2
    ):
        return None
    selected = [
        anchor.index
        for anchor in anchors
        if anchor.target_name == declared.get("target_name")
        and anchor.strand == declared.get("strand")
        and anchor.query_end - anchor.target_end == int(declared.get("diagonal", 0))
        and int(target_interval[0]) <= anchor.target_end < int(target_interval[1])
        and int(query_interval[0]) <= anchor.query_end < int(query_interval[1])
    ]
    return sorted(selected, reverse=True)


def independent_unit_runs(path: list[int]) -> dict[str, Any]:
    """Derive maximal unit-link runs from an independently declared path."""

    unit_flags: list[bool] = []
    for offset, anchor_index in enumerate(path):
        successor = path[offset + 1] if offset + 1 < len(path) else -1
        unit_flags.append(successor == anchor_index - 1)
    runs: list[int] = []
    current = 0
    for unit in unit_flags:
        if unit:
            current += 1
        elif current:
            runs.append(current)
            current = 0
    if current:
        runs.append(current)
    return {
        "unit_flags_sha256": canonical_json_sha256(unit_flags),
        "lengths": runs,
        "run_count": len(runs),
        "maximum_length": max(runs) if runs else 0,
        "total_unit_link_anchors": sum(runs),
    }


def selected_path_repeat_statistics(
    path: list[int], anchors: list[SeedAnchor], predecessors: tuple[int, ...]
) -> dict[str, Any]:
    """Measure realized same-target anchor multiplicity along the selected path."""

    group_sizes: dict[tuple[str, int, str], int] = {}
    for anchor in anchors:
        key = (anchor.target_name, anchor.target_end, anchor.strand)
        group_sizes[key] = group_sizes.get(key, 0) + 1
    path_multiplicities: list[int] = []
    contingency = {
        "duplicate_and_nonunit_gap": 0,
        "duplicate_and_unit_gap": 0,
        "singleton_and_nonunit_gap": 0,
        "singleton_and_unit_gap": 0,
    }
    histogram: dict[str, int] = {}
    for anchor_index in path:
        anchor = anchors[anchor_index]
        multiplicity = group_sizes[(anchor.target_name, anchor.target_end, anchor.strand)]
        path_multiplicities.append(multiplicity)
        histogram[str(multiplicity)] = histogram.get(str(multiplicity), 0) + 1
        duplicate = multiplicity > 1
        nonunit = anchor_index - predecessors[anchor_index] > 1
        if duplicate and nonunit:
            contingency["duplicate_and_nonunit_gap"] += 1
        elif duplicate:
            contingency["duplicate_and_unit_gap"] += 1
        elif nonunit:
            contingency["singleton_and_nonunit_gap"] += 1
        else:
            contingency["singleton_and_unit_gap"] += 1
    duplicate_count = sum(value > 1 for value in path_multiplicities)
    a = contingency["duplicate_and_nonunit_gap"]
    b = contingency["duplicate_and_unit_gap"]
    c = contingency["singleton_and_nonunit_gap"]
    d = contingency["singleton_and_unit_gap"]
    denominator = math.sqrt((a + b) * (c + d) * (a + c) * (b + d))
    phi = (a * d - b * c) / denominator if denominator else None
    return {
        "selected_path_anchor_count": len(path),
        "same_target_group_size_histogram": dict(
            sorted(histogram.items(), key=lambda item: int(item[0]))
        ),
        "same_target_group_sizes_sha256": canonical_json_sha256(path_multiplicities),
        "anchors_in_group_larger_than_one": duplicate_count,
        "fraction_in_group_larger_than_one": (
            duplicate_count / len(path) if path else None
        ),
        "maximum_same_target_group_size": max(path_multiplicities)
        if path_multiplicities
        else None,
        "duplicate_by_nonunit_gap_contingency": contingency,
        "phi_correlation_duplicate_vs_nonunit_gap": phi,
    }


def check_primary_coordinates(
    expectation: dict[str, Any], path: list[int], anchors: list[SeedAnchor]
) -> tuple[dict[str, Any], list[str]]:
    """Check an optional declared full-homolog diagonal against path coordinates."""

    declared = resolve_primary_path_declaration(
        expectation, anchors, path[0] if path else -1
    )
    has_any_declaration = "primary_path" in expectation or "primary_path_alternatives" in expectation
    if declared is None and not has_any_declaration:
        return {"declared": False}, []
    if declared is None:
        return {"declared": True}, ["ranked start matches no unique primary-path alternative"]
    errors: list[str] = []
    required = (
        "target_name",
        "strand",
        "diagonal",
        "target_interval",
        "query_interval",
        "minimum_fraction_on_declared_path",
    )
    missing = [field for field in required if field not in declared]
    if missing:
        return {"declared": True}, [f"primary_path expectation lacks {missing}"]
    target_interval = declared["target_interval"]
    query_interval = declared["query_interval"]
    if (
        not isinstance(target_interval, list)
        or len(target_interval) != 2
        or not isinstance(query_interval, list)
        or len(query_interval) != 2
    ):
        return {"declared": True}, ["primary path intervals must be two-element arrays"]
    joined = [anchors[index] for index in path if 0 <= index < len(anchors)]
    if len(joined) != len(path):
        errors.append("one or more predecessor indices lack an SD anchor record")
    matching = [
        anchor
        for anchor in joined
        if anchor.target_name == declared["target_name"]
        and anchor.strand == declared["strand"]
        and anchor.query_end - anchor.target_end == int(declared["diagonal"])
        and int(target_interval[0]) <= anchor.target_end < int(target_interval[1])
        and int(query_interval[0]) <= anchor.query_end < int(query_interval[1])
    ]
    fraction = len(matching) / len(joined) if joined else 0.0
    required_fraction = float(declared["minimum_fraction_on_declared_path"])
    if fraction < required_fraction:
        errors.append(
            "first path fraction on declared full-homolog diagonal is "
            f"{fraction:.6f}, below {required_fraction:.6f}"
        )
    return {
        "declared": True,
        "path_anchor_count": len(joined),
        "matching_anchor_count": len(matching),
        "matching_fraction": fraction,
        "required_fraction": required_fraction,
        "start_anchor": asdict(joined[0]) if joined else None,
        "terminal_anchor": asdict(joined[-1]) if joined else None,
    }, errors


def segment_name(segments: list[dict[str, Any]], coordinate: int) -> str:
    """Return the declared segment containing one end coordinate."""

    for segment in segments:
        if int(segment["start"]) <= coordinate < int(segment["end"]):
            return str(segment["name"])
    return "outside"


def read_fasta_sequence(path: Path) -> bytes:
    """Read the single validated FASTA sequence for construction-level checks."""

    sequence = bytearray()
    seen_header = False
    with path.open("rb") as source:
        for line in source:
            if line.startswith(b">"):
                if seen_header:
                    raise ValueError(f"multiple FASTA records in {path}")
                seen_header = True
            else:
                if not seen_header:
                    raise ValueError(f"sequence precedes FASTA header in {path}")
                sequence.extend(b"".join(line.split()).upper())
    if not seen_header or not sequence:
        raise ValueError(f"empty FASTA record in {path}")
    return bytes(sequence)


def manifest_artifact_root(case: CaseSpec) -> Path:
    """Recover the manifest directory from one declared relative artifact path."""

    root = Path(case.reference["resolved_path"]).resolve()
    for _part in Path(case.reference["path"]).parts:
        root = root.parent
    return root


def analyze_construction(
    case: CaseSpec,
    anchors: list[SeedAnchor],
    path: list[int],
    initial_dump: ChainDump,
) -> tuple[dict[str, Any], list[str]]:
    """Validate construction geometry and summarize anchor groups independently."""

    artifact_path = Path(case.construction_artifact["resolved_path"])
    with artifact_path.open("r", encoding="utf-8") as source:
        construction = json.load(source)
    construction_type = construction.get("construction_type") or construction.get(
        "construction"
    )
    errors: list[str] = []
    summary: dict[str, Any] = {"construction_type": construction_type}

    if construction_type == "mosaic_exact_full_diagonal":
        segments = construction.get("segments")
        if not isinstance(segments, list) or len(segments) != 5:
            errors.append("mosaic construction must declare five segments")
            return summary, errors
        expected_names = ["U0", "A", "U1", "A_prime", "U2"]
        if [segment.get("name") for segment in segments] != expected_names:
            errors.append("mosaic segment names or order differ from U0,A,U1,A_prime,U2")
        cursor = 0
        for segment in segments:
            if int(segment.get("start", -1)) != cursor:
                errors.append("mosaic segments are not contiguous")
                break
            if int(segment.get("end", -1)) <= cursor:
                errors.append("mosaic segment has nonpositive length")
                break
            cursor = int(segment["end"])
        if cursor != int(construction.get("total_length", -1)):
            errors.append("mosaic segment total differs from construction total_length")
        if cursor != int(case.reference["length"]) or cursor != int(case.query["length"]):
            errors.append("mosaic segment total differs from FASTA lengths")
        offset = int(construction.get("a_to_a_prime_offset", -1))
        a_start = int(segments[1]["start"])
        a_prime_start = int(segments[3]["start"])
        if offset != a_prime_start - a_start or offset <= 20_000:
            errors.append("mosaic repeat offset is not the declared >20000 separation")
        reference_sequence = read_fasta_sequence(Path(case.reference["resolved_path"]))
        query_sequence = read_fasta_sequence(Path(case.query["resolved_path"]))
        if reference_sequence != query_sequence:
            errors.append("mosaic reference and query sequences are not byte-identical")
        component_hashes = construction.get("component_sequence_sha256")
        component_sequences: dict[str, bytes] = {}
        if not isinstance(component_hashes, dict):
            errors.append("mosaic construction lacks component sequence hashes")
        else:
            for segment in segments:
                name = str(segment["name"])
                component = reference_sequence[
                    int(segment["start"]) : int(segment["end"])
                ]
                component_sequences[name] = component
                observed_digest = hashlib.sha256(component).hexdigest()
                if component_hashes.get(name) != observed_digest:
                    errors.append(f"mosaic component hash mismatch for {name}")

        mutation_declaration = construction.get("mutation_plan")
        if not isinstance(mutation_declaration, dict):
            errors.append("mosaic construction lacks mutation-plan declaration")
        elif "A" in component_sequences and "A_prime" in component_sequences:
            data_root = manifest_artifact_root(case)
            declared_plan_path = Path(str(mutation_declaration.get("path", "")))
            plan_path = (data_root / declared_plan_path).resolve()
            try:
                plan_path.relative_to(data_root.resolve())
            except ValueError:
                errors.append("mosaic mutation-plan path escapes the manifest directory")
                plan_path = data_root / "__invalid_plan_path__"
            if not plan_path.is_file():
                errors.append("mosaic mutation-plan artifact is missing")
            elif sha256_file(plan_path) != mutation_declaration.get("file_sha256"):
                errors.append("mosaic mutation-plan file digest mismatch")
            else:
                with plan_path.open("r", encoding="utf-8") as source:
                    mutation_plan = json.load(source)
                positions = mutation_plan.get("ordered_positions")
                replacement_text = mutation_plan.get("replacement_bases_ascii")
                if not isinstance(positions, list) or not isinstance(
                    replacement_text, str
                ):
                    errors.append("mosaic mutation plan lacks positions or replacements")
                else:
                    replacement_bases = replacement_text.encode("ascii")
                    a_sequence = component_sequences["A"]
                    if len(positions) != len(replacement_bases):
                        errors.append("mosaic mutation positions/replacements differ in length")
                    if len(set(int(position) for position in positions)) != len(positions):
                        errors.append("mosaic mutation positions are not unique")
                    if any(
                        int(position) < 0 or int(position) >= len(a_sequence)
                        for position in positions
                    ):
                        errors.append("mosaic mutation position is outside A")
                    if canonical_json_sha256(positions) != mutation_plan.get(
                        "ordered_positions_sha256"
                    ):
                        errors.append("mosaic mutation position digest mismatch")
                    if hashlib.sha256(replacement_bases).hexdigest() != mutation_plan.get(
                        "replacement_bases_sha256"
                    ):
                        errors.append("mosaic mutation replacement digest mismatch")
                    if mutation_declaration.get(
                        "ordered_positions_sha256"
                    ) != mutation_plan.get("ordered_positions_sha256"):
                        errors.append("outer and nested mutation position digests differ")
                    level = str(construction.get("level"))
                    applied_count = mutation_declaration.get("applied_prefix_count")
                    declared_count = construction.get(
                        "declared_substitution_count_between_A_and_A_prime"
                    )
                    if level == "independent_control":
                        if applied_count is not None or declared_count is not None:
                            errors.append("independent mosaic control declares a mutation prefix")
                    else:
                        plan_levels = mutation_plan.get("levels")
                        if not isinstance(plan_levels, dict) or level not in plan_levels:
                            errors.append("mosaic level is absent from the nested mutation plan")
                        else:
                            level_plan = plan_levels[level]
                            count = int(level_plan.get("substitution_count", -1))
                            if int(applied_count) != count or int(declared_count) != count:
                                errors.append("mosaic applied substitution count differs from plan")
                            if canonical_json_sha256(positions[:count]) != level_plan.get(
                                "positions_prefix_sha256"
                            ):
                                errors.append("mosaic mutation-position prefix digest mismatch")
                            if hashlib.sha256(replacement_bases[:count]).hexdigest() != level_plan.get(
                                "replacement_bases_prefix_sha256"
                            ):
                                errors.append("mosaic replacement prefix digest mismatch")
                            reconstructed = bytearray(a_sequence)
                            for position, replacement in zip(
                                positions[:count], replacement_bases[:count], strict=True
                            ):
                                position = int(position)
                                if reconstructed[position] == replacement:
                                    errors.append("mosaic replacement equals its original base")
                                    break
                                reconstructed[position] = replacement
                            if bytes(reconstructed) != component_sequences["A_prime"]:
                                errors.append(
                                    "mosaic A_prime differs from the declared nested mutation prefix"
                                )
                            realized_count = sum(
                                left != right
                                for left, right in zip(
                                    a_sequence,
                                    component_sequences["A_prime"],
                                    strict=True,
                                )
                            )
                            if realized_count != count:
                                errors.append("mosaic realized substitution count differs from plan")
        matrix: dict[str, int] = {}
        diagonals: dict[str, int] = {}
        for anchor in anchors:
            key = f"{segment_name(segments, anchor.target_end)}->{segment_name(segments, anchor.query_end)}"
            matrix[key] = matrix.get(key, 0) + 1
            diagonal = str(anchor.query_end - anchor.target_end)
            diagonals[diagonal] = diagonals.get(diagonal, 0) + 1
        expected_tracks = {"0", str(offset), str(-offset)}
        summary.update(
            {
                "segment_anchor_matrix": dict(sorted(matrix.items())),
                "diagonal_anchor_counts": dict(
                    sorted(diagonals.items(), key=lambda item: int(item[0]))
                ),
                "expected_track_anchor_count": sum(
                    count for diagonal, count in diagonals.items() if diagonal in expected_tracks
                ),
                "other_diagonal_anchor_count": sum(
                    count for diagonal, count in diagonals.items() if diagonal not in expected_tracks
                ),
                "a_to_a_prime_offset": offset,
            }
        )
        if diagonals.get("0", 0) == 0:
            errors.append("mosaic has no retained exact-diagonal anchor")

    elif construction_type == "exact_spaced_query_copies":
        copies = int(construction.get("copy_count", 0))
        pitch = int(construction.get("copy_pitch", 0))
        intervals = construction.get("copy_intervals")
        if copies < 1 or not isinstance(intervals, list) or len(intervals) != copies:
            errors.append("spaced-copy construction has inconsistent copy intervals")
            return summary, errors
        if pitch <= 20_000:
            errors.append("spaced-copy pitch does not exceed default long-join bandwidth")
        declared_diagonals = [int(interval["query_minus_target_diagonal"]) for interval in intervals]
        diagonal_counts = {
            diagonal: sum(
                anchor.strand == "+"
                and anchor.query_end - anchor.target_end == diagonal
                for anchor in anchors
            )
            for diagonal in declared_diagonals
        }
        nonzero_counts = [count for count in diagonal_counts.values() if count > 0]
        if len(nonzero_counts) != copies:
            errors.append("one or more exact spaced-copy diagonal groups has no anchor")
        if nonzero_counts and len(set(nonzero_counts)) != 1:
            errors.append("exact spaced-copy diagonal groups have unequal anchor counts")
        selected_diagonals = {
            anchors[index].query_end - anchors[index].target_end
            for index in path
            if 0 <= index < len(anchors)
        }
        if len(selected_diagonals) != 1 or not selected_diagonals.issubset(
            set(declared_diagonals)
        ):
            errors.append("selected spaced-copy path does not remain in one declared copy")
        coordinate_group_sizes: dict[tuple[int, str, int], int] = {}
        for anchor in anchors:
            key = (anchor.target_end, anchor.strand, anchor.span)
            coordinate_group_sizes[key] = coordinate_group_sizes.get(key, 0) + 1
        histogram: dict[str, int] = {}
        for size in coordinate_group_sizes.values():
            histogram[str(size)] = histogram.get(str(size), 0) + 1
        summary.update(
            {
                "copy_count": copies,
                "copy_pitch": pitch,
                "diagonal_anchor_counts": {
                    str(key): value for key, value in diagonal_counts.items()
                },
                "selected_copy_diagonal": (
                    next(iter(selected_diagonals)) if len(selected_diagonals) == 1 else None
                ),
                "target_coordinate_group_size_histogram": dict(
                    sorted(histogram.items(), key=lambda item: int(item[0]))
                ),
            }
        )

    elif construction_type == "unique_minimizer_island_adversarial_control":
        declared_anchors = construction.get("anchors")
        if not isinstance(declared_anchors, list):
            errors.append("comb control lacks its exact anchor array")
            return summary, errors
        expected_coordinates = [
            (
                int(anchor["reference"]),
                "+",
                int(anchor["query"]),
                int(construction.get("k", 15)),
            )
            for anchor in declared_anchors
        ]
        observed_coordinates = [
            (anchor.target_end, anchor.strand, anchor.query_end, anchor.span)
            for anchor in anchors
        ]
        if observed_coordinates != expected_coordinates:
            errors.append("comb control SD coordinate array differs from its exact declaration")
        expected_scores = [int(anchor["score"]) for anchor in declared_anchors]
        expected_predecessors = [int(anchor["predecessor"]) for anchor in declared_anchors]
        if list(initial_dump.scores) != expected_scores:
            errors.append("comb control f array differs from its exact declaration")
        if list(initial_dump.predecessors) != expected_predecessors:
            errors.append("comb control p array differs from its exact declaration")
        summary.update(
            {
                "control_scope": "unique-minimizer coordinate/island adversarial control",
                "repeat_frequency_evidence": False,
                "declared_anchor_count": len(declared_anchors),
            }
        )
    else:
        errors.append(f"unknown construction type: {construction_type!r}")
    return summary, errors


def analyze_stratified_calls(
    dumps: list[ChainDump], runl_records: list[dict[str, Any]]
) -> tuple[list[dict[str, Any]], list[str]]:
    """Validate and summarize every DP/RMQ call without mixing their stages."""

    errors: list[str] = []
    summaries: list[dict[str, Any]] = []
    record_groups: dict[tuple[str, int], list[dict[str, Any]]] = {}
    for record in runl_records:
        stage = str(record.get("stage", ""))
        call_id = int(record.get("call_id", -1))
        record_groups.setdefault((stage, call_id), []).append(record)
    dump_keys = {(dump.stage, dump.call_id) for dump in dumps}
    if set(record_groups) != dump_keys:
        errors.append("MM_RUNL call identities differ from emitted dump identities")
    for dump in sorted(dumps, key=lambda value: value.call_id):
        records = record_groups.get((dump.stage, dump.call_id), [])
        by_lane = {int(record.get("lanes", -1)): record for record in records}
        if set(by_lane) != set(DEFAULT_LANE_WIDTHS) or len(by_lane) != len(records):
            errors.append(
                f"{dump.stage} call {dump.call_id} lacks exactly one record per lane width"
            )
        top_score = dump.candidates[-1][1] if dump.candidates else None
        top_index = dump.candidates[-1][2] if dump.candidates else -1
        path, path_errors = reconstruct_path(top_index, dump.predecessors)
        errors.extend(
            f"{dump.stage} call {dump.call_id}: {error}" for error in path_errors
        )
        per_lane: dict[str, Any] = {}
        for lanes in DEFAULT_LANE_WIDTHS:
            emulated = emulate_runl(top_index, dump.predecessors, lanes)
            record = by_lane.get(lanes)
            identity_errors: list[str] = []
            if record is not None:
                expected_values = {
                    "schema_version": 1,
                    "call_id": dump.call_id,
                    "lanes": lanes,
                    "anchor_count": dump.anchor_count,
                    "candidate_end_count": dump.candidate_end_count,
                    "start_index": top_index,
                    "start_score": top_score if top_score is not None else -1,
                    "min_score": dump.min_score,
                    "has_ranked_end": int(bool(dump.candidates)),
                    "primary_path_anchors": len(path),
                    "scalar_path_length": len(path),
                    "scalar_validation_error": 0,
                    "emulation_error": 0,
                    "has_fast_batch": int(emulated["fast_batches"] > 0),
                }
                for field in (
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
                ):
                    expected_values[field] = emulated[field]
                for field, expected in expected_values.items():
                    if int(record.get(field, -2)) != int(expected):
                        identity_errors.append(field)
                if record.get("stage") != dump.stage:
                    identity_errors.append("stage")
            if identity_errors:
                errors.append(
                    f"{dump.stage} call {dump.call_id} L={lanes} identity fields differ: "
                    f"{sorted(set(identity_errors))}"
                )
            per_lane[str(lanes)] = {
                "fast_batches": emulated["fast_batches"],
                "scalar_fallbacks": emulated["scalar_fallbacks"],
                "primary_path_anchors": emulated["primary_path_anchors"],
                "fast_covered_anchors": emulated["fast_covered_anchors"],
                "runl_true_percent_when_bounds_allow_vector": percent(
                    emulated["fast_batches"], emulated["bounds_pass"]
                ),
                "identity_errors": identity_errors,
            }
        summaries.append(
            {
                "stage": dump.stage,
                "call_id": dump.call_id,
                "anchor_count": dump.anchor_count,
                "candidate_end_count": dump.candidate_end_count,
                "top_score": top_score,
                "top_index": top_index,
                "path_anchor_count": len(path),
                "predecessor_index_gaps": summarize_index_gaps(
                    path, dump.predecessors
                ),
                "by_lane": per_lane,
            }
        )
    return summaries, errors


def validate_case_artifacts(
    case: CaseSpec,
    raw_dir: Path,
    process: ProcessOutcome,
) -> dict[str, Any]:
    """Validate raw output and produce the analysis stored in a result shard."""

    stdout_path = raw_dir / "stdout.paf"
    stderr_path = raw_dir / "stderr.log"
    dump_dir = raw_dir / "dumps"
    errors: list[str] = []
    warnings: list[str] = []
    if process.exit_code != 0:
        errors.append(f"minimap2 exit code is {process.exit_code}")
    if process.timed_out:
        errors.append("minimap2 timed out")
    if process.interrupted:
        errors.append("minimap2 was interrupted")

    paf_records, paf_parse_errors, paf_lines = parse_paf(stdout_path)
    errors.extend(paf_parse_errors)
    paf_summary, paf_expectation_errors = validate_paf_expectation(
        case.expectation,
        paf_records,
        int(case.query["length"]),
        int(case.reference["length"]),
    )
    errors.extend(paf_expectation_errors)

    parsed = parse_stderr(stderr_path)
    errors.extend(parsed["parse_errors"])
    errors.extend(f"instrumentation: {value}" for value in parsed["instrumentation_errors"])
    map_records = parsed["map_records"]
    runl_records = parsed["runl_records"]
    anchors: list[SeedAnchor] = parsed["anchors"]
    seed_frequencies = parsed["seed_frequencies"]
    if len(map_records) != 1:
        errors.append(f"expected one MM_RUNL_MAP record, observed {len(map_records)}")
    else:
        map_record = map_records[0]
        if int(map_record.get("schema_version", -1)) != 1:
            errors.append("MM_RUNL_MAP schema_version must be 1")
        if int(map_record.get("query_length", -1)) != int(case.query["length"]):
            errors.append("MM_RUNL_MAP query length differs from the query FASTA")
        raw_minimizers = int(map_record.get("raw_query_minimizers", -1))
        filtered_minimizers = int(
            map_record.get("query_minimizers_after_query_filter", -1)
        )
        retained_anchors = int(map_record.get("retained_anchor_count", -1))
        if raw_minimizers < 1 or filtered_minimizers < 1:
            errors.append("MM_RUNL_MAP reports no query minimizers")
        if filtered_minimizers > raw_minimizers:
            errors.append("query minimizer count increased after query-side filtering")
        if retained_anchors <= 0:
            errors.append("MM_RUNL_MAP reports no retained anchors")
        if (
            case.expectation.get("require_no_query_minimizer_filter_loss", False)
            and filtered_minimizers != raw_minimizers
        ):
            errors.append(
                "query-side minimizer filtering removed "
                f"{raw_minimizers - filtered_minimizers} minimizer occurrences"
            )

    initial_seed_frequencies = [
        record for record in seed_frequencies if record["collection_pass"] == "initial"
    ]
    seed_frequency_expectation = case.expectation.get("seed_frequency")
    seed_frequency_summary: dict[str, Any] = {}
    if seed_frequency_expectation is not None:
        if not isinstance(seed_frequency_expectation, dict):
            errors.append("expectation.seed_frequency must be an object")
        else:
            allowed = {
                int(value)
                for value in seed_frequency_expectation.get(
                    "allowed_initial_reference_occurrences", []
                )
            }
            observed_values = sorted(
                {int(record["reference_occurrences"]) for record in initial_seed_frequencies}
            )
            filtered_count = sum(
                int(record["filtered"]) for record in initial_seed_frequencies
            )
            if not initial_seed_frequencies:
                errors.append("no initial SF seed-frequency records were emitted")
            if any(value not in allowed for value in observed_values):
                errors.append(
                    f"initial SF occurrence values {observed_values} exceed allowed {sorted(allowed)}"
                )
            required_filtered = int(
                seed_frequency_expectation.get("required_initial_filtered_count", 0)
            )
            if filtered_count != required_filtered:
                errors.append(
                    f"initial SF filtered count {filtered_count} differs from {required_filtered}"
                )
            occurrence_histogram: dict[str, int] = {}
            for record in initial_seed_frequencies:
                key = str(record["reference_occurrences"])
                occurrence_histogram[key] = occurrence_histogram.get(key, 0) + 1
            seed_frequency_summary = {
                "initial_record_count": len(initial_seed_frequencies),
                "observed_reference_occurrence_values": observed_values,
                "reference_occurrence_histogram": occurrence_histogram,
                "filtered_count": filtered_count,
                "post_map_rechain_record_count": len(seed_frequencies)
                - len(initial_seed_frequencies),
            }
    if not runl_records:
        errors.append("no MM_RUNL records were emitted")

    dump_errors: list[str] = []
    dumps: list[ChainDump] = []
    for path in sorted(dump_dir.glob("runl_call_*.tsv")):
        try:
            dumps.append(parse_dump(path))
        except ValueError as error:
            dump_errors.append(str(error))
    errors.extend(dump_errors)
    if not dumps:
        errors.append("no chaining dump was emitted")
    stratified_calls, stratified_errors = analyze_stratified_calls(dumps, runl_records)
    errors.extend(stratified_errors)

    dp_records = [record for record in runl_records if record.get("stage") == "dp"]
    dp_call_ids = sorted({int(record["call_id"]) for record in dp_records if "call_id" in record})
    initial_call_id = dp_call_ids[0] if dp_call_ids else None
    initial_records = [
        record for record in dp_records if int(record.get("call_id", -1)) == initial_call_id
    ]
    by_lane = {int(record.get("lanes", -1)): record for record in initial_records}
    if set(by_lane) != set(DEFAULT_LANE_WIDTHS):
        errors.append(
            "initial DP call lane records differ from declared widths: "
            f"observed {sorted(by_lane)}, expected {list(DEFAULT_LANE_WIDTHS)}"
        )
    if len(by_lane) != len(initial_records):
        errors.append("initial DP call has duplicate lane-width records")

    initial_dump_matches = [
        dump for dump in dumps if dump.stage == "dp" and dump.call_id == initial_call_id
    ]
    initial_dump = initial_dump_matches[0] if len(initial_dump_matches) == 1 else None
    if len(initial_dump_matches) != 1:
        errors.append(
            "expected one dump for the initial DP call, observed "
            f"{len(initial_dump_matches)}"
        )

    path: list[int] = []
    path_errors: list[str] = []
    candidate_summary: dict[str, Any] = {}
    gap_summary: dict[str, Any] = {}
    dumped_unit_runs: dict[str, Any] = {}
    independent_oracle: dict[str, Any] = {}
    coordinate_summary: dict[str, Any] = {"declared": False}
    construction_summary: dict[str, Any] = {}
    repeat_statistics: dict[str, Any] = {}
    per_lane: dict[str, Any] = {}
    if initial_dump is not None and initial_dump.candidates:
        top_rank, top_score, top_index = initial_dump.candidates[-1]
        maximum_count = sum(
            score == top_score for _, score, _ in initial_dump.candidates
        )
        second_score = (
            initial_dump.candidates[-2][1]
            if len(initial_dump.candidates) >= 2
            else None
        )
        candidate_summary = {
            "candidate_count": initial_dump.candidate_end_count,
            "top_rank": top_rank,
            "top_score": top_score,
            "top_index": top_index,
            "maximum_score_candidate_count": maximum_count,
            "unique_maximum_score": maximum_count == 1,
            "second_ranked_score": second_score,
            "top_minus_second_score": (
                top_score - second_score if second_score is not None else None
            ),
        }
        if case.expectation.get("require_unique_top_score", True) and maximum_count != 1:
            errors.append(
                f"top score {top_score} is shared by {maximum_count} candidate ends"
            )
        expected_multiplicity = case.expectation.get(
            "expected_maximum_score_candidate_multiplicity"
        )
        if expected_multiplicity is not None and maximum_count != int(expected_multiplicity):
            errors.append(
                f"top-score candidate multiplicity {maximum_count} differs from "
                f"declared {expected_multiplicity}"
            )
        tie_policy = case.expectation.get("candidate_tie_policy")
        if tie_policy is not None:
            if not isinstance(tie_policy, dict):
                errors.append("candidate_tie_policy must be an object")
            else:
                supported_policy = {
                    "candidate_sort_order": "score_ascending_then_anchor_index_ascending",
                    "selection_rule": "consume_candidates_from_end",
                    "equal_score_selection_order": "anchor_index_descending",
                }
                for field, supported_value in supported_policy.items():
                    if tie_policy.get(field) != supported_value:
                        errors.append(
                            f"unsupported candidate tie policy {field}="
                            f"{tie_policy.get(field)!r}; expected {supported_value!r}"
                        )
                top_score_indices = [
                    index
                    for _, score, index in initial_dump.candidates
                    if score == top_score
                ]
                declared_top_score = tie_policy.get("expected_top_score")
                if declared_top_score is None or int(declared_top_score) != top_score:
                    errors.append(
                        f"top candidate score {top_score} differs from declared "
                        f"{declared_top_score}"
                    )
                declared_top_indices = tie_policy.get(
                    "expected_top_score_anchor_indices"
                )
                if (
                    not isinstance(declared_top_indices, list)
                    or any(not isinstance(index, int) for index in declared_top_indices)
                    or declared_top_indices != top_score_indices
                ):
                    errors.append(
                        "top-score candidate indices differ from the declared tie set: "
                        f"observed {top_score_indices}, declared {declared_top_indices}"
                    )
                expected_first_index = tie_policy.get("expected_first_start_index")
                if expected_first_index is None or int(expected_first_index) != top_index:
                    errors.append(
                        f"first ranked start {top_index} differs from declared "
                        f"{expected_first_index}"
                    )
                if top_score_indices and top_index != max(top_score_indices):
                    errors.append(
                        "consume-from-end tie policy did not select the greatest "
                        "anchor index among maximum-score candidates"
                    )
                expected_first_label = tie_policy.get("expected_first_start_label")
                try:
                    with Path(case.construction_artifact["resolved_path"]).open(
                        "r", encoding="utf-8"
                    ) as source:
                        tie_construction = json.load(source)
                    declared_anchors = tie_construction.get("anchors")
                    observed_first_label = (
                        str(declared_anchors[top_index]["label"])
                        if isinstance(declared_anchors, list)
                        and 0 <= top_index < len(declared_anchors)
                        and isinstance(declared_anchors[top_index], dict)
                        and "label" in declared_anchors[top_index]
                        else None
                    )
                except (OSError, ValueError, TypeError) as error:
                    observed_first_label = None
                    errors.append(
                        f"could not resolve declared first-start label: {error}"
                    )
                if (
                    not isinstance(expected_first_label, str)
                    or observed_first_label != expected_first_label
                ):
                    errors.append(
                        f"first ranked start label {observed_first_label!r} differs "
                        f"from declared {expected_first_label!r}"
                    )
                candidate_summary["declared_tie_policy"] = tie_policy
                candidate_summary["observed_top_score_anchor_indices"] = (
                    top_score_indices
                )
                candidate_summary["observed_first_start_label"] = (
                    observed_first_label
                )
        path, path_errors = reconstruct_path(top_index, initial_dump.predecessors)
        errors.extend(path_errors)
        minimum_path_anchors = int(case.expectation.get("minimum_path_anchors", max(DEFAULT_LANE_WIDTHS)))
        if len(path) < minimum_path_anchors:
            errors.append(
                f"first path has {len(path)} anchors, below required {minimum_path_anchors}"
            )
        gap_summary = summarize_index_gaps(path, initial_dump.predecessors)
        dumped_unit_runs = selected_path_unit_runs(path, initial_dump.predecessors)
        if len(anchors) == initial_dump.anchor_count:
            repeat_statistics = selected_path_repeat_statistics(
                path, anchors, initial_dump.predecessors
            )
        coordinate_summary, coordinate_errors = check_primary_coordinates(
            case.expectation, path, anchors
        )
        errors.extend(coordinate_errors)
        coordinate_path = independent_coordinate_path(
            case.expectation, anchors, top_index
        )
        if coordinate_path is not None:
            if not coordinate_path:
                errors.append("declared coordinate path contains no SD anchors")
            elif coordinate_path != path:
                errors.append(
                    "dumped first predecessor path differs from the independently "
                    "selected declared-diagonal SD path"
                )
            coordinate_runs = independent_unit_runs(coordinate_path)
            independent_oracle = {
                "source": "declared SD coordinate path, independent of dumped p[]",
                "path_anchor_count": len(coordinate_path),
                "path_indices_sha256": canonical_json_sha256(coordinate_path),
                "unit_runs": coordinate_runs,
            }
        else:
            with Path(case.construction_artifact["resolved_path"]).open(
                "r", encoding="utf-8"
            ) as source:
                construction_for_oracle = json.load(source)
            if (
                construction_for_oracle.get("construction_type")
                == "unique_minimizer_island_adversarial_control"
                and isinstance(construction_for_oracle.get("anchors"), list)
            ):
                declared_predecessors = tuple(
                    int(anchor["predecessor"])
                    for anchor in construction_for_oracle["anchors"]
                )
                declared_path, declared_path_errors = reconstruct_path(
                    top_index, declared_predecessors
                )
                errors.extend(
                    f"comb independent oracle: {error}"
                    for error in declared_path_errors
                )
                if declared_path != path:
                    errors.append("dumped comb path differs from declared predecessor oracle")
                coordinate_runs = independent_unit_runs(declared_path)
                independent_oracle = {
                    "source": "comb construction's predeclared predecessor array",
                    "path_anchor_count": len(declared_path),
                    "path_indices_sha256": canonical_json_sha256(declared_path),
                    "unit_runs": coordinate_runs,
                }
            else:
                coordinate_runs = {
                    "lengths": dumped_unit_runs.get("lengths", []),
                    "run_count": dumped_unit_runs.get("run_count", 0),
                    "maximum_length": dumped_unit_runs.get("maximum_length", 0),
                    "total_unit_link_anchors": dumped_unit_runs.get(
                        "total_unit_link_anchors", 0
                    ),
                }
                independent_oracle = {
                    "source": "construction has no independent path declaration",
                    "path_anchor_count": len(path),
                    "unit_runs": coordinate_runs,
                }
        spaced_expectation = case.expectation.get("spaced_copy")
        if isinstance(spaced_expectation, dict):
            copies = int(spaced_expectation.get("copy_count", 0))
            oracle_lengths = list(coordinate_runs.get("lengths", []))
            if copies == 1 and oracle_lengths != [len(path)]:
                errors.append(
                    "R=1 spaced-copy oracle expected one unit run spanning the path"
                )
            if copies >= 2 and max(oracle_lengths, default=0) >= min(DEFAULT_LANE_WIDTHS):
                errors.append(
                    "R>=2 spaced-copy oracle found a unit run long enough for L=2"
                )
        construction_summary, construction_errors = analyze_construction(
            case, anchors, path, initial_dump
        )
        errors.extend(construction_errors)
        if len(anchors) != initial_dump.anchor_count:
            errors.append(
                f"SD anchor count {len(anchors)} differs from initial DP anchor count "
                f"{initial_dump.anchor_count}"
            )

        telemetry_fields = (
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
        )
        for lanes in DEFAULT_LANE_WIDTHS:
            record = by_lane.get(lanes)
            emulated = emulate_runl(top_index, initial_dump.predecessors, lanes)
            emulated["scalar_path_length"] = len(path)
            emulated["scalar_validation_error"] = 0
            closed_form = closed_form_runl_from_unit_runs(
                len(path), list(coordinate_runs["lengths"]), lanes
            )
            mismatches: dict[str, dict[str, int]] = {}
            oracle_mismatches: dict[str, dict[str, int]] = {}
            for field, oracle_value in closed_form.items():
                recomputed_value = emulated[field]
                if oracle_value != recomputed_value:
                    oracle_mismatches[field] = {
                        "window_emulation": recomputed_value,
                        "closed_form_oracle": oracle_value,
                    }
            if record is not None:
                for field in telemetry_fields:
                    observed = int(record.get(field, -1))
                    expected = emulated[field]
                    if observed != expected:
                        mismatches[field] = {"telemetry": observed, "recomputed": expected}
                fixed_identities = {
                    "schema_version": 1,
                    "call_id": initial_dump.call_id,
                    "anchor_count": initial_dump.anchor_count,
                    "candidate_end_count": initial_dump.candidate_end_count,
                    "min_score": initial_dump.min_score,
                    "has_ranked_end": 1,
                }
                for field, expected_value in fixed_identities.items():
                    observed_value = int(record.get(field, -1))
                    if observed_value != expected_value:
                        mismatches[field] = {
                            "telemetry": observed_value,
                            "recomputed": expected_value,
                        }
                expected_fast_flag = int(emulated["fast_batches"] > 0)
                if int(record.get("has_fast_batch", -1)) != expected_fast_flag:
                    mismatches["has_fast_batch"] = {
                        "telemetry": int(record.get("has_fast_batch", -1)),
                        "recomputed": expected_fast_flag,
                    }
                if int(record.get("start_index", -2)) != top_index:
                    mismatches["start_index"] = {
                        "telemetry": int(record.get("start_index", -2)),
                        "recomputed": top_index,
                    }
                if int(record.get("start_score", -2)) != top_score:
                    mismatches["start_score"] = {
                        "telemetry": int(record.get("start_score", -2)),
                        "recomputed": top_score,
                    }
            if mismatches:
                errors.append(f"L={lanes} telemetry differs from dump reconstruction")
            if oracle_mismatches:
                errors.append(f"L={lanes} closed-form unit-run oracle differs from emulation")
            per_lane[str(lanes)] = {
                **emulated,
                "closed_form_unit_run_oracle": closed_form,
                "runl_true_percent_of_all_loop_evaluations": percent(
                    emulated["fast_batches"], emulated["loop_evaluations"]
                ),
                "runl_true_percent_when_bounds_allow_vector": percent(
                    emulated["fast_batches"], emulated["bounds_pass"]
                ),
                "path_anchor_percent_processed_by_vector_batches": percent(
                    emulated["fast_covered_anchors"], emulated["primary_path_anchors"]
                ),
                "telemetry_reconstruction_mismatches": mismatches,
                "closed_form_oracle_mismatches": oracle_mismatches,
            }
    elif initial_dump is not None:
        errors.append("initial DP dump has no score-qualified candidate end")

    if map_records and initial_dump is not None:
        retained = int(map_records[0].get("retained_anchor_count", -1))
        if retained != initial_dump.anchor_count:
            errors.append(
                "initial DP anchor count differs from MM_RUNL_MAP retained count"
            )

    dump_artifacts = [
        {
            "path": str(path.relative_to(raw_dir)),
            "sha256": sha256_file(path),
            "size_bytes": path.stat().st_size,
        }
        for path in sorted(dump_dir.glob("runl_call_*.tsv"))
    ]
    return {
        "accepted": not errors,
        "errors": errors,
        "warnings": warnings,
        "paf": {
            "nonempty_line_count": paf_lines,
            **paf_summary,
        },
        "telemetry": {
            "map_records": map_records,
            "runl_record_count": len(runl_records),
            "stratified_calls": stratified_calls,
            "initial_dp_call_id": initial_call_id,
            "initial_dp_records": initial_records,
            "instrumentation_errors": parsed["instrumentation_errors"],
            "parse_errors": parsed["parse_errors"],
            "seed_anchor_count": len(anchors),
            "seed_frequency": seed_frequency_summary,
            "query_filter": (
                {
                    "raw_query_minimizers": int(
                        map_records[0].get("raw_query_minimizers", -1)
                    ),
                    "query_minimizers_after_query_filter": int(
                        map_records[0].get(
                            "query_minimizers_after_query_filter", -1
                        )
                    ),
                    "removed_query_minimizers": int(
                        map_records[0].get("raw_query_minimizers", -1)
                    )
                    - int(
                        map_records[0].get(
                            "query_minimizers_after_query_filter", -1
                        )
                    ),
                    "retained_anchor_count": int(
                        map_records[0].get("retained_anchor_count", -1)
                    ),
                }
                if len(map_records) == 1
                else None
            ),
        },
        "first_path": {
            "candidate_ordering": candidate_summary,
            "path_anchor_count": len(path),
            "path_indices_sha256": canonical_json_sha256(path),
            "predecessor_index_gaps": gap_summary,
            "dumped_maximal_unit_runs": dumped_unit_runs,
            "independent_oracle": independent_oracle,
            "declared_primary_coordinates": coordinate_summary,
            "construction_analysis": construction_summary,
            "realized_repeat_statistics": repeat_statistics,
            "by_lane": per_lane,
        },
        "dump_artifacts": dump_artifacts,
    }


def run_process(
    command: list[str],
    environment: dict[str, str],
    stdout_path: Path,
    stderr_path: Path,
    timeout_seconds: float,
) -> ProcessOutcome:
    """Run minimap2 with bounded timeout and sample its resident memory."""

    start = time.monotonic()
    peak_rss: int | None = None
    samples = 0
    timed_out = False
    interrupted = False
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        process = subprocess.Popen(
            command,
            cwd=ROOT,
            env=environment,
            stdout=stdout,
            stderr=stderr,
            start_new_session=True,
        )
        ACTIVE_PROCESSES.add(process.pid)
        try:
            while process.poll() is None:
                rss = process_rss_kib(process.pid)
                if rss is not None:
                    peak_rss = rss if peak_rss is None else max(peak_rss, rss)
                samples += 1
                elapsed = time.monotonic() - start
                if STOP_REQUESTED.is_set():
                    interrupted = True
                    try:
                        os.killpg(process.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    break
                if elapsed > timeout_seconds:
                    timed_out = True
                    try:
                        os.killpg(process.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    break
                time.sleep(PROCESS_POLL_SECONDS)
            if process.poll() is None:
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
            exit_code = process.wait()
        finally:
            ACTIVE_PROCESSES.discard(process.pid)
    return ProcessOutcome(
        exit_code=exit_code,
        timed_out=timed_out,
        interrupted=interrupted,
        wall_seconds=time.monotonic() - start,
        peak_rss_kib=peak_rss,
        samples=samples,
    )


def make_command(
    binary: Path, mapping: dict[str, Any], reference: Path, query: Path
) -> list[str]:
    """Construct the exact one-thread minimap2 invocation."""

    arguments = list(mapping["args"])
    if "-t" in arguments or any(argument.startswith("-t") for argument in arguments):
        raise ValueError("manifest.mapping.args must not set -t, which the runner fixes to 1")
    if "--print-seeds" in arguments:
        raise ValueError("manifest.mapping.args must not duplicate --print-seeds")
    forbidden_exact = {
        "-a",
        "--sam-hit-only",
        "--split-prefix",
        "-o",
        "-d",
        "--version",
        "--help",
    }
    forbidden_prefixes = ("--split-prefix=", "-o", "-d")
    forbidden_long_names = ("sam-hit-only", "split-prefix", "version", "help")
    for argument in arguments:
        if argument in forbidden_exact or argument.startswith(forbidden_prefixes):
            raise ValueError(
                f"manifest.mapping.args contains an unsafe/output-changing option: {argument}"
            )
        if argument.startswith("--"):
            long_name = argument[2:].split("=", 1)[0]
            if long_name and any(
                forbidden.startswith(long_name) for forbidden in forbidden_long_names
            ):
                raise ValueError(
                    "manifest.mapping.args contains a forbidden abbreviated long "
                    f"option: {argument}"
                )
        if (
            argument.startswith("-")
            and not argument.startswith("--")
            and any(option in argument[1:] for option in "atod")
        ):
            raise ValueError(
                f"manifest.mapping.args contains a forbidden clustered short option: {argument}"
            )
    command = [str(binary), "-t", "1"]
    if mapping.get("emit_seed_coordinates"):
        command.append("--print-seeds")
    if mapping.get("emit_seed_frequencies", False):
        command.append("--dbg-seed-occ")
    command.extend(arguments)
    command.extend((str(reference), str(query)))
    return command


def run_one_case(
    case: CaseSpec,
    manifest_path: Path,
    manifest_digest: str,
    binary: Path,
    binary_digest: str,
    mapping: dict[str, Any],
    raw_root: Path,
) -> tuple[dict[str, Any], Path]:
    """Run, validate, and atomically publish one case's raw artifacts."""

    temporary = raw_root / f".{case.case_id}.tmp.{os.getpid()}.{uuid.uuid4().hex}"
    final = raw_root / case.case_id
    if final.exists():
        raise FileExistsError(f"raw output already exists for {case.case_id}: {final}")
    temporary.mkdir(parents=True)
    dump_dir = temporary / "dumps"
    dump_dir.mkdir()
    stdout_path = temporary / "stdout.paf"
    stderr_path = temporary / "stderr.log"
    reference = Path(case.reference["resolved_path"])
    query = Path(case.query["resolved_path"])
    identity = case_identity(case)
    identity_digest = canonical_json_sha256(identity)
    source_digests = analysis_source_digests()
    command = make_command(binary, mapping, reference, query)
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    environment["MM_RUNL_EXPERIMENT"] = "1"
    environment["MM_RUNL_DUMP_DIR"] = str(dump_dir.resolve())
    invocation = {
        "schema_version": 1,
        "case_id": case.case_id,
        "started_at_utc": utc_timestamp(),
        "manifest_path": str(manifest_path.resolve()),
        "manifest_sha256": manifest_digest,
        "case_identity_sha256": identity_digest,
        "binary_path": str(binary.resolve()),
        "binary_sha256": binary_digest,
        "analysis_source_sha256": source_digests,
        "command": command,
        "environment_overrides": {
            "LC_ALL": "C",
            "MM_RUNL_EXPERIMENT": "1",
            "MM_RUNL_DUMP_DIR": "dumps",
        },
        "reference_sha256": case.reference["file_sha256"],
        "query_sha256": case.query["file_sha256"],
    }
    atomic_write_json(temporary / "invocation.json", invocation)
    try:
        outcome = run_process(
            command,
            environment,
            stdout_path,
            stderr_path,
            float(mapping["timeout_seconds"]),
        )
        validation = validate_case_artifacts(case, temporary, outcome)
        completed_at = utc_timestamp()
        raw_artifacts = {
            name: {
                "path": name,
                "sha256": sha256_file(temporary / name),
                "size_bytes": (temporary / name).stat().st_size,
            }
            for name in ("stdout.paf", "stderr.log", "invocation.json")
        }
        result = {
            "schema_version": 1,
            "case_id": case.case_id,
            "order": case.order,
            "replicate_id": case.replicate_id,
            "seed": case.seed,
            "group": case.group,
            "parameters": case.parameters,
            "expectation": case.expectation,
            "status": "accepted" if validation["accepted"] else "rejected",
            "started_at_utc": invocation["started_at_utc"],
            "completed_at_utc": completed_at,
            "provenance": {
                "manifest_path": str(manifest_path.resolve()),
                "manifest_sha256": manifest_digest,
                "case_identity_sha256": identity_digest,
                "minimap2_path": str(binary.resolve()),
                "minimap2_sha256": binary_digest,
                "analysis_source_sha256": source_digests,
            },
            "process": asdict(outcome),
            "validation": validation,
            "raw_artifacts": raw_artifacts,
            "input_artifacts": {
                "reference": {
                    key: value
                    for key, value in case.reference.items()
                    if key != "resolved_path"
                },
                "query": {
                    key: value for key, value in case.query.items() if key != "resolved_path"
                },
                "construction": {
                    key: value
                    for key, value in case.construction_artifact.items()
                    if key != "resolved_path"
                },
            },
        }
        atomic_write_json(temporary / "case_result.json", result)
        os.replace(temporary, final)
        return result, final
    except BaseException:
        failed = raw_root / f"{case.case_id}.failed.{uuid.uuid4().hex}"
        if temporary.exists():
            os.replace(temporary, failed)
        raise


def load_shard(path: Path) -> dict[str, Any]:
    """Load one result shard."""

    with path.open("r", encoding="utf-8") as source:
        value = json.load(source)
    if not isinstance(value, dict):
        raise ValueError(f"invalid result shard: {path}")
    return value


def write_shard(path: Path, result: dict[str, Any]) -> None:
    """Publish one immutable result shard."""

    if path.exists():
        raise FileExistsError(f"result shard already exists: {path}")
    atomic_write_json(path, result)


def publish_results(
    path: Path,
    manifest_path: Path,
    manifest_digest: str,
    binary: Path,
    binary_digest: str,
    resource_policy: dict[str, Any],
    cases: list[CaseSpec],
    selected_case_ids: set[str],
    shard_dir: Path,
) -> None:
    """Rebuild the human-readable YAML aggregate from completed shards."""

    runs: list[dict[str, Any]] = []
    for case in cases:
        shard = shard_dir / f"{case.case_id}.json"
        if shard.is_file():
            runs.append(load_shard(shard))
    summary = {
        "manifest_case_count": len(cases),
        "selected_case_count": len(selected_case_ids),
        "attempted_global": len(runs),
        "accepted": sum(run.get("status") == "accepted" for run in runs),
        "rejected": sum(run.get("status") == "rejected" for run in runs),
        "pending_global": len(cases) - len(runs),
    }
    if summary["pending_global"]:
        experiment_status = "partial"
    elif summary["rejected"]:
        experiment_status = "complete_with_rejections"
    else:
        experiment_status = "complete_pass"
    document = {
        "schema_version": 1,
        "experiment_id": "runl_repeat_disruption",
        "generated_at_utc": utc_timestamp(),
        "experiment_status": experiment_status,
        "selection": {
            "is_full_manifest": len(selected_case_ids) == len(cases),
            "selected_case_ids": sorted(selected_case_ids),
        },
        "manifest": {
            "path": str(manifest_path.resolve()),
            "sha256": manifest_digest,
        },
        "minimap2": {"path": str(binary.resolve()), "sha256": binary_digest},
        "lane_widths": list(DEFAULT_LANE_WIDTHS),
        "resource_policy": resource_policy,
        "summary": summary,
        "runs": runs,
    }
    encoded = yaml.safe_dump(
        document,
        sort_keys=False,
        allow_unicode=False,
        width=100,
    ).encode("utf-8")
    atomic_write_bytes(path, (encoded,))


def archive_failed_case(case_id: str, raw_root: Path, shard_path: Path) -> None:
    """Move a rejected prior attempt aside before an explicit rerun."""

    existing = raw_root / case_id
    timestamp = utc_timestamp().replace(":", "").replace("-", "")
    archive_root = raw_root / "superseded" / f"{case_id}.{timestamp}"
    archive_root.parent.mkdir(parents=True, exist_ok=True)
    if shard_path.exists():
        archive_root.mkdir(parents=True, exist_ok=True)
        archived_shard = archive_root / "published_shard.json"
        os.replace(shard_path, archived_shard)
    if existing.exists():
        archived_raw = archive_root / "raw_commit"
        archive_root.mkdir(parents=True, exist_ok=True)
        os.replace(existing, archived_raw)


def validate_existing_result(
    case: CaseSpec,
    raw_root: Path,
    shard_path: Path | None,
    manifest_path: Path,
    manifest_digest: str,
    binary: Path,
    binary_digest: str,
) -> dict[str, Any]:
    """Validate an existing raw commit and optional published shard for safe resume."""

    raw_case = raw_root / case.case_id
    if not raw_case.is_dir():
        raise ValueError(f"existing shard has no raw directory for {case.case_id}")
    embedded_path = raw_case / "case_result.json"
    if not embedded_path.is_file():
        raise ValueError(f"raw directory for {case.case_id} lacks case_result.json")
    embedded = load_shard(embedded_path)
    if shard_path is not None and shard_path.exists():
        shard = load_shard(shard_path)
        if canonical_json_sha256(shard) != canonical_json_sha256(embedded):
            raise ValueError(f"published shard differs from raw commit for {case.case_id}")
    else:
        shard = embedded

    expected_provenance = {
        "manifest_path": str(manifest_path.resolve()),
        "manifest_sha256": manifest_digest,
        "case_identity_sha256": canonical_json_sha256(case_identity(case)),
        "minimap2_path": str(binary.resolve()),
        "minimap2_sha256": binary_digest,
        "analysis_source_sha256": analysis_source_digests(),
    }
    if shard.get("provenance") != expected_provenance:
        raise ValueError(f"existing result provenance mismatch for {case.case_id}")
    if shard.get("case_id") != case.case_id or shard.get("order") != case.order:
        raise ValueError(f"existing result identity mismatch for {case.case_id}")

    raw_artifacts = shard.get("raw_artifacts")
    if not isinstance(raw_artifacts, dict):
        raise ValueError(f"existing result lacks raw_artifacts for {case.case_id}")
    for label, declaration in raw_artifacts.items():
        if not isinstance(declaration, dict) or not isinstance(declaration.get("path"), str):
            raise ValueError(f"invalid raw artifact declaration {label} for {case.case_id}")
        path = (raw_case / declaration["path"]).resolve()
        try:
            path.relative_to(raw_case.resolve())
        except ValueError as error:
            raise ValueError(f"raw artifact escapes case directory for {case.case_id}") from error
        if not path.is_file():
            raise ValueError(f"raw artifact is missing for {case.case_id}: {path}")
        if sha256_file(path) != declaration.get("sha256"):
            raise ValueError(f"raw artifact digest mismatch for {case.case_id}: {path}")
        if path.stat().st_size != int(declaration.get("size_bytes", -1)):
            raise ValueError(f"raw artifact size mismatch for {case.case_id}: {path}")

    dump_artifacts = shard.get("validation", {}).get("dump_artifacts", [])
    if not isinstance(dump_artifacts, list):
        raise ValueError(f"invalid dump artifact list for {case.case_id}")
    declared_dump_paths: set[str] = set()
    for declaration in dump_artifacts:
        if not isinstance(declaration, dict) or not isinstance(declaration.get("path"), str):
            raise ValueError(f"invalid dump declaration for {case.case_id}")
        path = (raw_case / declaration["path"]).resolve()
        declared_dump_paths.add(str(path.relative_to(raw_case.resolve())))
        try:
            path.relative_to(raw_case.resolve())
        except ValueError as error:
            raise ValueError(f"dump artifact escapes case directory for {case.case_id}") from error
        if not path.is_file() or sha256_file(path) != declaration.get("sha256"):
            raise ValueError(f"dump artifact digest mismatch for {case.case_id}: {path}")
        if path.stat().st_size != int(declaration.get("size_bytes", -1)):
            raise ValueError(f"dump artifact size mismatch for {case.case_id}: {path}")
    actual_dump_paths = {
        str(path.resolve().relative_to(raw_case.resolve()))
        for path in (raw_case / "dumps").glob("runl_call_*.tsv")
        if path.is_file()
    }
    if actual_dump_paths != declared_dump_paths:
        raise ValueError(f"dump artifact set mismatch for {case.case_id}")
    expected_top_level = {
        "case_result.json",
        "dumps",
        "invocation.json",
        "stderr.log",
        "stdout.paf",
    }
    if {path.name for path in raw_case.iterdir()} != expected_top_level:
        raise ValueError(f"raw top-level artifact set mismatch for {case.case_id}")

    expected_inputs = {
        "reference": {
            key: value for key, value in case.reference.items() if key != "resolved_path"
        },
        "query": {
            key: value for key, value in case.query.items() if key != "resolved_path"
        },
        "construction": {
            key: value
            for key, value in case.construction_artifact.items()
            if key != "resolved_path"
        },
    }
    if shard.get("input_artifacts") != expected_inputs:
        raise ValueError(f"existing result input declarations mismatch for {case.case_id}")
    invocation_path = raw_case / "invocation.json"
    with invocation_path.open("r", encoding="utf-8") as source:
        invocation = json.load(source)
    if invocation.get("manifest_sha256") != manifest_digest:
        raise ValueError(f"invocation manifest digest mismatch for {case.case_id}")
    if invocation.get("binary_sha256") != binary_digest:
        raise ValueError(f"invocation binary digest mismatch for {case.case_id}")
    if invocation.get("analysis_source_sha256") != expected_provenance["analysis_source_sha256"]:
        raise ValueError(f"invocation analysis-source digest mismatch for {case.case_id}")
    if invocation.get("case_identity_sha256") != expected_provenance["case_identity_sha256"]:
        raise ValueError(f"invocation case digest mismatch for {case.case_id}")
    return shard


def install_signal_handlers() -> None:
    """Request graceful shutdown and terminate active process groups."""

    def handler(_signal_number: int, _frame: Any) -> None:
        STOP_REQUESTED.set()
        ACTIVE_PROCESSES.terminate_all()

    signal.signal(signal.SIGINT, handler)
    signal.signal(signal.SIGTERM, handler)


def acquire_runner_lock(path: Path) -> None:
    """Acquire a nonblocking process-wide lock for this output namespace."""

    global RUNNER_LOCK_DESCRIPTOR
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT, 0o644)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as error:
        os.close(descriptor)
        raise RuntimeError(f"another repeat experiment runner holds {path}") from error
    RUNNER_LOCK_DESCRIPTOR = descriptor


def main() -> int:
    """Run pending cases with bounded, memory-aware process parallelism."""

    arguments = parse_args()
    manifest_path = require_under_experiment_root(arguments.manifest, "--manifest")
    manifest, cases = load_manifest(manifest_path)
    mapping = manifest["mapping"]
    binary = require_under_experiment_root(arguments.minimap2, "--minimap2")
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError(f"minimap2 binary is missing or not executable: {binary}")
    manifest_digest = sha256_file(manifest_path)
    binary_digest = sha256_file(binary)
    case_filter = expanded_filter(arguments.only_case)
    group_filter = expanded_filter(arguments.only_group)
    selected = [
        case
        for case in cases
        if (case_filter is None or case.case_id in case_filter)
        and (group_filter is None or case.group in group_filter)
    ]
    if not selected:
        raise ValueError("case filters selected no manifest entries")
    jobs, resource_policy = choose_jobs(arguments.jobs)
    raw_root = require_under_experiment_root(arguments.raw_dir, "--raw-dir")
    shard_dir = require_under_experiment_root(arguments.shard_dir, "--shard-dir")
    results_path = require_under_experiment_root(arguments.results, "--results")
    progress_log = require_under_experiment_root(
        arguments.progress_log, "--progress-log"
    )
    if raw_root == ROOT.resolve() or shard_dir == ROOT.resolve():
        raise ValueError("raw and shard directories must be strict children of the experiment root")
    validate_output_namespace(
        manifest_path,
        binary,
        cases,
        raw_root,
        shard_dir,
        results_path,
        progress_log,
    )
    if not arguments.dry_run:
        raw_root.mkdir(parents=True, exist_ok=True)
        shard_dir.mkdir(parents=True, exist_ok=True)
        acquire_runner_lock(ROOT / ".runl_repeat_experiment.lock")

    existing_results: dict[str, dict[str, Any]] = {}
    for case in cases:
        shard_path = shard_dir / f"{case.case_id}.json"
        raw_exists = (raw_root / case.case_id).exists()
        shard_exists = shard_path.exists()
        if shard_exists and not raw_exists:
            raise ValueError(f"case {case.case_id} has a shard but no raw commit")
        if raw_exists:
            prior = validate_existing_result(
                case,
                raw_root,
                shard_path if shard_exists else None,
                manifest_path,
                manifest_digest,
                binary,
                binary_digest,
            )
            if not shard_exists:
                if not arguments.dry_run:
                    write_shard(shard_path, prior)
                    append_progress(
                        progress_log,
                        "recovered_shard_from_raw_commit",
                        case_id=case.case_id,
                    )
            existing_results[case.case_id] = prior

    pending: list[CaseSpec] = []
    for case in selected:
        prior = existing_results.get(case.case_id)
        shard_path = shard_dir / f"{case.case_id}.json"
        if prior is None:
            pending.append(case)
        elif arguments.rerun_failed and prior.get("status") != "accepted":
            if not arguments.dry_run:
                archive_failed_case(case.case_id, raw_root, shard_path)
                existing_results.pop(case.case_id, None)
            pending.append(case)

    print(
        json.dumps(
            {
                "selected": len(selected),
                "pending": len(pending),
                "jobs": jobs,
                "dry_run": arguments.dry_run,
            },
            sort_keys=True,
        ),
        flush=True,
    )
    if arguments.dry_run:
        for case in pending:
            print(case.case_id)
        return 0

    append_progress(
        progress_log,
        "runner_start",
        selected_case_count=len(selected),
        pending_case_count=len(pending),
        manifest_sha256=manifest_digest,
        minimap2_sha256=binary_digest,
        resource_policy=resource_policy,
    )

    install_signal_handlers()
    futures: dict[Future[tuple[dict[str, Any], Path]], CaseSpec] = {}
    pending_iter = iter(pending)
    exhausted = False
    completed = 0
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        while not exhausted or futures:
            if STOP_REQUESTED.is_set():
                exhausted = True
            while (
                not exhausted
                and not STOP_REQUESTED.is_set()
                and len(futures) < jobs
                and memory_allows_launch(len(futures), resource_policy)
            ):
                try:
                    case = next(pending_iter)
                except StopIteration:
                    exhausted = True
                    break
                append_progress(progress_log, "case_start", case_id=case.case_id)
                future = executor.submit(
                    run_one_case,
                    case,
                    manifest_path,
                    manifest_digest,
                    binary,
                    binary_digest,
                    mapping,
                    raw_root,
                )
                futures[future] = case
            if not futures:
                if exhausted:
                    break
                append_progress(progress_log, "launch_paused_for_memory")
                time.sleep(1.0)
                continue
            done, _ = wait(futures, timeout=1.0, return_when=FIRST_COMPLETED)
            for future in done:
                case = futures.pop(future)
                try:
                    result, final_raw = future.result()
                    write_shard(shard_dir / f"{case.case_id}.json", result)
                    completed += 1
                    append_progress(
                        progress_log,
                        "case_complete",
                        case_id=case.case_id,
                        status=result["status"],
                        wall_seconds=result["process"]["wall_seconds"],
                        raw_dir=str(final_raw),
                    )
                    print(
                        json.dumps(
                            {
                                "case_id": case.case_id,
                                "status": result["status"],
                                "completed_this_run": completed,
                                "remaining_this_run": len(pending) - completed,
                            },
                            sort_keys=True,
                        ),
                        flush=True,
                    )
                except BaseException as error:
                    append_progress(
                        progress_log,
                        "case_exception",
                        case_id=case.case_id,
                        error_type=type(error).__name__,
                        error=str(error),
                    )
                    STOP_REQUESTED.set()
                    ACTIVE_PROCESSES.terminate_all()
                    raise
                finally:
                    publish_results(
                        results_path,
                        manifest_path,
                        manifest_digest,
                        binary,
                        binary_digest,
                        resource_policy,
                        cases,
                        {case.case_id for case in selected},
                        shard_dir,
                    )
            sample_host_resources(resource_policy)

    publish_results(
        results_path,
        manifest_path,
        manifest_digest,
        binary,
        binary_digest,
        resource_policy,
        cases,
        {case.case_id for case in selected},
        shard_dir,
    )
    append_progress(
        progress_log,
        "runner_stop",
        interrupted=STOP_REQUESTED.is_set(),
        completed_this_run=completed,
    )
    if STOP_REQUESTED.is_set():
        return 130
    selected_results = [
        load_shard(shard_dir / f"{case.case_id}.json")
        for case in selected
        if (shard_dir / f"{case.case_id}.json").is_file()
    ]
    if len(selected_results) != len(selected):
        return 3
    if any(result.get("status") != "accepted" for result in selected_results):
        return 2
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"runl_repeat_run_experiment.py: {type(error).__name__}: {error}", file=sys.stderr)
        raise SystemExit(1)
