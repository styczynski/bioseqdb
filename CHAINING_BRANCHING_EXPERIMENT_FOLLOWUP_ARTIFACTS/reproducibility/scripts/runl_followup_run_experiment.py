#!/usr/bin/env python3
"""Run a manifest-defined standalone minimap2 Run_L follow-up experiment.

Each attempted run publishes two immutable artifacts: raw stderr and a JSON
result shard.  The human-readable YAML result is rebuilt atomically from those
shards only after an invocation finishes, so interruption never loses already
completed runs and does not require rewriting the full YAML after every run.
"""

from __future__ import annotations

import argparse
import base64
import binascii
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
import platform
import re
import shlex
import signal
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path
from typing import Any

import yaml


ROOT = Path(__file__).resolve().parent
DEFAULT_MANIFEST = ROOT / "followup_data" / "manifest.json"
DEFAULT_RAW_DIR = ROOT / "followup_raw"
DEFAULT_SHARD_DIR = ROOT / "followup_run_shards"
DEFAULT_OUTPUT = ROOT / "followup_results.yaml"
DEFAULT_MINIMAP2 = ROOT / "minimap2"
TIME_BINARY = Path("/usr/bin/time")
LANE_WIDTHS = (4, 8, 16)
RESOURCE_SAMPLE_INTERVAL_SECONDS = 0.5
RAW_COUNTERS = (
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
    "has_ranked_end",
    "has_fast_batch",
)
SHARED_LANE_FIELDS = (
    "anchor_count",
    "candidate_end_count",
    "start_index",
    "start_score",
    "min_score",
    "has_ranked_end",
    "primary_path_anchors",
    "scalar_path_length",
)


@dataclass(frozen=True)
class ArmSpec:
    """One minimap2 argument arm declared by the workload manifest."""

    arm_id: str
    args: tuple[str, ...]
    metadata: dict[str, Any]


@dataclass(frozen=True)
class RunSpec:
    """One dataset, occurrence-count, and mapping-arm combination."""

    order: int
    run_id: str
    dataset_id: str
    arm_id: str
    replicate_id: str
    occurrence_count: int
    default_args: tuple[str, ...]
    arm_args: tuple[str, ...]
    timeout_seconds: float
    dataset: dict[str, Any]
    reference: dict[str, Any]


@dataclass(frozen=True)
class ProcessOutcome:
    """Observed process result, including runner-level launch failures."""

    attempted: bool
    exit_code: int | None
    timed_out: bool
    failure_kind: str | None
    failure_message: str | None
    observed_wall_seconds: float
    interrupted: bool = False


class RunCancelled(RuntimeError):
    """Indicate that a queued run was cancelled before publishing artifacts."""


class ActiveProcessGroups:
    """Track minimap2 process groups so termination signals cannot orphan them."""

    def __init__(self) -> None:
        self._process_group_ids: set[int] = set()

    def register(self, process_group_id: int) -> None:
        """Register a newly started standalone process group."""

        self._process_group_ids.add(process_group_id)

    def unregister(self, process_group_id: int) -> None:
        """Forget a process group after its leader has been reaped."""

        self._process_group_ids.discard(process_group_id)

    def terminate_all(self, signal_number: int) -> None:
        """Send a signal to a stable snapshot of every active process group."""

        process_group_ids = tuple(self._process_group_ids)
        for process_group_id in process_group_ids:
            try:
                os.killpg(process_group_id, signal_number)
            except OSError:
                pass


def utc_now() -> str:
    """Return a stable UTC timestamp suitable for result artifacts."""

    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def sha256(path: Path) -> str:
    """Return the SHA-256 digest of a file without loading it all into memory."""

    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def json_digest(value: Any) -> str:
    """Hash a JSON-compatible value using a canonical serialization."""

    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def display_path(path: Path) -> str:
    """Prefer a repository-relative artifact path while retaining external paths."""

    absolute = path.resolve()
    try:
        return str(absolute.relative_to(ROOT))
    except ValueError:
        return str(absolute)


def resolve_input_path(manifest_path: Path, declared_path: str) -> Path:
    """Resolve manifest input paths relative to the manifest's directory."""

    path = Path(declared_path)
    return path if path.is_absolute() else manifest_path.parent / path


def string_list(value: Any, field: str) -> tuple[str, ...]:
    """Validate that a manifest argument vector is an ordered string list."""

    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise ValueError(f"{field} must be a JSON array of strings")
    return tuple(value)


def bounded_jobs(value: str) -> int:
    """Parse a process parallelism value while enforcing the four-job ceiling."""

    try:
        jobs = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("--jobs must be an integer") from error
    if jobs < 1 or jobs > 4:
        raise argparse.ArgumentTypeError("--jobs must be between 1 and 4")
    return jobs


def comma_values(values: list[str] | None) -> set[str] | None:
    """Expand repeatable comma-separated CLI filters into a set."""

    if not values:
        return None
    expanded = {
        item.strip()
        for value in values
        for item in value.split(",")
        if item.strip()
    }
    return expanded or None


def n_filter(values: list[str] | None) -> set[int] | None:
    """Parse the repeatable occurrence-count filter."""

    expanded = comma_values(values)
    if expanded is None:
        return None
    try:
        parsed = {int(value) for value in expanded}
    except ValueError as error:
        raise ValueError("--only-N values must be integers") from error
    if any(value < 1 for value in parsed):
        raise ValueError("--only-N values must be positive")
    return parsed


def normalize_arms(mapping: dict[str, Any]) -> dict[str, ArmSpec]:
    """Normalize list- or mapping-shaped arm declarations."""

    raw_arms = mapping.get("arms", [])
    entries: list[tuple[str, Any]] = []
    if isinstance(raw_arms, dict):
        entries = [(str(arm_id), raw) for arm_id, raw in raw_arms.items()]
    elif isinstance(raw_arms, list):
        for index, raw in enumerate(raw_arms):
            if not isinstance(raw, dict) or "arm_id" not in raw:
                raise ValueError(f"mapping.arms[{index}] must contain arm_id")
            entries.append((str(raw["arm_id"]), raw))
    else:
        raise ValueError("mapping.arms must be an array or object")

    arms: dict[str, ArmSpec] = {}
    for arm_id, raw in entries:
        if arm_id in arms:
            raise ValueError(f"duplicate mapping arm_id: {arm_id}")
        if isinstance(raw, list):
            args = string_list(raw, f"mapping.arms.{arm_id}")
            metadata: dict[str, Any] = {}
        elif isinstance(raw, dict):
            args = string_list(raw.get("args", []), f"mapping arm {arm_id}.args")
            metadata = {key: value for key, value in raw.items() if key not in {"arm_id", "args"}}
        else:
            raise ValueError(f"mapping arm {arm_id} must be an object or string array")
        arms[arm_id] = ArmSpec(arm_id=arm_id, args=args, metadata=metadata)
    return arms


def reference_for_n(dataset: dict[str, Any], occurrence_count: int) -> dict[str, Any]:
    """Resolve one decimal-keyed reference entry from a dataset."""

    references = dataset.get("references")
    if not isinstance(references, dict):
        raise ValueError(f"dataset {dataset.get('dataset_id')} has no references object")
    reference = references.get(str(occurrence_count))
    if not isinstance(reference, dict):
        raise ValueError(
            f"dataset {dataset.get('dataset_id')} lacks reference N={occurrence_count}"
        )
    return reference


def build_specs(manifest: dict[str, Any]) -> tuple[list[RunSpec], dict[str, Any]]:
    """Expand the manifest datasets into a deterministic run matrix."""

    if manifest.get("schema_version") != 2:
        raise ValueError("manifest.schema_version must be 2")
    if manifest.get("status") != "complete":
        raise ValueError("manifest.status must be 'complete' before mapping starts")
    mapping = manifest.get("mapping")
    if not isinstance(mapping, dict):
        raise ValueError("manifest.mapping must be an object")
    if "default_args" not in mapping:
        raise ValueError("manifest.mapping.default_args must be explicitly declared")
    default_args = string_list(mapping["default_args"], "mapping.default_args")
    mapping_threads = int(mapping.get("mapping_threads", 1))
    if mapping_threads != 1:
        raise ValueError("manifest.mapping.mapping_threads must be 1 for this experiment")
    timeout_seconds = float(mapping.get("timeout_seconds", 3600))
    if timeout_seconds <= 0:
        raise ValueError("manifest.mapping.timeout_seconds must be positive")
    arms = normalize_arms(mapping)

    datasets = manifest.get("datasets")
    if not isinstance(datasets, list) or not datasets:
        raise ValueError("manifest.datasets must be a non-empty array")

    specs: list[RunSpec] = []
    seen_dataset_ids: set[str] = set()
    seen_run_ids: set[str] = set()
    order = 0
    for dataset_index, dataset in enumerate(datasets):
        if not isinstance(dataset, dict):
            raise ValueError(f"datasets[{dataset_index}] must be an object")
        dataset_id = str(dataset.get("dataset_id", ""))
        arm_id = str(dataset.get("arm_id", ""))
        replicate_id = str(dataset.get("replicate_id", ""))
        if not dataset_id or not arm_id or not replicate_id:
            raise ValueError(
                f"datasets[{dataset_index}] requires dataset_id, arm_id, and replicate_id"
            )
        if dataset_id in seen_dataset_ids:
            raise ValueError(f"duplicate dataset_id: {dataset_id}")
        seen_dataset_ids.add(dataset_id)
        if arm_id not in arms:
            raise ValueError(f"dataset {dataset_id} refers to undeclared mapping arm {arm_id}")
        for length_field in ("reference_length", "source_length", "query_length"):
            length_value = dataset.get(length_field)
            if (
                isinstance(length_value, bool)
                or not isinstance(length_value, int)
                or length_value <= 0
            ):
                raise ValueError(f"dataset {dataset_id}.{length_field} must be a positive integer")

        raw_n_values = dataset.get("n_values")
        if not isinstance(raw_n_values, list) or not raw_n_values:
            raise ValueError(f"dataset {dataset_id}.n_values must be a non-empty array")
        n_values = [int(value) for value in raw_n_values]
        if len(set(n_values)) != len(n_values) or any(value < 1 for value in n_values):
            raise ValueError(f"dataset {dataset_id}.n_values must contain unique positive integers")
        references = dataset.get("references")
        if not isinstance(references, dict):
            raise ValueError(f"dataset {dataset_id}.references must be an object")
        try:
            reference_n_values = {int(value) for value in references}
        except ValueError as error:
            raise ValueError(
                f"dataset {dataset_id}.references keys must be decimal N values"
            ) from error
        if reference_n_values != set(n_values):
            raise ValueError(
                f"dataset {dataset_id}.references keys must exactly match n_values"
            )

        arm = arms[arm_id]
        for occurrence_count in n_values:
            reference = reference_for_n(dataset, occurrence_count)
            if reference.get("intended_occurrences") != occurrence_count:
                raise ValueError(
                    f"dataset {dataset_id} reference N={occurrence_count} has inconsistent intended_occurrences"
                )
            if reference.get("verified_positions_match_intended") is not True:
                raise ValueError(
                    f"dataset {dataset_id} reference N={occurrence_count} lacks exact-position verification"
                )
            if reference.get("verified_exact_source_occurrences_overlapping") != occurrence_count:
                raise ValueError(
                    f"dataset {dataset_id} reference N={occurrence_count} has an incorrect verified occurrence count"
                )
            if reference.get("verified_positions_sha256") != reference.get(
                "intended_positions_sha256"
            ):
                raise ValueError(
                    f"dataset {dataset_id} reference N={occurrence_count} position hashes disagree"
                )
            run_id = f"{dataset_id}__N{occurrence_count}"
            if run_id in seen_run_ids:
                raise ValueError(f"duplicate derived run_id: {run_id}")
            seen_run_ids.add(run_id)
            specs.append(
                RunSpec(
                    order=order,
                    run_id=run_id,
                    dataset_id=dataset_id,
                    arm_id=arm_id,
                    replicate_id=replicate_id,
                    occurrence_count=occurrence_count,
                    default_args=default_args,
                    arm_args=arm.args,
                    timeout_seconds=timeout_seconds,
                    dataset=dataset,
                    reference=reference,
                )
            )
            order += 1

    normalized_mapping = {
        "default_args": list(default_args),
        "arms": [
            {"arm_id": arm.arm_id, "args": list(arm.args), **arm.metadata}
            for arm in arms.values()
        ],
        "mapping_threads": 1,
        "timeout_seconds": timeout_seconds,
    }
    return specs, normalized_mapping


def artifact_stem(run_id: str) -> str:
    """Create a collision-resistant, filesystem-safe name for one run."""

    safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", run_id).strip("._") or "run"
    return f"{safe[:140]}__{hashlib.sha256(run_id.encode('utf-8')).hexdigest()[:12]}"


def spec_payload(spec: RunSpec) -> dict[str, Any]:
    """Return the scientifically relevant immutable fingerprint payload."""

    dataset = spec.dataset
    reference = spec.reference

    def hashes(entry: Any) -> dict[str, Any]:
        if not isinstance(entry, dict):
            return {}
        return {key: value for key, value in entry.items() if key.endswith("sha256")}

    return {
        "run_id": spec.run_id,
        "dataset_id": spec.dataset_id,
        "arm_id": spec.arm_id,
        "replicate_id": spec.replicate_id,
        "occurrence_count": spec.occurrence_count,
        "default_args": list(spec.default_args),
        "arm_args": list(spec.arm_args),
        "mapping_threads": 1,
        "timeout_seconds": spec.timeout_seconds,
        "master_seed": dataset.get("master_seed"),
        "derived_seeds": dataset.get("seeds"),
        "reference_length": dataset.get("reference_length"),
        "source_length": dataset.get("source_length"),
        "query_length": dataset.get("query_length"),
        "source": {"path": (dataset.get("source") or {}).get("path"), **hashes(dataset.get("source"))},
        "query": {"path": (dataset.get("query") or {}).get("path"), **hashes(dataset.get("query"))},
        "placements": {
            "path": (dataset.get("placements") or {}).get("path"),
            **hashes(dataset.get("placements")),
        },
        "reference": {
            "path": reference.get("path"),
            **hashes(reference),
            "intended_occurrences": reference.get("intended_occurrences"),
            "intended_positions_sha256": reference.get("intended_positions_sha256"),
        },
        "mutation_model": dataset.get("mutation_model"),
        "realized_edits": dataset.get("realized_edits"),
    }


def rate(
    numerator: int | None, denominator: int | None
) -> dict[str, int | float | bool | None]:
    """Represent a rate without turning an undefined zero-denominator rate into zero."""

    if numerator is None or denominator is None:
        return {
            "numerator": numerator,
            "denominator": denominator,
            "defined": False,
            "fraction": None,
            "percent": None,
        }
    if denominator == 0:
        return {
            "numerator": numerator,
            "denominator": 0,
            "defined": False,
            "fraction": None,
            "percent": None,
        }
    fraction = numerator / denominator
    return {
        "numerator": numerator,
        "denominator": denominator,
        "defined": True,
        "fraction": fraction,
        "percent": 100.0 * fraction,
    }


def integer_field(record: dict[str, Any], key: str) -> int | None:
    """Return a telemetry integer without accepting booleans as counters."""

    value = record.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        return None
    return value


def counter_identity_checks(record: dict[str, Any]) -> tuple[list[dict[str, Any]], list[str]]:
    """Evaluate every exact identity implied by the Run_L instrumentation."""

    required = {
        "call_id",
        "lanes",
        "anchor_count",
        "min_score",
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
        "has_ranked_end",
        "has_fast_batch",
        "candidate_end_count",
        "start_index",
        "start_score",
    }
    values = {key: integer_field(record, key) for key in required}
    errors = [f"missing or non-integer Run_L field: {key}" for key, value in values.items() if value is None]
    for key, value in values.items():
        if value is not None and key not in {"start_index", "start_score"} and value < 0:
            errors.append(f"negative Run_L counter: {key}")
    if errors:
        return [], errors

    if record.get("schema_version") != 1:
        errors.append("Run_L schema_version must be 1")
    if record.get("stage") not in {"dp", "rmq"}:
        errors.append("Run_L stage must be 'dp' or 'rmq'")

    lanes = values["lanes"]
    loops = values["loop_evaluations"]
    bounds_pass = values["bounds_pass"]
    fast_batches = values["fast_batches"]
    scalar_fallbacks = values["scalar_fallbacks"]
    primary_path = values["primary_path_anchors"]
    candidate_ends = values["candidate_end_count"]
    assert lanes is not None
    assert loops is not None
    assert bounds_pass is not None
    assert fast_batches is not None
    assert scalar_fallbacks is not None
    assert primary_path is not None
    assert candidate_ends is not None

    expressions = [
        (
            "loop_evaluations == fast_batches + scalar_fallbacks",
            loops,
            fast_batches + scalar_fallbacks,
        ),
        (
            "bounds_pass == fast_batches + contiguous_mismatch",
            bounds_pass,
            fast_batches + int(values["contiguous_mismatch"]),
        ),
        (
            "loop_evaluations == bounds_pass + bounds_fail_current + bounds_fail_capacity + bounds_fail_both",
            loops,
            bounds_pass
            + int(values["bounds_fail_current"])
            + int(values["bounds_fail_capacity"])
            + int(values["bounds_fail_both"]),
        ),
        (
            "primary_path_anchors == lanes * fast_batches + scalar_fallbacks",
            primary_path,
            lanes * fast_batches + scalar_fallbacks,
        ),
        (
            "fast_covered_anchors == lanes * fast_batches",
            int(values["fast_covered_anchors"]),
            lanes * fast_batches,
        ),
        (
            "primary_path_anchors == scalar_path_length",
            primary_path,
            int(values["scalar_path_length"]),
        ),
        ("scalar_validation_error == 0", int(values["scalar_validation_error"]), 0),
        ("emulation_error == 0", int(values["emulation_error"]), 0),
        ("bounds_fail_capacity == 0", int(values["bounds_fail_capacity"]), 0),
        (
            "has_fast_batch == (fast_batches > 0)",
            int(values["has_fast_batch"]),
            int(fast_batches > 0),
        ),
        (
            "has_ranked_end == (candidate_end_count > 0)",
            int(values["has_ranked_end"]),
            int(candidate_ends > 0),
        ),
    ]
    checks = [
        {"identity": identity, "left": left, "right": right, "passes": left == right}
        for identity, left, right in expressions
    ]
    errors.extend(check["identity"] for check in checks if not check["passes"])

    if candidate_ends == 0:
        if values["start_index"] != -1:
            errors.append("start_index must be -1 when candidate_end_count == 0")
        if values["start_score"] != -1:
            errors.append("start_score must be -1 when candidate_end_count == 0")
    else:
        if int(values["start_index"]) < 0:
            errors.append("start_index must be nonnegative when a ranked end exists")
        if int(values["start_score"]) < int(values["min_score"]):
            errors.append("ranked start_score must satisfy min_score")
    if primary_path > int(values["anchor_count"]):
        errors.append("primary_path_anchors exceeds anchor_count")
    if candidate_ends > int(values["anchor_count"]):
        errors.append("candidate_end_count exceeds anchor_count")
    return checks, errors


def decorate_record(record: dict[str, Any]) -> dict[str, Any]:
    """Attach exact counter checks and denominator-safe rates to one Run_L record."""

    output = dict(record)
    checks, errors = counter_identity_checks(record)
    fast_batches = integer_field(record, "fast_batches")
    loops = integer_field(record, "loop_evaluations")
    bounds_pass = integer_field(record, "bounds_pass")
    fast_covered = integer_field(record, "fast_covered_anchors")
    primary_path = integer_field(record, "primary_path_anchors")
    lanes = integer_field(record, "lanes")
    output["rates"] = {
        "predicate_true_per_loop_evaluation": rate(fast_batches, loops),
        "predicate_true_per_bounds_passing_evaluation": rate(fast_batches, bounds_pass),
        "fast_path_anchor_coverage": rate(fast_covered, primary_path),
    }
    output["scalar_iterations_avoided"] = (
        None if lanes is None or fast_batches is None else (lanes - 1) * fast_batches
    )
    output["counter_identity_checks"] = checks
    output["validation_errors"] = errors
    return output


def sum_field(records: list[dict[str, Any]], key: str) -> int | None:
    """Sum a counter, returning null rather than fabricating a partial total."""

    values = [integer_field(record, key) for record in records]
    return None if any(value is None for value in values) else sum(int(value) for value in values)


def initial_chaining_call(
    records: list[dict[str, Any]],
) -> tuple[str | None, int | None, list[dict[str, Any]]]:
    """Select the protocol's initial DP call, or initial RMQ call for RMQ-only arms."""

    dp_records = [record for record in records if record.get("stage") == "dp"]
    selected = dp_records or [record for record in records if record.get("stage") == "rmq"]
    call_ids = [integer_field(record, "call_id") for record in selected]
    valid_call_ids = [call_id for call_id in call_ids if call_id is not None]
    if not valid_call_ids:
        return None, None, []
    first_call_id = min(valid_call_ids)
    initial_records = [
        record
        for record in selected
        if integer_field(record, "call_id") == first_call_id
    ]
    return str(initial_records[0].get("stage")), first_call_id, initial_records


def aggregate_records(records: list[dict[str, Any]]) -> dict[str, Any]:
    """Aggregate Run_L telemetry without mixing DP and RMQ stages or lane widths."""

    decorated = [decorate_record(record) for record in records]
    by_stage: dict[str, Any] = {}
    stages = sorted({str(record.get("stage", "unknown")) for record in decorated})
    for stage in stages:
        stage_records = [record for record in decorated if str(record.get("stage", "unknown")) == stage]
        lane_summaries: dict[str, Any] = {}
        for lanes in LANE_WIDTHS:
            selected = [record for record in stage_records if integer_field(record, "lanes") == lanes]
            totals = {counter: sum_field(selected, counter) for counter in RAW_COUNTERS}
            totals["backtrack_calls"] = len(selected)
            totals["candidate_end_count_total"] = sum_field(selected, "candidate_end_count")
            totals["anchor_count_total"] = sum_field(selected, "anchor_count")
            validation_errors = [
                f"call {record.get('call_id')}: {error}"
                for record in selected
                for error in record["validation_errors"]
            ]
            lane_summaries[str(lanes)] = {
                "raw": totals,
                "rates": {
                    "predicate_true_per_loop_evaluation": rate(
                        totals["fast_batches"], totals["loop_evaluations"]
                    ),
                    "predicate_true_per_bounds_passing_evaluation": rate(
                        totals["fast_batches"], totals["bounds_pass"]
                    ),
                    "fast_path_anchor_coverage": rate(
                        totals["fast_covered_anchors"], totals["primary_path_anchors"]
                    ),
                    "calls_with_any_fast_batch": rate(
                        totals["has_fast_batch"], totals["has_ranked_end"]
                    ),
                },
                "scalar_iterations_avoided": (
                    None
                    if totals["fast_batches"] is None
                    else (lanes - 1) * int(totals["fast_batches"])
                ),
                "validation_errors": validation_errors,
            }
        call_ids = sorted(
            {
                int(record["call_id"])
                for record in stage_records
                if integer_field(record, "call_id") is not None
            }
        )
        by_stage[stage] = {"call_ids": call_ids, "lanes": lane_summaries}

    initial_stage, initial_call_id, initial_records = initial_chaining_call(decorated)
    initial_by_lane: dict[str, Any] | None = None
    if initial_records:
        initial_by_lane = {
            str(lanes): next(
                (
                    record
                    for record in initial_records
                    if integer_field(record, "lanes") == lanes
                ),
                None,
            )
            for lanes in LANE_WIDTHS
        }

    all_errors = [
        f"{record.get('stage')} call {record.get('call_id')} L={record.get('lanes')}: {error}"
        for record in decorated
        for error in record["validation_errors"]
    ]
    return {
        "initial_chaining_stage": initial_stage,
        "initial_chaining_call_id": initial_call_id,
        "initial_chaining_by_lane": initial_by_lane,
        "initial_dp_by_lane": initial_by_lane if initial_stage == "dp" else None,
        "initial_rmq_by_lane": initial_by_lane if initial_stage == "rmq" else None,
        "by_stage": by_stage,
        "all_validation_errors": all_errors,
        "all_counter_identities_pass": None if not decorated else not all_errors,
    }


def parse_scalar(value: str) -> int | float | str:
    """Parse a diagnostic scalar while preserving unexpected text verbatim."""

    try:
        return int(value)
    except ValueError:
        try:
            return float(value)
        except ValueError:
            return value


def parse_index_diagnostics(stderr: str) -> dict[str, Any]:
    """Parse minimap2 index shape, frequency, and runtime summary diagnostics."""

    shape_pattern = re.compile(
        r"kmer size: (\d+); skip: (\d+); is_hpc: (\d+); #seq: (\d+)"
    )
    statistics_pattern = re.compile(
        r"distinct minimizers: ([\d,]+) \(([0-9.eE+-]+)% are singletons\); "
        r"average occurrences: ([0-9.eE+-]+); average spacing: ([0-9.eE+-]+); "
        r"total length: ([\d,]+)"
    )
    shapes = [
        {
            "k": int(match.group(1)),
            "w": int(match.group(2)),
            "is_hpc": bool(int(match.group(3))),
            "reference_sequence_count": int(match.group(4)),
        }
        for match in shape_pattern.finditer(stderr)
    ]
    statistics = [
        {
            "distinct_reference_minimizers": int(match.group(1).replace(",", "")),
            "singleton_minimizer_percent": float(match.group(2)),
            "average_reference_occurrences": float(match.group(3)),
            "average_reference_minimizer_spacing": float(match.group(4)),
            "total_reference_length": int(match.group(5).replace(",", "")),
        }
        for match in statistics_pattern.finditer(stderr)
    ]
    records: list[dict[str, Any]] = []
    for index in range(max(len(shapes), len(statistics))):
        records.append(
            {
                **(shapes[index] if index < len(shapes) else {}),
                **(statistics[index] if index < len(statistics) else {}),
            }
        )

    mid_occ_updates = [
        int(match.group(1))
        for match in re.finditer(r"\bmid_occ = (\d+)", stderr)
    ]
    version_match = re.search(r"^\[M::main\] Version: (.+)$", stderr, re.MULTILINE)
    command_match = re.search(r"^\[M::main\] CMD: (.+)$", stderr, re.MULTILINE)
    runtime_match = re.search(
        r"^\[M::main\] Real time: ([0-9.eE+-]+) sec; CPU: ([0-9.eE+-]+) sec; "
        r"Peak RSS: ([0-9.eE+-]+) GB$",
        stderr,
        re.MULTILINE,
    )
    primary = records[0] if records else {}
    return {
        "k": primary.get("k"),
        "w": primary.get("w"),
        "is_hpc": primary.get("is_hpc"),
        "reference_sequence_count": primary.get("reference_sequence_count"),
        "distinct_reference_minimizers": primary.get("distinct_reference_minimizers"),
        "singleton_minimizer_percent": primary.get("singleton_minimizer_percent"),
        "average_reference_occurrences": primary.get("average_reference_occurrences"),
        "average_reference_minimizer_spacing": primary.get(
            "average_reference_minimizer_spacing"
        ),
        "total_reference_length": primary.get("total_reference_length"),
        "records": records,
        "mid_occ_updates": mid_occ_updates,
        "reported_version": version_match.group(1) if version_match else None,
        "reported_command": command_match.group(1) if command_match else None,
        "runtime_summary": (
            {
                "real_seconds": float(runtime_match.group(1)),
                "cpu_seconds": float(runtime_match.group(2)),
                "peak_rss_gib": float(runtime_match.group(3)),
            }
            if runtime_match
            else None
        ),
    }


def summarize_seed_frequencies(
    records: list[dict[str, Any]],
    map_records: list[dict[str, Any]],
    *,
    threshold_field: str | None,
    summary_scope: str,
) -> dict[str, Any]:
    """Summarize SF records while retaining their exact per-minimizer values."""

    histogram: dict[int, dict[str, int]] = {}
    for record in records:
        occurrences = record["reference_occurrences"]
        bucket = histogram.setdefault(
            occurrences,
            {"reference_occurrences": occurrences, "seed_count": 0, "filtered_seed_count": 0, "retained_seed_count": 0},
        )
        bucket["seed_count"] += 1
        if record["filtered"]:
            bucket["filtered_seed_count"] += 1
        else:
            bucket["retained_seed_count"] += 1

    threshold_values = {
        int(record[threshold_field])
        for record in map_records
        if threshold_field is not None
        and isinstance(record.get(threshold_field), int)
        and not isinstance(record.get(threshold_field), bool)
    }
    filter_threshold = (
        next(iter(threshold_values)) if len(threshold_values) == 1 else None
    )
    rescued = [
        record
        for record in records
        if filter_threshold is not None
        and record["reference_occurrences"] > filter_threshold
        and not record["filtered"]
    ]
    return {
        "summary_scope": summary_scope,
        "record_scope": (
            "query minimizers remaining after query-side filtering that have at least one "
            "reference-index occurrence; zero-hit query minimizers do not emit SF records"
        ),
        "seed_count": len(records),
        "filtered_seed_count": sum(record["filtered"] for record in records),
        "retained_seed_count": sum(not record["filtered"] for record in records),
        "anchors_before_reference_filter": sum(
            record["reference_occurrences"] for record in records
        ),
        "anchors_after_reference_filter": sum(
            record["reference_occurrences"] for record in records if not record["filtered"]
        ),
        "anchors_removed_by_reference_filter": sum(
            record["reference_occurrences"] for record in records if record["filtered"]
        ),
        "filter_threshold_field_for_rescue_classification": threshold_field,
        "filter_threshold_for_rescue_classification": filter_threshold,
        "occ_dist_rescued_high_frequency_seed_count": (
            len(rescued) if threshold_field is not None else None
        ),
        "occ_dist_rescued_high_frequency_anchor_count": (
            sum(record["reference_occurrences"] for record in rescued)
            if threshold_field is not None
            else None
        ),
        "maximum_reference_occurrences": max(
            (record["reference_occurrences"] for record in records), default=None
        ),
        "occurrence_histogram": [histogram[key] for key in sorted(histogram)],
    }


def parse_stderr(stderr: str) -> dict[str, Any]:
    """Parse all experiment telemetry from immutable raw stderr."""

    map_records: list[dict[str, Any]] = []
    runl_records: list[dict[str, Any]] = []
    sf_records: list[dict[str, Any]] = []
    timings: list[dict[str, Any]] = []
    instrumentation_errors: list[str] = []
    parse_errors: list[str] = []

    for line_number, line in enumerate(stderr.splitlines(), 1):
        if line.startswith("MM_RUNL_MAP\t"):
            try:
                value = json.loads(line.split("\t", 1)[1])
                if not isinstance(value, dict):
                    raise ValueError("record is not a JSON object")
                map_records.append(value)
            except (json.JSONDecodeError, ValueError) as error:
                parse_errors.append(f"line {line_number}: invalid MM_RUNL_MAP record: {error}")
        elif line.startswith("MM_RUNL\t"):
            try:
                value = json.loads(line.split("\t", 1)[1])
                if not isinstance(value, dict):
                    raise ValueError("record is not a JSON object")
                runl_records.append(value)
            except (json.JSONDecodeError, ValueError) as error:
                parse_errors.append(f"line {line_number}: invalid MM_RUNL record: {error}")
        elif line.startswith("SF\t"):
            fields = line.split("\t")
            try:
                if len(fields) != 4:
                    raise ValueError(f"expected 4 tab-separated fields, found {len(fields)}")
                query_position, reference_occurrences, filtered = map(int, fields[1:])
                if query_position < 0 or reference_occurrences <= 0 or filtered not in {0, 1}:
                    raise ValueError("out-of-range SF value")
                sf_records.append(
                    {
                        "query_position": query_position,
                        "reference_occurrences": reference_occurrences,
                        "filtered": filtered,
                        "collection_pass": (
                            "initial_before_map_diagnostic"
                            if not map_records
                            else "post_map_rechain"
                        ),
                        "record_scope": (
                            "hit-producing query minimizer after query-side filtering"
                        ),
                    }
                )
            except ValueError as error:
                parse_errors.append(f"line {line_number}: invalid SF record: {error}")
        elif line.startswith("MM_TIME\t"):
            timing: dict[str, Any] = {}
            try:
                for field in line.split("\t")[1:]:
                    key, raw_value = field.split("=", 1)
                    timing[key] = parse_scalar(raw_value)
                timings.append(timing)
            except ValueError as error:
                parse_errors.append(f"line {line_number}: invalid MM_TIME record: {error}")
        elif line.startswith("MM_RUNL_ERROR\t"):
            instrumentation_errors.append(line.split("\t", 1)[1])

    initial_sf_records = [
        record
        for record in sf_records
        if record["collection_pass"] == "initial_before_map_diagnostic"
    ]
    post_map_sf_records = [
        record for record in sf_records if record["collection_pass"] == "post_map_rechain"
    ]
    return {
        "map_records": map_records,
        "runl_records": runl_records,
        "sf_records": sf_records,
        "initial_sf_records": initial_sf_records,
        "post_map_rechain_sf_records": post_map_sf_records,
        "sf_summary": summarize_seed_frequencies(
            initial_sf_records,
            map_records,
            threshold_field="mid_occ",
            summary_scope="initial seed collection before MM_RUNL_MAP",
        ),
        "post_map_rechain_sf_summary": summarize_seed_frequencies(
            post_map_sf_records,
            map_records,
            threshold_field="max_occ",
            summary_scope="optional post-map rechain seed collection",
        ),
        "sf_all_passes_summary": summarize_seed_frequencies(
            sf_records,
            map_records,
            threshold_field=None,
            summary_scope=(
                "all seed-collection records; a query minimizer can appear once per pass, "
                "so totals are collection events rather than unique minimizers"
            ),
        ),
        "timing_records": timings,
        "timing": timings[-1] if timings else None,
        "index_diagnostics": parse_index_diagnostics(stderr),
        "instrumentation_errors": instrumentation_errors,
        "parse_errors": parse_errors,
    }


def cross_lane_errors(records: list[dict[str, Any]]) -> list[str]:
    """Validate that every instrumented call has one consistent L4/L8/L16 triplet."""

    errors: list[str] = []
    grouped: dict[tuple[str, int], list[dict[str, Any]]] = {}
    for record in records:
        call_id = integer_field(record, "call_id")
        if call_id is None:
            errors.append("Run_L record has missing or non-integer call_id")
            continue
        grouped.setdefault((str(record.get("stage", "unknown")), call_id), []).append(record)

    call_ids = sorted({call_id for _, call_id in grouped})
    if call_ids and call_ids != list(range(1, call_ids[-1] + 1)):
        errors.append(f"Run_L call_ids must be contiguous from 1, observed {call_ids}")
    for call_id in call_ids:
        stages = {stage for stage, grouped_call_id in grouped if grouped_call_id == call_id}
        if len(stages) != 1:
            errors.append(f"Run_L call_id {call_id} appears in multiple stages: {sorted(stages)}")

    for (stage, call_id), selected in sorted(grouped.items()):
        lanes = [integer_field(record, "lanes") for record in selected]
        if sorted(lane for lane in lanes if lane is not None) != list(LANE_WIDTHS):
            errors.append(
                f"{stage} call {call_id}: expected exactly lanes {list(LANE_WIDTHS)}, observed {lanes}"
            )
        for field in SHARED_LANE_FIELDS:
            values = [record.get(field) for record in selected]
            if values and any(value != values[0] for value in values[1:]):
                errors.append(f"{stage} call {call_id}: {field} differs across lane records")
    return errors


def telemetry_validation(
    parsed: dict[str, Any],
    process: ProcessOutcome,
    *,
    expected_query_length: int | None = None,
    expected_reference_length: int | None = None,
    expected_index_options: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Validate parser completeness and exact SF/Run_L accounting."""

    parse_errors = list(parsed["parse_errors"])
    instrumentation_errors = list(parsed["instrumentation_errors"])
    identity_errors = [
        f"{record.get('stage')} call {record.get('call_id')} L={record.get('lanes')}: {error}"
        for record in parsed["runl_records"]
        for error in counter_identity_checks(record)[1]
    ]
    lane_errors = cross_lane_errors(parsed["runl_records"])
    accounting_errors: list[str] = []

    required_map_fields = (
        "query_length",
        "raw_query_minimizers",
        "query_minimizers_after_query_filter",
        "retained_anchor_count",
        "mid_occ",
        "max_occ",
        "max_max_occ",
        "occ_dist",
        "rep_len",
    )
    for index, record in enumerate(parsed["map_records"], 1):
        if record.get("schema_version") != 1:
            accounting_errors.append(f"MM_RUNL_MAP record {index} schema_version must be 1")
        for field in required_map_fields:
            value = record.get(field)
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                accounting_errors.append(
                    f"MM_RUNL_MAP record {index} has invalid {field}"
                )
        if (
            expected_query_length is not None
            and record.get("query_length") != expected_query_length
        ):
            accounting_errors.append(
                f"MM_RUNL_MAP record {index} query_length != manifest query_length"
            )

    required_time_fields = (
        "wall_seconds",
        "user_seconds",
        "system_seconds",
        "max_rss_kib",
        "exit_status",
    )
    for index, record in enumerate(parsed["timing_records"], 1):
        for field in required_time_fields:
            value = record.get(field)
            if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0:
                accounting_errors.append(f"MM_TIME record {index} has invalid {field}")
    if (
        parsed["timing"] is not None
        and process.exit_code is not None
        and process.exit_code >= 0
        and parsed["timing"].get("exit_status") != process.exit_code
    ):
        accounting_errors.append("MM_TIME exit_status != observed process exit_code")

    if process.attempted and process.exit_code == 0:
        if len(parsed["map_records"]) != 1:
            accounting_errors.append(
                f"successful one-query process must emit exactly one MM_RUNL_MAP record, observed {len(parsed['map_records'])}"
            )
        if parsed["timing"] is None:
            accounting_errors.append("successful process emitted no MM_TIME record")
        index_records = parsed["index_diagnostics"]["records"]
        if len(index_records) != 1:
            accounting_errors.append(
                f"successful one-reference process must emit one complete index diagnostic, observed {len(index_records)}"
            )
        else:
            index_record = index_records[0]
            required_positive_integer_fields = (
                "k",
                "w",
                "reference_sequence_count",
                "distinct_reference_minimizers",
                "total_reference_length",
            )
            for field in required_positive_integer_fields:
                value = index_record.get(field)
                if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
                    accounting_errors.append(f"index diagnostic has invalid {field}")
            if not isinstance(index_record.get("is_hpc"), bool):
                accounting_errors.append("index diagnostic has invalid is_hpc")
            singleton_percent = index_record.get("singleton_minimizer_percent")
            if (
                isinstance(singleton_percent, bool)
                or not isinstance(singleton_percent, (int, float))
                or not 0 <= singleton_percent <= 100
            ):
                accounting_errors.append(
                    "index diagnostic has invalid singleton_minimizer_percent"
                )
            for field in (
                "average_reference_occurrences",
                "average_reference_minimizer_spacing",
            ):
                value = index_record.get(field)
                if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
                    accounting_errors.append(f"index diagnostic has invalid {field}")
            if (
                expected_reference_length is not None
                and index_record.get("total_reference_length") != expected_reference_length
            ):
                accounting_errors.append(
                    "index total_reference_length != manifest reference_length"
                )
            if index_record.get("reference_sequence_count") != 1:
                accounting_errors.append(
                    "index reference_sequence_count != one-query pilot manifest expectation"
                )
            for field, expected in (expected_index_options or {}).items():
                if expected is not None and index_record.get(field) != expected:
                    accounting_errors.append(
                        f"observed index {field} != explicit manifest argument value {expected}"
                    )
        retained_anchors = sum(
            int(record["retained_anchor_count"])
            for record in parsed["map_records"]
            if isinstance(record.get("retained_anchor_count"), int)
            and not isinstance(record.get("retained_anchor_count"), bool)
        )
        if retained_anchors > 0 and not parsed["runl_records"]:
            accounting_errors.append(
                "successful process with retained anchors emitted no MM_RUNL records"
            )

    if parsed["map_records"]:
        maximum_possible_sf = sum(
            int(record["query_minimizers_after_query_filter"])
            for record in parsed["map_records"]
            if isinstance(record.get("query_minimizers_after_query_filter"), int)
        )
        if len(parsed["initial_sf_records"]) > maximum_possible_sf:
            accounting_errors.append(
                "SF record count exceeds sum(query_minimizers_after_query_filter)"
            )
        expected_anchors = sum(
            int(record["retained_anchor_count"])
            for record in parsed["map_records"]
            if isinstance(record.get("retained_anchor_count"), int)
        )
        observed_anchors = int(parsed["sf_summary"]["anchors_after_reference_filter"])
        if expected_anchors != observed_anchors:
            accounting_errors.append(
                "SF retained anchor sum != MM_RUNL_MAP retained_anchor_count"
            )

    if len(parsed["map_records"]) == 1 and parsed["runl_records"]:
        retained_anchor_count = parsed["map_records"][0].get("retained_anchor_count")
        _, _, initial_records = initial_chaining_call(parsed["runl_records"])
        if (
            isinstance(retained_anchor_count, int)
            and retained_anchor_count > 0
            and initial_records
        ):
            first_record = initial_records[0]
            if first_record.get("anchor_count") != retained_anchor_count:
                accounting_errors.append(
                    "initial chaining call anchor_count != MM_RUNL_MAP retained_anchor_count"
                )

    if len(parsed["map_records"]) == 1 and parsed["post_map_rechain_sf_records"]:
        retained_anchor_count = parsed["map_records"][0].get("retained_anchor_count")
        rechain_anchor_count = parsed["post_map_rechain_sf_summary"].get(
            "anchors_after_reference_filter"
        )
        if isinstance(rechain_anchor_count, int) and rechain_anchor_count > 0:
            dp_call_ids = sorted(
                {
                    int(record["call_id"])
                    for record in parsed["runl_records"]
                    if record.get("stage") == "dp"
                    and integer_field(record, "call_id") is not None
                }
            )
            if isinstance(retained_anchor_count, int) and retained_anchor_count > 0:
                rechain_call_ids = dp_call_ids[1:]
            else:
                # mg_lchain_dp emits no Run_L record for an empty initial anchor set;
                # in that case the first observed DP call belongs to the rechain pass.
                rechain_call_ids = dp_call_ids
            if len(rechain_call_ids) != 1:
                accounting_errors.append(
                    "post-map rechain with retained anchors must emit exactly one corresponding DP Run_L call"
                )
            else:
                rechain_records = [
                    record
                    for record in parsed["runl_records"]
                    if record.get("stage") == "dp"
                    and integer_field(record, "call_id") == rechain_call_ids[0]
                ]
                if not rechain_records or rechain_records[0].get(
                    "anchor_count"
                ) != rechain_anchor_count:
                    accounting_errors.append(
                        "post-map rechain DP anchor_count != retained rechain SF anchor sum"
                    )

    all_errors = parse_errors + instrumentation_errors + identity_errors + lane_errors + accounting_errors
    return {
        "parse_errors": parse_errors,
        "instrumentation_errors": instrumentation_errors,
        "counter_identity_errors": identity_errors,
        "cross_lane_errors": lane_errors,
        "seed_frequency_accounting_errors": accounting_errors,
        "run_l_counter_identities_pass": (
            None if not parsed["runl_records"] else not identity_errors
        ),
        "cross_lane_consistency_pass": (
            None if not parsed["runl_records"] else not lane_errors
        ),
        "seed_frequency_accounting_pass": not accounting_errors,
        "all_required_checks_pass": not all_errors,
        "all_errors": all_errors,
    }


def endpoint_eligibility(records: list[dict[str, Any]]) -> dict[str, Any]:
    """Report score-qualified candidates separately from the Run_L predicate."""

    stage, first_call, initial_records = initial_chaining_call(records)
    if not initial_records or first_call is None:
        return {
            "status": "not_evaluated",
            "has_score_qualified_candidate_end": None,
            "candidate_end_count": None,
            "start_score": None,
            "min_score": None,
        }
    record = initial_records[0]
    candidate_end_count = integer_field(record, "candidate_end_count")
    has_endpoint = None if candidate_end_count is None else candidate_end_count > 0
    return {
        "status": (
            "score_qualified_candidate_end_available"
            if has_endpoint
            else "no_score_qualified_candidate_end"
            if has_endpoint is False
            else "invalid_telemetry"
        ),
        "has_score_qualified_candidate_end": has_endpoint,
        "candidate_end_count": candidate_end_count,
        "start_score": integer_field(record, "start_score"),
        "min_score": integer_field(record, "min_score"),
        "stage": stage,
        "call_id": first_call,
    }


def artifact_info(
    manifest_path: Path, entry: Any, label: str
) -> tuple[dict[str, Any], list[str]]:
    """Hash and validate one manifest-declared input artifact."""

    errors: list[str] = []
    if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
        return {"label": label, "path": None}, [f"{label} entry lacks a string path"]
    path = resolve_input_path(manifest_path, entry["path"])
    declared_hashes = {
        key: value for key, value in entry.items() if key.endswith("sha256")
    }
    observed_hash: str | None = None
    size_bytes: int | None = None
    if not path.is_file():
        errors.append(f"{label} file does not exist: {path}")
    else:
        try:
            observed_hash = sha256(path)
            size_bytes = path.stat().st_size
        except OSError as error:
            errors.append(f"unable to hash {label} file {path}: {error}")
    declared_file_hash = entry.get("sha256")
    if not isinstance(declared_file_hash, str) or re.fullmatch(
        r"[0-9a-f]{64}", declared_file_hash
    ) is None:
        errors.append(f"{label} entry lacks a lowercase hexadecimal sha256")
    hash_matches = (
        None
        if not isinstance(declared_file_hash, str) or observed_hash is None
        else declared_file_hash == observed_hash
    )
    if hash_matches is False:
        errors.append(
            f"{label} sha256 mismatch: declared {declared_file_hash}, observed {observed_hash}"
        )
    return {
        "label": label,
        "declared_path": entry["path"],
        "resolved_path": str(path.resolve()),
        "result_path": display_path(path),
        "size_bytes": size_bytes,
        "declared_hashes": declared_hashes,
        "observed_sha256": observed_hash,
        "declared_file_sha256_matches": hash_matches,
    }, errors


def dataset_inputs(
    spec: RunSpec, manifest_path: Path
) -> tuple[dict[str, Any], list[str]]:
    """Collect source, query, placement, and selected-reference provenance."""

    entries = {
        "source": spec.dataset.get("source"),
        "query": spec.dataset.get("query"),
        "placements": spec.dataset.get("placements"),
        "reference": spec.reference,
    }
    infos: dict[str, Any] = {}
    errors: list[str] = []
    for label, entry in entries.items():
        info, entry_errors = artifact_info(manifest_path, entry, label)
        infos[label] = info
        errors.extend(entry_errors)
    return infos, errors


def fsync_directory(path: Path) -> None:
    """Best-effort fsync of a directory after publishing an artifact."""

    try:
        descriptor = os.open(path, os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(descriptor)
    except OSError:
        pass
    finally:
        os.close(descriptor)


def write_private_temp(directory: Path, basename: str, data: bytes) -> Path:
    """Write and fsync a uniquely named private temporary artifact."""

    directory.mkdir(parents=True, exist_ok=True)
    temporary = directory / f".{basename}.tmp.{os.getpid()}.{threading.get_ident()}.{uuid.uuid4().hex}"
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
    return temporary


def publish_temp_immutable(temporary: Path, destination: Path) -> None:
    """Atomically publish a file without ever replacing an existing artifact."""

    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(temporary, destination)
    except FileExistsError as error:
        raise RuntimeError(f"immutable artifact already exists: {destination}") from error
    finally:
        temporary.unlink(missing_ok=True)
    fsync_directory(destination.parent)


def write_immutable_json(path: Path, value: Any) -> None:
    """Serialize and atomically publish one immutable JSON artifact."""

    data = (
        json.dumps(value, sort_keys=True, indent=2, ensure_ascii=True, allow_nan=False)
        + "\n"
    ).encode("utf-8")
    temporary = write_private_temp(path.parent, path.name, data)
    publish_temp_immutable(temporary, path)


def atomic_write_yaml(path: Path, value: Any) -> None:
    """Atomically replace the consolidated YAML, which is derived from shards."""

    data = yaml.safe_dump(
        value,
        sort_keys=False,
        width=120,
        allow_unicode=False,
    ).encode("utf-8")
    temporary = write_private_temp(path.parent, path.name, data)
    try:
        os.replace(temporary, path)
        fsync_directory(path.parent)
    finally:
        temporary.unlink(missing_ok=True)


def terminate_and_reap(process: subprocess.Popen[bytes], signal_number: int) -> int:
    """Terminate one process group, escalating after a bounded grace period."""

    try:
        os.killpg(process.pid, signal_number)
    except ProcessLookupError:
        pass
    try:
        return process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        return process.wait(timeout=5)


def run_process(
    argv: list[str],
    environment: dict[str, str],
    timeout_seconds: float,
    stderr_temp: Path,
    *,
    shutdown_requested: threading.Event,
    active_process_groups: ActiveProcessGroups,
) -> ProcessOutcome:
    """Run the timed command in its own process group and preserve raw stderr."""

    if shutdown_requested.is_set():
        raise RunCancelled("process launch cancelled by SIGINT or SIGTERM")
    started = time.monotonic()
    process: subprocess.Popen[bytes] | None = None
    attempted = False
    exit_code: int | None = None
    timed_out = False
    interrupted = False
    failure_kind: str | None = None
    failure_message: str | None = None

    try:
        descriptor = os.open(stderr_temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
        with os.fdopen(descriptor, "wb") as stderr_output:
            try:
                if shutdown_requested.is_set():
                    raise RunCancelled("process launch cancelled by SIGINT or SIGTERM")
                process = subprocess.Popen(
                    argv,
                    cwd=ROOT,
                    env=environment,
                    stdout=subprocess.DEVNULL,
                    stderr=stderr_output,
                    start_new_session=True,
                )
                attempted = True
                active_process_groups.register(process.pid)
                try:
                    deadline = time.monotonic() + timeout_seconds
                    while True:
                        if shutdown_requested.is_set():
                            interrupted = True
                            failure_kind = "interrupted"
                            failure_message = "runner received SIGINT or SIGTERM"
                            exit_code = terminate_and_reap(process, signal.SIGTERM)
                            stderr_output.write(
                                f"MM_RUNNER_ERROR\t{failure_kind}: {failure_message}\n".encode(
                                    "utf-8"
                                )
                            )
                            break
                        exit_code = process.poll()
                        if exit_code is not None:
                            if shutdown_requested.is_set():
                                interrupted = True
                                failure_kind = "interrupted"
                                failure_message = "runner received SIGINT or SIGTERM"
                                stderr_output.write(
                                    f"MM_RUNNER_ERROR\t{failure_kind}: {failure_message}\n".encode(
                                        "utf-8"
                                    )
                                )
                            break
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            timed_out = True
                            failure_kind = "timeout"
                            failure_message = f"process exceeded {timeout_seconds:g} seconds"
                            exit_code = terminate_and_reap(process, signal.SIGTERM)
                            stderr_output.write(
                                f"MM_RUNNER_ERROR\t{failure_kind}: {failure_message}\n".encode(
                                    "utf-8"
                                )
                            )
                            break
                        shutdown_requested.wait(min(0.2, remaining))
                finally:
                    active_process_groups.unregister(process.pid)
            except FileNotFoundError as error:
                failure_kind = "executable_not_found"
                failure_message = str(error)
                stderr_output.write(f"MM_RUNNER_ERROR\t{failure_kind}: {error}\n".encode("utf-8"))
            except OSError as error:
                failure_kind = "process_launch_error"
                failure_message = str(error)
                stderr_output.write(f"MM_RUNNER_ERROR\t{failure_kind}: {error}\n".encode("utf-8"))
            stderr_output.flush()
            os.fsync(stderr_output.fileno())
    except RunCancelled:
        stderr_temp.unlink(missing_ok=True)
        raise
    except OSError as error:
        raise RuntimeError(f"unable to create stderr temporary file {stderr_temp}: {error}") from error

    observed_wall = time.monotonic() - started
    if attempted and not timed_out and not interrupted and exit_code != 0:
        if exit_code is not None and exit_code < 0:
            failure_kind = "terminated_by_signal"
            failure_message = f"process terminated by signal {-exit_code}"
        else:
            failure_kind = "nonzero_exit"
            failure_message = f"process exited with code {exit_code}"
    return ProcessOutcome(
        attempted=attempted,
        exit_code=exit_code,
        timed_out=timed_out,
        failure_kind=failure_kind,
        failure_message=failure_message,
        observed_wall_seconds=observed_wall,
        interrupted=interrupted,
    )


def synthetic_stderr_temp(raw_dir: Path, stem: str, messages: list[str]) -> Path:
    """Create raw stderr for a preflight failure that never launched minimap2."""

    data = "".join(f"MM_RUNNER_ERROR\t{message}\n" for message in messages).encode("utf-8")
    return write_private_temp(raw_dir, f"{stem}.stderr.log", data)


def record_status(process: ProcessOutcome, validation: dict[str, Any]) -> str:
    """Classify execution failures independently of endpoint eligibility."""

    if not process.attempted:
        return "failed_preflight_or_launch"
    if process.interrupted:
        return "failed_interrupted"
    if process.timed_out:
        return "failed_timeout"
    if process.exit_code != 0:
        return "failed_process"
    if not validation["all_required_checks_pass"]:
        return "completed_with_validation_errors"
    return "completed"


def explicit_index_options(arguments: tuple[str, ...]) -> dict[str, Any]:
    """Track explicit k/w values unless a later preset makes them unknowable."""

    expected: dict[str, Any] = {"k": None, "w": None, "is_hpc": False}
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "-x" or (argument.startswith("-x") and len(argument) > 2):
            expected = {"k": None, "w": None, "is_hpc": None}
            index += 2 if argument == "-x" else 1
            continue
        if argument == "-H":
            expected["is_hpc"] = True
        for option, field in (("-k", "k"), ("-w", "w")):
            raw_value: str | None = None
            if argument == option and index + 1 < len(arguments):
                raw_value = arguments[index + 1]
                index += 1
            elif argument.startswith(option) and len(argument) > len(option):
                raw_value = argument[len(option) :]
            if raw_value is not None:
                try:
                    expected[field] = int(raw_value)
                except ValueError:
                    expected[field] = None
        index += 1
    return expected


def execute_run(
    spec: RunSpec,
    *,
    manifest_path: Path,
    manifest_sha256: str,
    minimap2: Path,
    minimap2_sha256: str,
    runner_sha256: str,
    execution_environment: dict[str, Any],
    shutdown_requested: threading.Event,
    active_process_groups: ActiveProcessGroups,
    raw_dir: Path,
    shard_dir: Path,
) -> dict[str, Any]:
    """Execute one run and publish its immutable stderr and JSON shard."""

    if shutdown_requested.is_set():
        raise RunCancelled(f"run {spec.run_id} cancelled before launch")
    stem = artifact_stem(spec.run_id)
    stderr_path = raw_dir / f"{stem}.stderr.log"
    shard_path = shard_dir / f"{stem}.json"
    if stderr_path.exists() or shard_path.exists():
        raise RuntimeError(
            f"pending run {spec.run_id} already has an artifact; refusing to overwrite immutable data"
        )

    inputs, input_errors = dataset_inputs(spec, manifest_path)
    reference_path = Path(inputs["reference"].get("resolved_path") or "")
    query_path = Path(inputs["query"].get("resolved_path") or "")
    injected_args = ["-t", "1", "--dbg-seed-occ"]
    mapping_argv = [
        str(minimap2.resolve()),
        *spec.default_args,
        *spec.arm_args,
        *injected_args,
        str(reference_path),
        str(query_path),
    ]
    time_format = (
        "MM_TIME\twall_seconds=%e\tuser_seconds=%U\tsystem_seconds=%S"
        "\tmax_rss_kib=%M\texit_status=%x"
    )
    timed_argv = [str(TIME_BINARY), "-f", time_format, *mapping_argv]
    environment = os.environ.copy()
    environment["MM_RUNL_EXPERIMENT"] = "1"
    environment["LC_ALL"] = "C"
    environment.pop("MM_RUNL_DUMP_DIR", None)

    started_at = utc_now()
    if input_errors:
        stderr_temp = synthetic_stderr_temp(raw_dir, stem, input_errors)
        process = ProcessOutcome(
            attempted=False,
            exit_code=None,
            timed_out=False,
            failure_kind="input_validation",
            failure_message="; ".join(input_errors),
            observed_wall_seconds=0.0,
        )
    else:
        stderr_temp = raw_dir / (
            f".{stem}.stderr.log.tmp.{os.getpid()}.{threading.get_ident()}.{uuid.uuid4().hex}"
        )
        process = run_process(
            timed_argv,
            environment,
            spec.timeout_seconds,
            stderr_temp,
            shutdown_requested=shutdown_requested,
            active_process_groups=active_process_groups,
        )
    completed_at = utc_now()

    stderr_bytes = stderr_temp.read_bytes()
    stderr_text = stderr_bytes.decode("utf-8", errors="replace")
    stderr_hash = hashlib.sha256(stderr_bytes).hexdigest()

    parsed = parse_stderr(stderr_text)
    validation = telemetry_validation(
        parsed,
        process,
        expected_query_length=int(spec.dataset["query_length"]),
        expected_reference_length=int(spec.dataset["reference_length"]),
        expected_index_options=explicit_index_options(spec.default_args + spec.arm_args),
    )
    validation["input_errors"] = input_errors
    validation["input_hashes_pass"] = not input_errors
    if input_errors:
        validation["all_required_checks_pass"] = False
        validation["all_errors"] = input_errors + validation["all_errors"]
    aggregate = aggregate_records(parsed["runl_records"])
    decorated_records = [decorate_record(record) for record in parsed["runl_records"]]
    endpoint = endpoint_eligibility(parsed["runl_records"])
    status = record_status(process, validation)

    dataset = spec.dataset
    reference = spec.reference
    run = {
        "run_id": spec.run_id,
        "status": status,
        "arm": spec.arm_id,
        "arm_id": spec.arm_id,
        "dataset_id": spec.dataset_id,
        "replicate_id": spec.replicate_id,
        "series_memberships": [
            {"series": "occurrence_count", "value": spec.occurrence_count}
        ],
        "parameters": {
            "occurrence_count": spec.occurrence_count,
            "mapping_threads": 1,
            "manifest_default_args": list(spec.default_args),
            "manifest_arm_args": list(spec.arm_args),
            "runner_injected_args": injected_args,
            "timeout_seconds": spec.timeout_seconds,
            "k": parsed["index_diagnostics"].get("k"),
            "w": parsed["index_diagnostics"].get("w"),
        },
        "dataset": {
            "dataset_id": spec.dataset_id,
            "arm_id": spec.arm_id,
            "replicate_id": spec.replicate_id,
            "master_seed": dataset.get("master_seed"),
            "derived_seeds": dataset.get("seeds"),
            "reference_length": dataset.get("reference_length"),
            "source_length": dataset.get("source_length"),
            "query_length": dataset.get("query_length"),
            "cell_width": dataset.get("cell_width"),
            "minimum_full_grid_copy_gap": dataset.get("minimum_full_grid_copy_gap"),
            "intended_occurrences": reference.get("intended_occurrences"),
            "verified_exact_source_occurrences_overlapping": reference.get(
                "verified_exact_source_occurrences_overlapping"
            ),
            "minimum_copy_gap": reference.get("minimum_copy_gap"),
            "copy_fraction": reference.get("copy_fraction"),
            "intended_positions_sha256": reference.get("intended_positions_sha256"),
            "mutation_model": dataset.get("mutation_model"),
            "realized_mutations": dataset.get("realized_edits"),
            "inputs": inputs,
        },
        "argv": mapping_argv,
        "argv_shell_quoted_for_display": shlex.join(mapping_argv),
        "timed_argv": timed_argv,
        "started_at_utc": started_at,
        "completed_at_utc": completed_at,
        "process": {
            "attempted": process.attempted,
            "exit_code": process.exit_code,
            "timed_out": process.timed_out,
            "interrupted": process.interrupted,
            "failure_kind": process.failure_kind,
            "failure_message": process.failure_message,
        },
        "exit_code": process.exit_code,
        "timing": parsed["timing"] or {
            "wrapper_observed_wall_seconds": process.observed_wall_seconds
        },
        "wrapper_observed_wall_seconds": process.observed_wall_seconds,
        "stderr_path": display_path(stderr_path),
        "stderr_sha256": stderr_hash,
        "map_diagnostics": parsed["map_records"],
        "seed_frequency_records": parsed["initial_sf_records"],
        "seed_frequency_summary": parsed["sf_summary"],
        "seed_frequency_all_passes_records": parsed["sf_records"],
        "seed_frequency_all_passes_summary": parsed["sf_all_passes_summary"],
        "post_map_rechain_seed_frequency_records": parsed[
            "post_map_rechain_sf_records"
        ],
        "post_map_rechain_seed_frequency_summary": parsed[
            "post_map_rechain_sf_summary"
        ],
        "index_diagnostics": parsed["index_diagnostics"],
        "backtrack_records": decorated_records,
        "summary": aggregate,
        "score_gate_eligibility": endpoint,
        "validation": validation,
        "provenance": {
            "manifest_sha256": manifest_sha256,
            "run_spec_sha256": json_digest(spec_payload(spec)),
            "minimap2_binary_sha256": minimap2_sha256,
            "runner_sha256": runner_sha256,
            "instrumentation_environment": {"MM_RUNL_EXPERIMENT": "1"},
            "seed_frequency_flag": "--dbg-seed-occ",
            "mapping_threads": 1,
            "execution_environment": execution_environment,
        },
    }
    shard = {
        "shard_schema_version": 1,
        "shard_state": "complete",
        "run_id": spec.run_id,
        "run_spec_sha256": json_digest(spec_payload(spec)),
        "created_at_utc": completed_at,
        "stderr_recovery": {
            "encoding": "base64",
            "sha256": stderr_hash,
            "data": base64.b64encode(stderr_bytes).decode("ascii"),
        },
        "run": run,
    }
    write_immutable_json(shard_path, shard)
    # The shard is the commit marker.  It carries exact recovery bytes so a
    # crash between these two publications cannot permanently orphan a run.
    publish_temp_immutable(stderr_temp, stderr_path)
    return {
        "run_id": spec.run_id,
        "status": status,
        "shard_path": shard_path,
        "stderr_path": stderr_path,
        "wall_seconds": process.observed_wall_seconds,
        "endpoint": endpoint,
    }


def load_completed_shard(
    spec: RunSpec,
    shard_dir: Path,
    *,
    manifest_sha256: str,
    minimap2_sha256: str,
    runner_sha256: str,
) -> tuple[dict[str, Any], Path] | None:
    """Load and validate the immutable completion marker for one run."""

    path = shard_dir / f"{artifact_stem(spec.run_id)}.json"
    if not path.exists():
        return None
    try:
        shard = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"invalid shard {path}: {error}") from error
    expected_digest = json_digest(spec_payload(spec))
    if (
        not isinstance(shard, dict)
        or shard.get("shard_schema_version") != 1
        or shard.get("shard_state") != "complete"
    ):
        raise RuntimeError(f"shard is not a completed run artifact: {path}")
    if shard.get("run_id") != spec.run_id:
        raise RuntimeError(f"shard run_id mismatch: {path}")
    if shard.get("run_spec_sha256") != expected_digest:
        raise RuntimeError(
            f"shard spec mismatch for {spec.run_id}; use a new shard/raw/output directory for the changed manifest"
        )
    run = shard.get("run")
    if not isinstance(run, dict):
        raise RuntimeError(f"shard has no run object: {path}")
    if run.get("run_id") != spec.run_id:
        raise RuntimeError(f"nested shard run_id mismatch: {path}")
    run_provenance = run.get("provenance")
    if not isinstance(run_provenance, dict):
        raise RuntimeError(f"shard has no provenance object: {path}")
    if run_provenance.get("run_spec_sha256") != expected_digest:
        raise RuntimeError(f"nested shard run spec mismatch: {path}")
    expected_provenance = {
        "manifest_sha256": manifest_sha256,
        "minimap2_binary_sha256": minimap2_sha256,
        "runner_sha256": runner_sha256,
    }
    for key, expected in expected_provenance.items():
        if run_provenance.get(key) != expected:
            raise RuntimeError(
                f"shard {key} mismatch for {spec.run_id}; use new artifact directories for changed inputs"
            )
    stderr_value = run.get("stderr_path")
    if not isinstance(stderr_value, str):
        raise RuntimeError(f"shard has no stderr_path: {path}")
    stderr_path = Path(stderr_value)
    if not stderr_path.is_absolute():
        stderr_path = ROOT / stderr_path
    if not stderr_path.exists():
        recovery = shard.get("stderr_recovery")
        if not isinstance(recovery, dict) or recovery.get("encoding") != "base64":
            raise RuntimeError(f"shard stderr artifact is missing: {stderr_path}")
        try:
            recovered_bytes = base64.b64decode(recovery.get("data", ""), validate=True)
        except (ValueError, binascii.Error) as error:
            raise RuntimeError(f"invalid stderr recovery payload in {path}: {error}") from error
        recovered_hash = hashlib.sha256(recovered_bytes).hexdigest()
        if recovered_hash != recovery.get("sha256") or recovered_hash != run.get("stderr_sha256"):
            raise RuntimeError(f"stderr recovery hash mismatch in shard: {path}")
        temporary = write_private_temp(stderr_path.parent, stderr_path.name, recovered_bytes)
        publish_temp_immutable(temporary, stderr_path)
    if not stderr_path.is_file():
        raise RuntimeError(f"shard stderr artifact is not a regular file: {stderr_path}")
    if sha256(stderr_path) != run.get("stderr_sha256"):
        raise RuntimeError(f"shard stderr hash mismatch: {stderr_path}")
    return shard, path


def memory_snapshot() -> dict[str, Any]:
    """Read global MemAvailable and swap counters from procfs."""

    sample: dict[str, Any] = {"timestamp_utc": utc_now()}
    wanted = {"MemTotal", "MemAvailable", "SwapTotal", "SwapFree"}
    try:
        for line in Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
            key, raw_value = line.split(":", 1)
            if key in wanted:
                sample[f"{key.lower()}_kib"] = int(raw_value.strip().split()[0])
    except (OSError, ValueError) as error:
        sample["read_error"] = str(error)
    if "swaptotal_kib" in sample and "swapfree_kib" in sample:
        sample["swap_used_kib"] = sample["swaptotal_kib"] - sample["swapfree_kib"]
    return sample


def monitor_resources(stop: threading.Event, samples: list[dict[str, Any]]) -> None:
    """Sample global memory and swap at the predeclared 0.5-second cadence."""

    while True:
        samples.append(memory_snapshot())
        if stop.wait(RESOURCE_SAMPLE_INTERVAL_SECONDS):
            return


def resource_summary(samples: list[dict[str, Any]]) -> dict[str, Any]:
    """Summarize resource samples while retaining the full time series."""

    available = [
        int(sample["memavailable_kib"])
        for sample in samples
        if isinstance(sample.get("memavailable_kib"), int)
    ]
    swap_used = [
        int(sample["swap_used_kib"])
        for sample in samples
        if isinstance(sample.get("swap_used_kib"), int)
    ]
    return {
        "sample_interval_seconds": RESOURCE_SAMPLE_INTERVAL_SECONDS,
        "sample_count": len(samples),
        "minimum_mem_available_kib": min(available) if available else None,
        "starting_mem_available_kib": available[0] if available else None,
        "ending_mem_available_kib": available[-1] if available else None,
        "starting_swap_used_kib": swap_used[0] if swap_used else None,
        "maximum_swap_used_kib": max(swap_used) if swap_used else None,
        "ending_swap_used_kib": swap_used[-1] if swap_used else None,
        "maximum_swap_growth_kib": (
            max(value - swap_used[0] for value in swap_used) if swap_used else None
        ),
    }


def write_resource_invocation(
    shard_dir: Path,
    *,
    invocation_id: str,
    manifest_sha256: str,
    started_at: str,
    completed_at: str,
    requested_jobs: int,
    effective_jobs: int,
    resource_safety_policy: dict[str, Any],
    interrupted_by_signal: str | None,
    cancelled_run_ids: list[str],
    worker_errors: list[str],
    runner_sha256: str,
    minimap2_sha256: str,
    execution_environment_sha256: str,
    filters: dict[str, Any],
    selected_run_ids: list[str],
    pending_run_ids: list[str],
    samples: list[dict[str, Any]],
) -> Path:
    """Persist global resource monitoring as an immutable invocation shard."""

    path = shard_dir / "_invocations" / f"{invocation_id}.json"
    value = {
        "resource_schema_version": 1,
        "invocation_id": invocation_id,
        "manifest_sha256": manifest_sha256,
        "started_at_utc": started_at,
        "completed_at_utc": completed_at,
        "requested_parallel_processes": requested_jobs,
        "parallel_processes": effective_jobs,
        "resource_safety_policy": resource_safety_policy,
        "interrupted_by_signal": interrupted_by_signal,
        "cancelled_run_ids": cancelled_run_ids,
        "worker_errors": worker_errors,
        "runner_sha256": runner_sha256,
        "minimap2_sha256": minimap2_sha256,
        "execution_environment_sha256": execution_environment_sha256,
        "filters": filters,
        "selected_run_ids": selected_run_ids,
        "pending_run_ids_at_start": pending_run_ids,
        "summary": resource_summary(samples),
        "samples": samples,
    }
    write_immutable_json(path, value)
    return path


def command_result(argv: list[str]) -> dict[str, Any]:
    """Capture a small provenance command without making experiment execution depend on it."""

    try:
        result = subprocess.run(
            argv,
            cwd=ROOT,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=20,
        )
        return {
            "argv": argv,
            "exit_code": result.returncode,
            "output": result.stdout.strip(),
        }
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"argv": argv, "exit_code": None, "output": None, "error": str(error)}


def cpu_model() -> str:
    """Return the first Linux CPU model description, if available."""

    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return "unknown"


def file_hashes(paths: list[Path]) -> dict[str, str | None]:
    """Hash available instrumentation sources for consolidated provenance."""

    result: dict[str, str | None] = {}
    for path in paths:
        try:
            result[display_path(path)] = sha256(path) if path.is_file() else None
        except OSError:
            result[display_path(path)] = None
    return result


def capture_execution_environment(minimap2: Path) -> dict[str, Any]:
    """Snapshot execution-time source, tool, Python, and host provenance."""

    return {
        "captured_at_utc": utc_now(),
        "minimap2_version": command_result([str(minimap2), "--version"]),
        "time_binary": display_path(TIME_BINARY),
        "time_binary_sha256": sha256(TIME_BINARY),
        "time_version": command_result([str(TIME_BINARY), "--version"]),
        "instrumentation_source_sha256": file_hashes(
            [ROOT / "lchain.c", ROOT / "map.c", ROOT / "seed.c", ROOT / "index.c", ROOT / "main.c"]
        ),
        "python": {"executable": sys.executable, "version": sys.version},
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "cpu_model": cpu_model(),
            "logical_cpu_count": os.cpu_count(),
        },
    }


def load_resource_invocations(
    shard_dir: Path,
    manifest_sha256: str,
    runner_sha256: str,
    minimap2_sha256: str,
) -> list[dict[str, Any]]:
    """Load this manifest's immutable resource shards in chronological order."""

    directory = shard_dir / "_invocations"
    if not directory.exists():
        return []
    values: list[dict[str, Any]] = []
    for path in sorted(directory.glob("*.json")):
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise RuntimeError(f"invalid resource invocation shard {path}: {error}") from error
        if not isinstance(value, dict):
            raise RuntimeError(f"resource invocation shard is not an object: {path}")
        if (
            value.get("manifest_sha256") != manifest_sha256
            or value.get("runner_sha256") != runner_sha256
            or value.get("minimap2_sha256") != minimap2_sha256
        ):
            continue
        value["artifact_path"] = display_path(path)
        value["artifact_sha256"] = sha256(path)
        values.append(value)
    return values


def consolidate(
    *,
    output: Path,
    manifest_path: Path,
    manifest: dict[str, Any],
    manifest_sha256: str,
    normalized_mapping: dict[str, Any],
    specs: list[RunSpec],
    shard_dir: Path,
    minimap2: Path,
    runner_sha256: str,
    minimap2_sha256: str,
    filters: dict[str, Any],
    resource_safety_policy: dict[str, Any],
) -> dict[str, Any]:
    """Rebuild the complete YAML atomically from immutable run shards."""

    runs: list[dict[str, Any]] = []
    missing_run_ids: list[str] = []
    for spec in specs:
        loaded = load_completed_shard(
            spec,
            shard_dir,
            manifest_sha256=manifest_sha256,
            minimap2_sha256=minimap2_sha256,
            runner_sha256=runner_sha256,
        )
        if loaded is None:
            missing_run_ids.append(spec.run_id)
            continue
        shard, path = loaded
        run = dict(shard["run"])
        run["shard_path"] = display_path(path)
        run["shard_sha256"] = sha256(path)
        runs.append(run)

    failed_runs = [run for run in runs if run.get("status") != "completed"]
    validation_errors = [
        {"run_id": run["run_id"], "errors": run.get("validation", {}).get("all_errors", [])}
        for run in runs
        if run.get("validation", {}).get("all_errors")
    ]
    if missing_run_ids:
        experiment_status = "partial"
        completed_at: str | None = None
    elif failed_runs:
        experiment_status = "complete_with_failures"
        completed_at = utc_now()
    else:
        experiment_status = "complete"
        completed_at = utc_now()

    measurement_environment_digests = sorted(
        {
            json_digest(run["provenance"]["execution_environment"])
            for run in runs
            if isinstance(run.get("provenance", {}).get("execution_environment"), dict)
        }
    )
    provenance = {
        "manifest_path": display_path(manifest_path),
        "manifest_sha256": manifest_sha256,
        "runner_path": display_path(Path(__file__)),
        "runner_sha256": runner_sha256,
        "minimap2_path": display_path(minimap2),
        "minimap2_binary_sha256": minimap2_sha256,
        "measurement_execution_environment_sha256": measurement_environment_digests,
        "consolidation_time_state": {
            "captured_at_utc": utc_now(),
            "minimap2_version": command_result([str(minimap2), "--version"]),
            "time_binary": display_path(TIME_BINARY),
            "time_version": command_result([str(TIME_BINARY), "--version"]),
            "instrumentation_source_sha256": file_hashes(
                [ROOT / "lchain.c", ROOT / "map.c", ROOT / "seed.c", ROOT / "index.c", ROOT / "main.c"]
            ),
            "python": {"executable": sys.executable, "version": sys.version},
            "host": {
                "platform": platform.platform(),
                "machine": platform.machine(),
                "cpu_model": cpu_model(),
                "logical_cpu_count": os.cpu_count(),
            },
        },
        "mapping_threads": 1,
        "required_environment": {"MM_RUNL_EXPERIMENT": "1", "LC_ALL": "C"},
        "required_argument": "--dbg-seed-occ",
    }
    result = {
        "schema_version": 2,
        "experiment": {
            "title": "Manifest-driven minimap2 first-path Run_L follow-up",
            "status": experiment_status,
            "consolidated_at_utc": utc_now(),
            "completed_at_utc": completed_at,
            "first_path_predicate": {
                "name": "Run_L",
                "headline_latex": (
                    r"\operatorname{Run}_L(j,c)\iff j\geq L-1\land c\leq n-L\land "
                    r"\bigwedge_{u=0}^{L-1}p_{j-u}=j-u-1"
                ),
                "scope": (
                    "Evaluated only along minimap2's score/index-ranked first predecessor path; "
                    "it is not an endpoint eligibility test."
                ),
                "measured_lane_widths": list(LANE_WIDTHS),
            },
            "score_gate_eligibility": {
                "definition": (
                    "candidate_end_count counts predecessor endpoints satisfying the score gate "
                    "f[i] >= min_score. It does not include minimap2's later minimum-anchor-count "
                    "gate. Absent score-qualified candidates are reported separately, and all "
                    "Run_L rates with zero denominators remain null."
                ),
                "score_gate_only": True,
                "minimum_count_gate_included": False,
                "not_equivalent_to_predicate_failure": True,
            },
            "planned_run_count": len(specs),
            "completed_shard_count": len(runs),
            "missing_run_count": len(missing_run_ids),
            "failed_run_count": len(failed_runs),
            "last_invocation_filters": filters,
            "last_invocation_resource_safety_policy": resource_safety_policy,
        },
        "provenance": provenance,
        "mapping": normalized_mapping,
        "workload_manifest": manifest,
        "arms": {
            arm["arm_id"]: {key: value for key, value in arm.items() if key != "arm_id"}
            for arm in normalized_mapping["arms"]
        },
        "runs": runs,
        "run_inventory": {
            "planned_run_ids": [spec.run_id for spec in specs],
            "completed_run_ids": [run["run_id"] for run in runs],
            "missing_run_ids": missing_run_ids,
            "failed_run_ids": [run["run_id"] for run in failed_runs],
        },
        "validations": {
            "all_completed_runs_pass_required_checks": (
                bool(runs)
                and all(
                    run.get("validation", {}).get("all_required_checks_pass") is True
                    for run in runs
                )
            ),
            "all_observed_run_l_counter_identities_pass": (
                bool(runs)
                and all(
                    run.get("validation", {}).get("run_l_counter_identities_pass") in {True, None}
                    for run in runs
                )
            ),
            "runs_with_validation_errors": validation_errors,
        },
        "resource_monitoring": load_resource_invocations(
            shard_dir, manifest_sha256, runner_sha256, minimap2_sha256
        ),
    }
    atomic_write_yaml(output, result)
    return result


def selected_specs(
    specs: list[RunSpec],
    *,
    only_n: set[int] | None,
    only_replicates: set[str] | None,
    only_arms: set[str] | None,
) -> list[RunSpec]:
    """Apply independent pilot filters without changing the registered matrix."""

    return [
        spec
        for spec in specs
        if (only_n is None or spec.occurrence_count in only_n)
        and (only_replicates is None or spec.replicate_id in only_replicates)
        and (only_arms is None or spec.arm_id in only_arms)
    ]


def parse_args() -> argparse.Namespace:
    """Parse the runner CLI."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--raw-dir", type=Path, default=DEFAULT_RAW_DIR)
    parser.add_argument("--shard-dir", type=Path, default=DEFAULT_SHARD_DIR)
    parser.add_argument("--minimap2", type=Path, default=DEFAULT_MINIMAP2)
    parser.add_argument(
        "--jobs",
        type=bounded_jobs,
        default=min(4, os.cpu_count() or 1),
        help=(
            "requested process ceiling (1-4); this uncalibrated pilot runner records the "
            "request but forces one process for resource safety"
        ),
    )
    parser.add_argument(
        "--only-N",
        "--only-n",
        dest="only_n",
        action="append",
        help="run only this N (repeat the option or provide comma-separated values)",
    )
    parser.add_argument(
        "--only-replicate",
        action="append",
        help="run only this replicate_id (repeat or comma-separate)",
    )
    parser.add_argument(
        "--only-arm",
        action="append",
        help="run only this arm_id (repeat or comma-separate)",
    )
    return parser.parse_args()


def main() -> int:
    """Run pending filtered cases, persist monitoring, and consolidate results."""

    args = parse_args()
    manifest_path = args.manifest.resolve()
    output = args.output.resolve()
    raw_dir = args.raw_dir.resolve()
    shard_dir = args.shard_dir.resolve()
    minimap2 = args.minimap2.resolve()
    try:
        manifest_bytes = manifest_path.read_bytes()
        manifest_text = manifest_bytes.decode("utf-8")
        manifest = json.loads(manifest_text)
        if not isinstance(manifest, dict):
            raise ValueError("manifest root must be a JSON object")
        specs, normalized_mapping = build_specs(manifest)
        only_n = n_filter(args.only_n)
        only_replicates = comma_values(args.only_replicate)
        only_arms = comma_values(args.only_arm)
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
        print(f"manifest or argument error: {error}", file=sys.stderr)
        return 2

    unknown_replicates = (
        set() if only_replicates is None else only_replicates - {spec.replicate_id for spec in specs}
    )
    unknown_arms = set() if only_arms is None else only_arms - {spec.arm_id for spec in specs}
    unknown_n = set() if only_n is None else only_n - {spec.occurrence_count for spec in specs}
    if unknown_replicates or unknown_arms or unknown_n:
        print(
            "filter selects undeclared values: "
            f"N={sorted(unknown_n)} replicates={sorted(unknown_replicates)} arms={sorted(unknown_arms)}",
            file=sys.stderr,
        )
        return 2

    filtered = selected_specs(
        specs,
        only_n=only_n,
        only_replicates=only_replicates,
        only_arms=only_arms,
    )
    if not filtered:
        print("the combined filters select zero manifest runs", file=sys.stderr)
        return 2

    if not minimap2.is_file():
        print(f"instrumented minimap2 binary not found: {minimap2}", file=sys.stderr)
        return 2
    if not TIME_BINARY.is_file():
        print(f"GNU time binary not found: {TIME_BINARY}", file=sys.stderr)
        return 2

    manifest_hash = hashlib.sha256(manifest_bytes).hexdigest()
    runner_hash = sha256(Path(__file__))
    minimap2_hash = sha256(minimap2)
    execution_environment = capture_execution_environment(minimap2)
    raw_dir.mkdir(parents=True, exist_ok=True)
    shard_dir.mkdir(parents=True, exist_ok=True)

    try:
        completed = {
            spec.run_id
            for spec in specs
            if load_completed_shard(
                spec,
                shard_dir,
                manifest_sha256=manifest_hash,
                minimap2_sha256=minimap2_hash,
                runner_sha256=runner_hash,
            )
            is not None
        }
    except RuntimeError as error:
        print(f"resume validation error: {error}", file=sys.stderr)
        return 2
    pending = [spec for spec in filtered if spec.run_id not in completed]
    requested_jobs = args.jobs
    jobs = 1
    resource_safety_policy = {
        "mode": "forced_serial",
        "requested_jobs": requested_jobs,
        "effective_jobs": jobs,
        "reason": (
            "No per-process peak-RSS calibration is available before this pilot; serial "
            "submission prevents unsafe concurrent launches while global memory/swap is sampled."
        ),
        "queued_futures": 0,
        "active_launch_thresholds": None,
    }
    filters = {
        "only_N": sorted(only_n) if only_n is not None else None,
        "only_replicate": sorted(only_replicates) if only_replicates is not None else None,
        "only_arm": sorted(only_arms) if only_arms is not None else None,
    }
    invocation_started = utc_now()
    invocation_id = (
        "invocation_"
        + invocation_started.replace("-", "").replace(":", "").replace(".", "")
        + f"_{os.getpid()}_{uuid.uuid4().hex[:10]}"
    )
    print(
        f"{invocation_started} selected={len(filtered)} pending={len(pending)} "
        f"resumed={len(filtered) - len(pending)} requested_jobs={requested_jobs} "
        f"effective_jobs={jobs} resource_policy=forced_serial",
        flush=True,
    )

    samples: list[dict[str, Any]] = []
    stop_monitor = threading.Event()
    shutdown_requested = threading.Event()
    active_process_groups = ActiveProcessGroups()
    monitor: threading.Thread | None = None
    worker_errors: list[str] = []
    cancelled_run_ids: list[str] = []
    received_signal_number: int | None = None
    newly_failed = False
    if pending:
        previous_signal_handlers: dict[int, Any] = {}

        def handle_termination_signal(signal_number: int, _frame: Any) -> None:
            """Request bounded shutdown; a second signal escalates immediately."""

            nonlocal received_signal_number
            if received_signal_number is None:
                received_signal_number = signal_number
                shutdown_requested.set()
                active_process_groups.terminate_all(signal.SIGTERM)
            else:
                active_process_groups.terminate_all(signal.SIGKILL)

        for signal_number in (signal.SIGINT, signal.SIGTERM):
            previous_signal_handlers[signal_number] = signal.signal(
                signal_number, handle_termination_signal
            )
        monitor = threading.Thread(
            target=monitor_resources,
            args=(stop_monitor, samples),
            name="runl-global-resource-monitor",
            daemon=True,
        )
        monitor.start()
        try:
            for index, spec in enumerate(pending, 1):
                if shutdown_requested.is_set():
                    cancelled_run_ids.extend(run.run_id for run in pending[index - 1 :])
                    break
                try:
                    result = execute_run(
                        spec,
                        manifest_path=manifest_path,
                        manifest_sha256=manifest_hash,
                        minimap2=minimap2,
                        minimap2_sha256=minimap2_hash,
                        runner_sha256=runner_hash,
                        execution_environment=execution_environment,
                        shutdown_requested=shutdown_requested,
                        active_process_groups=active_process_groups,
                        raw_dir=raw_dir,
                        shard_dir=shard_dir,
                    )
                except RunCancelled:
                    cancelled_run_ids.append(spec.run_id)
                    cancelled_run_ids.extend(run.run_id for run in pending[index:])
                    break
                except Exception as error:  # preserve completed shards, then consolidate
                    worker_errors.append(f"{spec.run_id}: {type(error).__name__}: {error}")
                    newly_failed = True
                    print(
                        f"{utc_now()} [{index}/{len(pending)}] {spec.run_id} runner_error={error}",
                        file=sys.stderr,
                        flush=True,
                    )
                    if shutdown_requested.is_set():
                        cancelled_run_ids.extend(run.run_id for run in pending[index:])
                        break
                    continue
                newly_failed = newly_failed or result["status"] != "completed"
                endpoint = result["endpoint"].get("status")
                current_memory = memory_snapshot()
                available = current_memory.get("memavailable_kib")
                swap_used = current_memory.get("swap_used_kib")
                available_mib = None if available is None else round(available / 1024, 1)
                swap_mib = None if swap_used is None else round(swap_used / 1024, 1)
                print(
                    f"{utc_now()} [{index}/{len(pending)}] {spec.run_id} "
                    f"status={result['status']} endpoint={endpoint} "
                    f"wall={result['wall_seconds']:.3f}s "
                    f"mem_available_mib={available_mib} swap_used_mib={swap_mib}",
                    flush=True,
                )
                if shutdown_requested.is_set():
                    cancelled_run_ids.extend(run.run_id for run in pending[index:])
                    break
        finally:
            active_process_groups.terminate_all(signal.SIGTERM)
            stop_monitor.set()
            if monitor is not None:
                monitor.join()
            samples.append(memory_snapshot())
            for signal_number, previous_handler in previous_signal_handlers.items():
                signal.signal(signal_number, previous_handler)

    invocation_completed = utc_now()
    received_signal_name = (
        signal.Signals(received_signal_number).name
        if received_signal_number is not None
        else None
    )
    if pending:
        try:
            write_resource_invocation(
                shard_dir,
                invocation_id=invocation_id,
                manifest_sha256=manifest_hash,
                started_at=invocation_started,
                completed_at=invocation_completed,
                requested_jobs=requested_jobs,
                effective_jobs=jobs,
                resource_safety_policy=resource_safety_policy,
                interrupted_by_signal=received_signal_name,
                cancelled_run_ids=cancelled_run_ids,
                worker_errors=worker_errors,
                runner_sha256=runner_hash,
                minimap2_sha256=minimap2_hash,
                execution_environment_sha256=json_digest(execution_environment),
                filters=filters,
                selected_run_ids=[spec.run_id for spec in filtered],
                pending_run_ids=[spec.run_id for spec in pending],
                samples=samples,
            )
        except Exception as error:
            worker_errors.append(f"resource monitoring artifact: {type(error).__name__}: {error}")
            newly_failed = True

    try:
        result = consolidate(
            output=output,
            manifest_path=manifest_path,
            manifest=manifest,
            manifest_sha256=manifest_hash,
            normalized_mapping=normalized_mapping,
            specs=specs,
            shard_dir=shard_dir,
            minimap2=minimap2,
            runner_sha256=runner_hash,
            minimap2_sha256=minimap2_hash,
            filters=filters,
            resource_safety_policy=resource_safety_policy,
        )
    except Exception as error:
        print(f"consolidation failed: {type(error).__name__}: {error}", file=sys.stderr)
        return 1

    if worker_errors:
        print("runner errors:", file=sys.stderr)
        for error in worker_errors:
            print(f"  {error}", file=sys.stderr)
    print(
        f"{utc_now()} done status={result['experiment']['status']} output={output}",
        flush=True,
    )
    selected_run_ids = {spec.run_id for spec in filtered}
    selected_failures = [
        run
        for run in result["runs"]
        if run.get("run_id") in selected_run_ids and run.get("status") != "completed"
    ]
    if received_signal_number is not None:
        return 128 + received_signal_number
    return 1 if newly_failed or selected_failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
