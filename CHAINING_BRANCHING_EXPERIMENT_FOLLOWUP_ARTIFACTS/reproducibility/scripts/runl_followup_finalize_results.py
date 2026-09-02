#!/usr/bin/env python3
"""Merge and plot the follow-up minimap2 Run_L experiment.

The script consumes schema-v2 result files produced by
``runl_followup_run_experiment.py``.  A schema-v1 result file from the original
experiment may be supplied as context.  Every plotted observation is one
generated query/reference replicate.  Anchor counts are never pooled and an
undefined rate is emitted as YAML null and an empty CSV cell.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import math
import sys
import tempfile
import textwrap
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Sequence

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import yaml
from matplotlib.transforms import ScaledTranslation


SCRIPT_PATH = Path(__file__).resolve()
LANES = (4, 8, 16)
DEFAULT_FILTER_FRACTION = 0.0002
DEFAULT_SCORE_THRESHOLD = 40
PLOT_FORMATS = ("pdf", "png", "svg")
THESIS_TEXT_WIDTH_IN = 6.5

# Keep the vector output editable and readable when it is included at thesis
# text width.  Type 42 embeds TrueType glyphs instead of Type 3 bitmap fonts.
matplotlib.rcParams.update(
    {
        "font.size": 8.0,
        "axes.titlesize": 9.0,
        "axes.labelsize": 8.5,
        "xtick.labelsize": 7.5,
        "ytick.labelsize": 7.5,
        "legend.fontsize": 7.0,
        "figure.titlesize": 10.0,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
        "svg.fonttype": "none",
    }
)

COHORT_MARKERS = ("o", "s", "^", "D", "v", "P", "X", "*")
ENDPOINT_MARKER_OFFSET_POINTS = 5.0
DATA_MARKER_SIZE_POINTS = 4.2
DATA_MARKER_EDGE_WIDTH_POINTS = 0.9
ERRORBAR_CAP_SIZE_POINTS = 2.0
ERRORBAR_LINE_WIDTH_POINTS = 0.8
ENDPOINT_CLIP_SAFETY_POINTS = 1.0
ENDPOINT_ARTIST_HALF_WIDTH_POINTS = max(
    DATA_MARKER_SIZE_POINTS / 2.0 + DATA_MARKER_EDGE_WIDTH_POINTS / 2.0,
    ERRORBAR_CAP_SIZE_POINTS + ERRORBAR_LINE_WIDTH_POINTS / 2.0,
) + ENDPOINT_CLIP_SAFETY_POINTS

FRIENDLY_ARMS = {
    "dense_default_filter_4p5m_1kb_holdout": "Dense 4.5 Mb / 1 kb, holdout",
    "dense_default_filter_4p5m_1kb": "Dense 4.5 Mb / 1 kb, pilot",
    "larger_target_only_40m_1kb": "40 Mb / 1 kb, larger-target control",
    "proportional_20m_2kb": "20 Mb / 2 kb, proportional control",
    "proportional_40m_4kb": "40 Mb / 4 kb, proportional control",
    "proportional_80m_8kb": "80 Mb / 8 kb, proportional control",
    "requested_default": "Original 10 Mb / 1 kb",
}

SHORT_ARM_NAMES = {
    "dense_default_filter_4p5m_1kb_holdout": "4.5 Mb / 1 kb holdout",
    "dense_default_filter_4p5m_1kb": "4.5 Mb / 1 kb pilot",
    "larger_target_only_40m_1kb": "40 Mb / 1 kb control",
    "proportional_20m_2kb": "20 Mb / 2 kb control",
    "proportional_40m_4kb": "40 Mb / 4 kb control",
    "proportional_80m_8kb": "80 Mb / 8 kb control",
    "requested_default": "original 10 Mb / 1 kb",
}

CSV_FIELDS = (
    "source_role",
    "input_label",
    "cohort_id",
    "cohort_label",
    "arm",
    "run_id",
    "replicate_id",
    "master_seed",
    "occurrence_count",
    "reference_length",
    "query_length",
    "copy_fraction",
    "k",
    "w",
    "lane_width",
    "score_gate_eligible",
    "candidate_end_count",
    "start_score",
    "score_threshold",
    "loop_evaluations",
    "bounds_pass",
    "fast_batches",
    "primary_path_anchors",
    "fast_covered_anchors",
    "exact_run_l_rate_percent",
    "conditional_contiguity_percent",
    "path_coverage_percent",
    "raw_query_minimizers",
    "retained_anchor_count",
    "mid_occ",
    "distinct_reference_minimizers",
    "mid_occ_per_copy",
    "retained_anchors_per_copy",
    "default_filter_tail_budget",
)


def sha256(path: Path) -> str:
    """Return a streaming SHA-256 digest."""
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def utc_now() -> str:
    """Return a second-resolution UTC timestamp."""
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def require(condition: bool, message: str) -> None:
    """Raise a user-facing validation error when *condition* is false."""
    if not condition:
        raise ValueError(message)


def parse_named_path(value: str) -> tuple[str, Path]:
    """Parse ``LABEL=PATH`` used for pilot and control inputs."""
    require("=" in value, f"expected LABEL=PATH, received {value!r}")
    label, raw_path = value.split("=", 1)
    require(bool(label.strip()), f"empty input label in {value!r}")
    require(bool(raw_path.strip()), f"empty input path in {value!r}")
    return label.strip(), Path(raw_path).expanduser().resolve()


def load_yaml(path: Path) -> dict[str, Any]:
    """Load and minimally validate one result document."""
    require(path.is_file(), f"result file does not exist: {path}")
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    require(isinstance(value, dict), f"top-level YAML value must be a mapping: {path}")
    require(isinstance(value.get("runs"), list), f"missing runs list: {path}")
    return value


def rate_percent(record: dict[str, Any] | None, key: str) -> float | None:
    """Read one explicit rate without turning an undefined value into zero."""
    if record is None:
        return None
    rate = (record.get("rates") or {}).get(key)
    if not isinstance(rate, dict):
        return None
    denominator = rate.get("denominator")
    defined = rate.get(
        "defined", isinstance(denominator, int) and denominator > 0
    )
    if not defined:
        return None
    require(
        isinstance(denominator, int) and denominator > 0,
        f"rate {key} is marked defined but has invalid denominator {denominator!r}",
    )
    value = rate.get("percent")
    require(isinstance(value, (int, float)), f"rate {key} lacks a numeric percent")
    return float(value)


def first_map_record(run: dict[str, Any]) -> dict[str, Any]:
    """Return the only mapping diagnostic record used by these experiments."""
    records = run.get("map_diagnostics") or []
    return records[0] if records else {}


def first_path_record(run: dict[str, Any], lanes: int) -> dict[str, Any] | None:
    """Return the initial DP backtrack record for one emulated lane width."""
    summary = run.get("summary") or {}
    records = (
        summary.get("initial_chaining_by_lane")
        or summary.get("initial_dp_by_lane")
        or {}
    )
    record = records.get(str(lanes), records.get(lanes))
    return record if isinstance(record, dict) else None


def require_nonnegative_int(record: dict[str, Any], key: str, context: str) -> int:
    """Read one required nonnegative integer counter."""
    value = record.get(key)
    require(
        isinstance(value, int) and not isinstance(value, bool) and value >= 0,
        f"{context} has invalid {key}: {value!r}",
    )
    return value


def validate_rate_mapping(
    rate: Any,
    expected_numerator: int,
    expected_denominator: int,
    context: str,
) -> None:
    """Validate one serialized rate against its independently checked counters."""
    require(isinstance(rate, dict), f"{context} rate is missing")
    numerator = rate.get("numerator")
    denominator = rate.get("denominator")
    defined = rate.get("defined")
    require(
        isinstance(numerator, int) and not isinstance(numerator, bool),
        f"{context} has invalid numerator {numerator!r}",
    )
    require(
        isinstance(denominator, int) and not isinstance(denominator, bool),
        f"{context} has invalid denominator {denominator!r}",
    )
    require(
        numerator == expected_numerator,
        f"{context} numerator {numerator} != counter {expected_numerator}",
    )
    require(
        denominator == expected_denominator,
        f"{context} denominator {denominator} != counter {expected_denominator}",
    )
    require(isinstance(defined, bool), f"{context} has invalid defined flag {defined!r}")
    percent = rate.get("percent")
    fraction = rate.get("fraction")
    if expected_denominator == 0:
        require(not defined, f"{context} is defined with a zero denominator")
        require(percent is None, f"{context} zero-denominator percent is not null")
        require(fraction is None, f"{context} zero-denominator fraction is not null")
        return
    require(defined, f"{context} is undefined despite a positive denominator")
    require(
        isinstance(percent, (int, float)) and not isinstance(percent, bool),
        f"{context} lacks numeric percent",
    )
    require(
        isinstance(fraction, (int, float)) and not isinstance(fraction, bool),
        f"{context} lacks numeric fraction",
    )
    expected_fraction = expected_numerator / expected_denominator
    require(
        math.isclose(float(fraction), expected_fraction, rel_tol=1e-12, abs_tol=1e-12),
        f"{context} fraction does not match counters",
    )
    require(
        math.isclose(
            float(percent), 100.0 * expected_fraction, rel_tol=1e-12, abs_tol=1e-12
        ),
        f"{context} percent does not match counters",
    )


def validate_initial_lane_record(record: dict[str, Any], lanes: int, context: str) -> None:
    """Reject partial telemetry that could otherwise look like an undefined rate."""
    require(record.get("lanes") == lanes, f"{context} reports wrong lane width")
    require(record.get("stage") == "dp", f"{context} is not an initial DP record")
    require(record.get("call_id") == 1, f"{context} is not call_id 1")
    candidate_ends = require_nonnegative_int(record, "candidate_end_count", context)
    loop_evaluations = require_nonnegative_int(record, "loop_evaluations", context)
    bounds_pass = require_nonnegative_int(record, "bounds_pass", context)
    fast_batches = require_nonnegative_int(record, "fast_batches", context)
    scalar_fallbacks = require_nonnegative_int(record, "scalar_fallbacks", context)
    contiguous_mismatch = require_nonnegative_int(record, "contiguous_mismatch", context)
    primary_anchors = require_nonnegative_int(record, "primary_path_anchors", context)
    covered_anchors = require_nonnegative_int(record, "fast_covered_anchors", context)
    has_ranked_end = record.get("has_ranked_end")
    require(has_ranked_end in (0, 1, False, True), f"{context} has invalid ranked-end flag")
    require(
        bool(has_ranked_end) == (candidate_ends > 0),
        f"{context} ranked-end flag disagrees with candidate count",
    )
    require(
        loop_evaluations == fast_batches + scalar_fallbacks,
        f"{context} loop counter identity failed",
    )
    require(
        bounds_pass == fast_batches + contiguous_mismatch,
        f"{context} bounds counter identity failed",
    )
    require(
        primary_anchors == lanes * fast_batches + scalar_fallbacks,
        f"{context} path counter identity failed",
    )
    require(
        covered_anchors == lanes * fast_batches,
        f"{context} covered-anchor identity failed",
    )
    require(record.get("emulation_error") == 0, f"{context} has emulation errors")
    require(
        record.get("scalar_validation_error") == 0,
        f"{context} has scalar validation errors",
    )
    require(record.get("validation_errors") == [], f"{context} has validation errors")
    rates = record.get("rates")
    require(isinstance(rates, dict), f"{context} lacks rates")
    validate_rate_mapping(
        rates.get("predicate_true_per_loop_evaluation"),
        fast_batches,
        loop_evaluations,
        f"{context} exact Run_L rate",
    )
    validate_rate_mapping(
        rates.get("predicate_true_per_bounds_passing_evaluation"),
        fast_batches,
        bounds_pass,
        f"{context} conditional contiguity rate",
    )
    validate_rate_mapping(
        rates.get("fast_path_anchor_coverage"),
        covered_anchors,
        primary_anchors,
        f"{context} path-coverage rate",
    )


def nested_number(mapping: dict[str, Any], *keys: str) -> int | float | None:
    """Read a number from nested dictionaries, returning null when absent."""
    value: Any = mapping
    for key in keys:
        if not isinstance(value, dict):
            return None
        value = value.get(key)
    return value if isinstance(value, (int, float)) and not isinstance(value, bool) else None


def friendly_arm(arm: str, source_role: str, input_label: str) -> str:
    """Create a stable publication label for one cohort."""
    if arm in FRIENDLY_ARMS:
        return FRIENDLY_ARMS[arm]
    role = source_role.replace("_", " ")
    return f"{input_label}: {arm} ({role})"


def endpoint_fields(run: dict[str, Any]) -> tuple[bool, int | None, int | None, int]:
    """Extract score-gate state independently from Run_L counters."""
    endpoint = run.get("score_gate_eligibility") or run.get("endpoint_eligibility")
    initial = first_path_record(run, LANES[0])
    if isinstance(endpoint, dict):
        eligible = bool(
            endpoint.get(
                "has_score_qualified_candidate_end",
                endpoint.get("has_score_qualified_endpoint", False),
            )
        )
        count = endpoint.get("candidate_end_count")
        score = endpoint.get("start_score")
        threshold = endpoint.get("min_score", DEFAULT_SCORE_THRESHOLD)
    elif initial is not None:
        count = initial.get("candidate_end_count")
        eligible = bool(initial.get("has_ranked_end", bool(count)))
        score = initial.get("start_score")
        threshold = initial.get("min_score", DEFAULT_SCORE_THRESHOLD)
    else:
        eligible, count, score, threshold = False, None, None, DEFAULT_SCORE_THRESHOLD
    if not eligible:
        score = None
    return eligible, count, score, int(threshold)


def infer_replicate(run: dict[str, Any], source_role: str) -> tuple[str, int | None]:
    """Return the independent sampling-unit identifier and its seed."""
    replicate = str(run.get("replicate_id") or "original_single_replicate")
    seed = nested_number(run, "dataset", "master_seed")
    if source_role == "original":
        return replicate, None
    return replicate, int(seed) if seed is not None else None


def normalize_run(
    run: dict[str, Any], source_role: str, input_label: str, source_schema: int
) -> list[dict[str, Any]]:
    """Expand one run into three lane-specific long-form rows."""
    parameters = run.get("parameters") or {}
    dataset = run.get("dataset") or {}
    mapping = first_map_record(run)
    arm = str(run.get("arm", "unknown"))
    occurrence = parameters.get("occurrence_count")
    require(isinstance(occurrence, int) and occurrence > 0, f"bad N in {run.get('run_id')}")
    replicate, master_seed = infer_replicate(run, source_role)

    reference_length = dataset.get("reference_length")
    if reference_length is None:
        reference_length = nested_number(run, "index_diagnostics", "total_reference_length")
    query_length = dataset.get("query_length", mapping.get("query_length"))
    copy_fraction = dataset.get("copy_fraction")
    if copy_fraction is None and reference_length and query_length:
        copy_fraction = occurrence * query_length / reference_length

    distinct = nested_number(run, "index_diagnostics", "distinct_reference_minimizers")
    if distinct is None:
        records = (run.get("index_diagnostics") or {}).get("records") or []
        if records:
            distinct = records[0].get("distinct_reference_minimizers")
    mid_occ = mapping.get("mid_occ")
    retained = mapping.get("retained_anchor_count")
    eligible, candidate_count, start_score, threshold = endpoint_fields(run)
    cohort_id = f"{source_role}:{input_label}:{arm}"

    common = {
        "source_role": source_role,
        "input_label": input_label,
        "cohort_id": cohort_id,
        "cohort_label": friendly_arm(arm, source_role, input_label),
        "arm": arm,
        "run_id": run.get("run_id"),
        "replicate_id": replicate,
        "master_seed": master_seed,
        "occurrence_count": occurrence,
        "reference_length": reference_length,
        "query_length": query_length,
        "copy_fraction": copy_fraction,
        "k": parameters.get("k"),
        "w": parameters.get("w"),
        "score_gate_eligible": eligible,
        "candidate_end_count": candidate_count,
        "start_score": start_score,
        "score_threshold": threshold,
        "raw_query_minimizers": mapping.get("raw_query_minimizers"),
        "retained_anchor_count": retained,
        "mid_occ": mid_occ,
        "distinct_reference_minimizers": distinct,
        "mid_occ_per_copy": (mid_occ / (occurrence + 1)) if mid_occ is not None else None,
        "retained_anchors_per_copy": (retained / occurrence) if retained is not None else None,
        "default_filter_tail_budget": (
            math.ceil(DEFAULT_FILTER_FRACTION * distinct) if distinct is not None else None
        ),
        "source_schema_version": source_schema,
    }

    rows: list[dict[str, Any]] = []
    for lanes in LANES:
        record = first_path_record(run, lanes)
        rows.append(
            {
                **common,
                "lane_width": lanes,
                "loop_evaluations": record.get("loop_evaluations") if record else None,
                "bounds_pass": record.get("bounds_pass") if record else None,
                "fast_batches": record.get("fast_batches") if record else None,
                "primary_path_anchors": (
                    record.get("primary_path_anchors") if record else None
                ),
                "fast_covered_anchors": (
                    record.get("fast_covered_anchors") if record else None
                ),
                "exact_run_l_rate_percent": rate_percent(
                    record, "predicate_true_per_loop_evaluation"
                ),
                "conditional_contiguity_percent": rate_percent(
                    record, "predicate_true_per_bounds_passing_evaluation"
                ),
                "path_coverage_percent": rate_percent(
                    record, "fast_path_anchor_coverage"
                ),
            }
        )
    return rows


def validate_document(
    document: dict[str, Any], path: Path, role: str, allow_partial: bool
) -> None:
    """Reject incomplete or invalid follow-up inputs unless explicitly allowed."""
    schema = int(document.get("schema_version", 0))
    if role == "original":
        require(
            schema == 1,
            f"historical original input must use schema_version 1, found {schema} in {path}",
        )
    else:
        require(
            schema == 2,
            f"{role} follow-up input must use schema_version 2, found {schema} in {path}",
        )
    if schema == 2:
        experiment = document.get("experiment") or {}
        if not allow_partial:
            require(experiment.get("status") == "complete", f"incomplete {role} input: {path}")
            require(experiment.get("missing_run_count") == 0, f"missing runs in {path}")
            require(experiment.get("failed_run_count") == 0, f"failed runs in {path}")
        checks = document.get("validations") or {}
        require(
            checks.get("all_observed_run_l_counter_identities_pass") is True,
            f"missing or failed Run_L counter-identity validation in {path}",
        )
        for run in document["runs"]:
            if run.get("status", "completed") not in {
                "complete",
                "completed",
                "success",
            }:
                continue
            records = (run.get("summary") or {}).get("initial_chaining_by_lane")
            require(
                isinstance(records, dict),
                f"schema-v2 run lacks initial_chaining_by_lane: {run.get('run_id')} in {path}",
            )
            for lanes in LANES:
                record = records.get(str(lanes), records.get(lanes))
                require(
                    isinstance(record, dict),
                    f"schema-v2 run lacks L={lanes} initial record: "
                    f"{run.get('run_id')} in {path}",
                )
                validate_initial_lane_record(
                    record,
                    lanes,
                    f"{run.get('run_id')} L={lanes} in {path}",
                )


def collect_rows(
    inputs: Sequence[tuple[str, str, Path]], allow_partial: bool
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Load every input and return normalized rows plus provenance records."""
    rows: list[dict[str, Any]] = []
    provenance: list[dict[str, Any]] = []
    seen: set[tuple[str, str, str]] = set()
    for role, label, path in inputs:
        document = load_yaml(path)
        validate_document(document, path, role, allow_partial)
        schema = int(document["schema_version"])
        accepted = 0
        skipped_by_selection = 0
        for run in document["runs"]:
            if run.get("status", "completed") not in {"complete", "completed", "success"}:
                continue
            if role == "original":
                memberships = run.get("series_memberships") or []
                is_occurrence_sweep = any(
                    member.get("series") == "occurrence_count"
                    for member in memberships
                    if isinstance(member, dict)
                )
                parameters = run.get("parameters") or {}
                if (
                    run.get("arm") != "requested_default"
                    or parameters.get("k") != 15
                    or not is_occurrence_sweep
                ):
                    skipped_by_selection += 1
                    continue
            arm = str(run.get("arm", "unknown"))
            # The original result contains sensitivity arms.  Keep them in the
            # export, but plot only the requested default arm as historical context.
            run_id = str(run.get("run_id"))
            identity = (role, label, run_id)
            require(identity not in seen, f"duplicate run identity {identity}")
            seen.add(identity)
            rows.extend(normalize_run(run, role, label, schema))
            accepted += 1
        provenance.append(
            {
                "source_role": role,
                "input_label": label,
                "path": str(path),
                "sha256": sha256(path),
                "schema_version": schema,
                "completed_run_count": accepted,
                "skipped_run_count_by_selection": skipped_by_selection,
                "selection": (
                    "requested_default arm, k=15 occurrence-count sweep"
                    if role == "original"
                    else "all completed runs"
                ),
                "experiment_status": (document.get("experiment") or {}).get("status"),
            }
        )
    require(bool(rows), "no completed runs were found")
    return rows, provenance


def percentile(values: Sequence[float], q: float) -> float | None:
    """Return a linearly interpolated percentile or null for no observations."""
    if not values:
        return None
    return float(np.percentile(np.asarray(values, dtype=float), q, method="linear"))


def summarize(rows: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    """Compute per-replicate median and IQR without pooling anchor counters."""
    metrics = (
        "score_gate_eligible",
        "start_score",
        "exact_run_l_rate_percent",
        "conditional_contiguity_percent",
        "path_coverage_percent",
        "mid_occ_per_copy",
        "retained_anchors_per_copy",
        "default_filter_tail_budget",
    )
    groups: dict[tuple[str, int, int], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        groups[(row["cohort_id"], row["occurrence_count"], row["lane_width"])].append(row)
    output: list[dict[str, Any]] = []
    for (cohort_id, occurrence, lanes), group in sorted(groups.items()):
        head = group[0]
        metric_summaries: dict[str, Any] = {}
        for metric in metrics:
            values = [
                float(row[metric])
                for row in group
                if row.get(metric) is not None
            ]
            metric_summaries[metric] = {
                "n_defined_replicates": len(values),
                "n_total_replicates": len(group),
                "median": percentile(values, 50),
                "q1": percentile(values, 25),
                "q3": percentile(values, 75),
                "minimum": min(values) if values else None,
                "maximum": max(values) if values else None,
            }
        eligible_count = sum(bool(row["score_gate_eligible"]) for row in group)
        output.append(
            {
                "cohort_id": cohort_id,
                "cohort_label": head["cohort_label"],
                "source_role": head["source_role"],
                "arm": head["arm"],
                "occurrence_count": occurrence,
                "lane_width": lanes,
                "replicate_count": len(group),
                "eligible_replicate_count": eligible_count,
                "eligible_replicate_percent": 100.0 * eligible_count / len(group),
                "metrics": metric_summaries,
            }
        )
    return output


def write_csv(path: Path, rows: Sequence[dict[str, Any]]) -> None:
    """Write deterministic long-form plot data."""
    with path.open("w", encoding="utf-8", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=CSV_FIELDS, extrasaction="ignore")
        writer.writeheader()
        for row in sorted(
            rows,
            key=lambda value: (
                value["source_role"],
                value["input_label"],
                value["arm"],
                value["replicate_id"],
                value["occurrence_count"],
                value["lane_width"],
            ),
        ):
            writer.writerow({key: "" if row.get(key) is None else row.get(key) for key in CSV_FIELDS})


def plotted_rows(rows: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    """Select comparable cohorts for figures while retaining all rows in exports."""
    return list(rows)


def cohort_order(rows: Sequence[dict[str, Any]]) -> list[str]:
    """Place the holdout first, then pilot, controls, and historical context."""
    priority = {"compact_holdout": 0, "pilot": 1, "control": 2, "original": 3}
    cohorts = {row["cohort_id"]: row for row in rows}
    return sorted(
        cohorts,
        key=lambda key: (
            priority.get(cohorts[key]["source_role"], 9),
            cohorts[key]["cohort_label"],
        ),
    )


def style_maps(
    rows: Sequence[dict[str, Any]],
) -> tuple[dict[str, Any], dict[str, str], dict[str, str]]:
    """Return stable color, line-style, and marker mappings."""
    order = cohort_order(rows)
    palette = list(plt.get_cmap("tab10").colors)
    colors = {cohort: palette[index % len(palette)] for index, cohort in enumerate(order)}
    markers = {
        cohort: COHORT_MARKERS[index % len(COHORT_MARKERS)]
        for index, cohort in enumerate(order)
    }
    styles: dict[str, str] = {}
    by_id = {row["cohort_id"]: row for row in rows}
    for cohort in order:
        styles[cohort] = {
            "compact_holdout": "-",
            "pilot": ":",
            "control": "--",
            "original": "-.",
        }.get(by_id[cohort]["source_role"], "--")
    return colors, styles, markers


def replicate_count_note(rows: Sequence[dict[str, Any]]) -> str:
    """Describe the independent replicate count at each sampled N."""
    role_names = {
        "compact_holdout": "holdout",
        "pilot": "pilot",
        "control": "controls",
        "original": "original",
    }
    by_role_and_point: dict[tuple[str, str, int], set[str]] = defaultdict(set)
    for row in rows:
        by_role_and_point[
            (row["source_role"], row["cohort_id"], row["occurrence_count"])
        ].add(row["replicate_id"])
    counts_by_role: dict[str, set[int]] = defaultdict(set)
    for (role, _cohort, _occurrence), replicates in by_role_and_point.items():
        counts_by_role[role].add(len(replicates))
    parts: list[str] = []
    for role in ("compact_holdout", "pilot", "control", "original"):
        counts = sorted(counts_by_role.get(role, set()))
        if not counts:
            continue
        count_text = str(counts[0]) if len(counts) == 1 else f"{counts[0]}--{counts[-1]}"
        parts.append(f"{role_names[role]} n={count_text}")
    return (
        "Replicates at each sampled $N$: "
        + ", ".join(parts)
        + ". $n$ counts generated replicates. Rate summaries omit undefined values. "
        "Endpoint-only markers are unconnected and separated by fixed 5-point "
        "display-space offsets. Their stored $N$ values are unchanged."
    )


def cohort_occurrences(
    rows: Sequence[dict[str, Any]], cohort: str, lanes: int
) -> tuple[int, ...]:
    """Return every sampled N for one cohort and lane width."""
    return tuple(
        sorted(
            {
                int(row["occurrence_count"])
                for row in rows
                if row["cohort_id"] == cohort and row["lane_width"] == lanes
            }
        )
    )


def endpoint_only_cohorts(rows: Sequence[dict[str, Any]], lanes: int) -> list[str]:
    """Return arms sampled only at the two requested endpoints."""
    return [
        cohort
        for cohort in cohort_order(rows)
        if cohort_occurrences(rows, cohort, lanes) == (20, 4000)
    ]


def endpoint_marker_offset_points(
    rows: Sequence[dict[str, Any]], cohort: str, lanes: int
) -> float:
    """Return a fixed display-space offset for one endpoint-only cohort."""
    endpoint_cohorts = endpoint_only_cohorts(rows, lanes)
    if cohort not in endpoint_cohorts or len(endpoint_cohorts) < 2:
        return 0.0
    rank = endpoint_cohorts.index(cohort)
    centered_rank = rank - (len(endpoint_cohorts) - 1) / 2.0
    return ENDPOINT_MARKER_OFFSET_POINTS * centered_rank


def endpoint_marker_transform(
    axis: plt.Axes,
    rows: Sequence[dict[str, Any]],
    cohort: str,
    lanes: int,
) -> Any:
    """Translate markers in points while leaving their data x coordinate unchanged."""
    offset_points = endpoint_marker_offset_points(rows, cohort, lanes)
    return axis.transData + ScaledTranslation(
        offset_points / 72.0,
        0.0,
        axis.figure.dpi_scale_trans,
    )


def include_endpoint_data_limits(
    axis: plt.Axes, x: np.ndarray, *y_arrays: np.ndarray
) -> None:
    """Include true endpoint coordinates before applying a display-only transform."""
    for y in y_arrays:
        finite = np.isfinite(x) & np.isfinite(y)
        if np.any(finite):
            axis.update_datalim(np.column_stack((x[finite], y[finite])))
    axis.autoscale_view()


def endpoint_display_points(
    rows: Sequence[dict[str, Any]], lanes: int
) -> list[tuple[float, float]]:
    """Return true endpoint N values and their horizontal point offsets."""
    points: list[tuple[float, float]] = []
    for cohort in endpoint_only_cohorts(rows, lanes):
        offset_points = endpoint_marker_offset_points(rows, cohort, lanes)
        points.extend(
            (float(occurrence), offset_points)
            for occurrence in cohort_occurrences(rows, cohort, lanes)
        )
    return points


def derive_log_x_limits(
    axis: plt.Axes,
    rows: Sequence[dict[str, Any]],
    lanes: int,
) -> tuple[float, float]:
    """Pad the true N domain enough for every display-shifted endpoint artist."""
    sampled_n = sorted(
        {
            float(row["occurrence_count"])
            for row in rows
            if float(row["occurrence_count"]) > 0.0
        }
    )
    require(bool(sampled_n), "cannot configure a log x-axis without positive N values")
    lower_log = math.log10(sampled_n[0])
    upper_log = math.log10(sampled_n[-1])
    log_span = upper_log - lower_log
    log_padding = max(0.04, 0.04 * log_span)
    renderer = axis.figure.canvas.get_renderer()
    axis_width_points = (
        axis.get_window_extent(renderer).width * 72.0 / axis.figure.dpi
    )
    require(axis_width_points > 0.0, "cannot derive endpoint padding for a zero-width axis")
    endpoint_points = endpoint_display_points(rows, lanes)
    for _iteration in range(100):
        padded_span = max(log_span + 2.0 * log_padding, 1e-12)
        largest_deficit = 0.0
        for occurrence, offset_points in endpoint_points:
            center_points = (
                axis_width_points
                * (math.log10(occurrence) - lower_log + log_padding)
                / padded_span
                + offset_points
            )
            left_deficit = ENDPOINT_ARTIST_HALF_WIDTH_POINTS - center_points
            right_deficit = (
                center_points
                + ENDPOINT_ARTIST_HALF_WIDTH_POINTS
                - axis_width_points
            )
            largest_deficit = max(largest_deficit, left_deficit, right_deficit)
        if largest_deficit <= 0.0:
            break
        decades_per_point = padded_span / axis_width_points
        log_padding += 1.25 * largest_deficit * decades_per_point + 0.005
    else:
        raise ValueError("failed to derive transform-aware endpoint padding")
    return (
        10.0 ** (lower_log - log_padding),
        10.0 ** (upper_log + log_padding),
    )


def require_endpoint_artists_inside_axis(
    axis: plt.Axes,
    rows: Sequence[dict[str, Any]],
    lanes: int,
) -> None:
    """Fail closed when an offset marker or cap would intersect an axes spine."""
    renderer = axis.figure.canvas.get_renderer()
    axes_box = axis.get_window_extent(renderer)
    half_width_pixels = (
        ENDPOINT_ARTIST_HALF_WIDTH_POINTS * axis.figure.dpi / 72.0
    )
    for cohort in endpoint_only_cohorts(rows, lanes):
        transform = endpoint_marker_transform(axis, rows, cohort, lanes)
        for occurrence in cohort_occurrences(rows, cohort, lanes):
            center_x = transform.transform([[float(occurrence), 0.0]])[0, 0]
            require(
                center_x - half_width_pixels >= axes_box.x0 - 1e-7
                and center_x + half_width_pixels <= axes_box.x1 + 1e-7,
                f"offset endpoint artist for {cohort} at N={occurrence} "
                "would be clipped by the axes boundary",
            )


def cohort_zorder(row: dict[str, Any]) -> int:
    """Keep the primary holdout visible when values coincide."""
    return {
        "original": 3,
        "control": 4,
        "pilot": 5,
        "compact_holdout": 6,
    }.get(row["source_role"], 3)


def marker_face(row: dict[str, Any], color: Any) -> Any:
    """Use open comparison markers and a filled primary marker."""
    return color if row["source_role"] == "compact_holdout" else "white"


def metric_series(
    rows: Sequence[dict[str, Any]], cohort: str, lanes: int, metric: str
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Return x, median, q1, and q3 with NaN at undefined sampled points."""
    groups: dict[int, list[float]] = defaultdict(list)
    sampled_x: set[int] = set()
    for row in rows:
        if row["cohort_id"] != cohort or row["lane_width"] != lanes:
            continue
        sampled_x.add(int(row["occurrence_count"]))
        value = row.get(metric)
        if value is not None:
            groups[row["occurrence_count"]].append(float(value))
    x = np.asarray(sorted(sampled_x), dtype=float)

    def value_or_nan(n: float, q: float) -> float:
        values = groups.get(int(n), [])
        value = percentile(values, q)
        return np.nan if value is None else value

    median = np.asarray([value_or_nan(n, 50) for n in x], dtype=float)
    q1 = np.asarray([value_or_nan(n, 25) for n in x], dtype=float)
    q3 = np.asarray([value_or_nan(n, 75) for n in x], dtype=float)
    return x, median, q1, q3


def plot_replicate_curves(
    axis: plt.Axes,
    rows: Sequence[dict[str, Any]],
    cohort: str,
    lanes: int,
    metric: str,
    color: Any,
) -> None:
    """Draw every holdout replicate as a faint curve, preserving null gaps."""
    replicate_rows: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        if row["cohort_id"] == cohort and row["lane_width"] == lanes:
            replicate_rows[row["replicate_id"]].append(row)
    all_x = sorted({row["occurrence_count"] for values in replicate_rows.values() for row in values})
    for values in replicate_rows.values():
        lookup = {row["occurrence_count"]: row.get(metric) for row in values}
        y = [np.nan if lookup.get(x) is None else float(lookup[x]) for x in all_x]
        axis.plot(all_x, y, color=color, alpha=0.20, linewidth=0.7, zorder=1)


def plot_summary_metric(
    axis: plt.Axes,
    rows: Sequence[dict[str, Any]],
    metric: str,
    lanes: int,
    colors: dict[str, Any],
    styles: dict[str, str],
    markers: dict[str, str],
    show_replicate_curves: bool,
    include_undefined_legend: bool = True,
) -> None:
    """Draw cohort medians and replicate-level IQR without bridging nulls."""
    by_id = {row["cohort_id"]: row for row in rows}
    for cohort in cohort_order(rows):
        head = by_id[cohort]
        if show_replicate_curves and head["source_role"] == "compact_holdout":
            plot_replicate_curves(axis, rows, cohort, lanes, metric, colors[cohort])
        x, median, q1, q3 = metric_series(rows, cohort, lanes, metric)
        if not len(x):
            continue
        label = head["cohort_label"]
        zorder = cohort_zorder(head)
        if not np.any(np.isfinite(median)):
            if not include_undefined_legend:
                continue
            undefined_suffix = (
                "undefined at N=20,4000"
                if cohort_occurrences(rows, cohort, lanes) == (20, 4000)
                else "undefined"
            )
            axis.plot(
                [],
                [],
                color=colors[cohort],
                linestyle="None",
                marker=markers[cohort],
                markerfacecolor="white",
                markeredgewidth=DATA_MARKER_EDGE_WIDTH_POINTS,
                markersize=4.5,
                label=f"{label} ({undefined_suffix})",
                zorder=zorder,
            )
            continue
        endpoint_only = cohort_occurrences(rows, cohort, lanes) == (20, 4000)
        plot_transform: dict[str, Any] = {}
        if endpoint_only:
            include_endpoint_data_limits(axis, x, median, q1, q3)
            plot_transform["transform"] = endpoint_marker_transform(
                axis, rows, cohort, lanes
            )
        axis.plot(
            x,
            median,
            color=colors[cohort],
            linestyle="None" if endpoint_only else styles[cohort],
            marker=markers[cohort],
            markerfacecolor=marker_face(head, colors[cohort]),
            markeredgecolor=colors[cohort],
            markeredgewidth=DATA_MARKER_EDGE_WIDTH_POINTS,
            markersize=DATA_MARKER_SIZE_POINTS,
            linewidth=1.4,
            label=label,
            zorder=zorder,
            **plot_transform,
        )
        has_spread = np.isfinite(q1) & np.isfinite(q3) & ~np.isclose(q1, q3)
        if not np.any(has_spread):
            continue
        if endpoint_only:
            finite = np.isfinite(median) & np.isfinite(q1) & np.isfinite(q3)
            axis.errorbar(
                x[finite],
                median[finite],
                yerr=np.vstack(
                    (median[finite] - q1[finite], q3[finite] - median[finite])
                ),
                color=colors[cohort],
                linestyle="None",
                linewidth=ERRORBAR_LINE_WIDTH_POINTS,
                capsize=ERRORBAR_CAP_SIZE_POINTS,
                zorder=zorder - 0.2,
                transform=plot_transform["transform"],
            )
        else:
            axis.fill_between(
                x,
                q1,
                q3,
                where=np.isfinite(q1) & np.isfinite(q3),
                color=colors[cohort],
                alpha=0.13,
                linewidth=0,
                interpolate=False,
                zorder=zorder - 0.5,
            )


def save_figure(figure: plt.Figure, output_dir: Path, stem: str) -> list[Path]:
    """Save one figure in vector and raster publication formats."""
    paths: list[Path] = []
    for extension in PLOT_FORMATS:
        path = output_dir / f"{stem}.{extension}"
        figure.savefig(path, dpi=300 if extension == "png" else None)
        paths.append(path)
    plt.close(figure)
    return paths


def add_shared_legend(figure: plt.Figure, axes: Iterable[plt.Axes]) -> None:
    """Add one deduplicated legend below a multi-panel figure."""
    handles: list[Any] = []
    labels: list[str] = []
    seen: set[str] = set()
    for axis in axes:
        axis_handles, axis_labels = axis.get_legend_handles_labels()
        for handle, label in zip(axis_handles, axis_labels):
            if label and label not in seen:
                handles.append(handle)
                labels.append(label)
                seen.add(label)
    if handles:
        figure.legend(
            handles,
            labels,
            loc="lower center",
            bbox_to_anchor=(0.5, 0.01),
            frameon=False,
            fontsize=6.7,
            ncol=min(2, len(handles)),
        )


def set_figure_title(
    figure: plt.Figure,
    title: str,
    rows: Sequence[dict[str, Any]],
    note: str = "",
) -> None:
    """Add the scientific title and dynamically verified sample sizes."""
    subtitle = replicate_count_note(rows)
    if note:
        subtitle += " " + note
    wrapped_subtitle = textwrap.fill(
        subtitle,
        width=92,
        break_long_words=False,
        break_on_hyphens=False,
    )
    figure.suptitle(
        f"{title}\n{wrapped_subtitle}",
        fontsize=9.2,
        y=0.985,
        linespacing=1.15,
    )


def undefined_arm_names(
    rows: Sequence[dict[str, Any]], metric: str, lanes: int
) -> list[str]:
    """Return concise names for arms with no defined metric value."""
    by_id = {row["cohort_id"]: row for row in rows}
    names: list[str] = []
    for cohort in cohort_order(rows):
        values = [
            row.get(metric)
            for row in rows
            if row["cohort_id"] == cohort and row["lane_width"] == lanes
        ]
        if values and all(value is None for value in values):
            names.append(SHORT_ARM_NAMES.get(by_id[cohort]["arm"], by_id[cohort]["cohort_label"]))
    return names


def annotate_undefined(
    axis: plt.Axes,
    rows: Sequence[dict[str, Any]],
    metric: str,
    lanes: int,
    prefix: str,
) -> None:
    """State which sampled arms have no defined metric without inventing zeros."""
    names = undefined_arm_names(rows, metric, lanes)
    if not names:
        return
    axis.text(
        0.02,
        0.035,
        prefix + ":\n" + ", ".join(names),
        transform=axis.transAxes,
        ha="left",
        va="bottom",
        fontsize=6.4,
        color="#40464d",
        bbox={
            "boxstyle": "round,pad=0.25",
            "facecolor": "white",
            "edgecolor": "#b8bec5",
            "linewidth": 0.6,
            "alpha": 0.92,
        },
        zorder=20,
    )


def configure_axis(
    axis: plt.Axes,
    rows: Sequence[dict[str, Any]],
    lanes: int,
    ylabel: str,
    percentage: bool = False,
) -> None:
    """Apply shared formatting and explicit limits from true stored N values."""
    axis.set_xscale("log")
    axis.set_xlim(*derive_log_x_limits(axis, rows, lanes))
    require_endpoint_artists_inside_axis(axis, rows, lanes)
    axis.set_xlabel("Planted query occurrences, $N$")
    axis.set_ylabel(ylabel)
    axis.grid(True, which="major", color="#d7dce2", linewidth=0.6)
    axis.grid(True, which="minor", axis="x", color="#edf0f3", linewidth=0.4)
    if percentage:
        axis.set_ylim(-3, 103)


def figure_eligibility_and_score(
    rows: Sequence[dict[str, Any]],
    output_dir: Path,
    colors: dict[str, Any],
    styles: dict[str, str],
    markers: dict[str, str],
) -> list[Path]:
    """Plot score-gate eligibility separately from Run_L execution."""
    figure, axes = plt.subplots(1, 2, figsize=(THESIS_TEXT_WIDTH_IN, 4.05))
    figure.subplots_adjust(
        left=0.105, right=0.985, top=0.72, bottom=0.30, wspace=0.31
    )
    by_id = {row["cohort_id"]: row for row in rows}
    lane_rows = [row for row in rows if row["lane_width"] == LANES[0]]
    for cohort in cohort_order(lane_rows):
        group = [row for row in lane_rows if row["cohort_id"] == cohort]
        grouped: dict[int, list[dict[str, Any]]] = defaultdict(list)
        for row in group:
            grouped[row["occurrence_count"]].append(row)
        x = np.asarray(sorted(grouped), dtype=float)
        eligible = np.asarray(
            [100.0 * sum(r["score_gate_eligible"] for r in grouped[int(n)]) / len(grouped[int(n)]) for n in x]
        )
        head = by_id[cohort]
        endpoint_only = cohort_occurrences(lane_rows, cohort, LANES[0]) == (20, 4000)
        plot_transform: dict[str, Any] = {}
        if endpoint_only:
            include_endpoint_data_limits(axes[0], x, eligible)
            plot_transform["transform"] = endpoint_marker_transform(
                axes[0], lane_rows, cohort, LANES[0]
            )
        axes[0].plot(
            x,
            eligible,
            color=colors[cohort],
            linestyle="None" if endpoint_only else styles[cohort],
            marker=markers[cohort],
            markerfacecolor=marker_face(head, colors[cohort]),
            markeredgecolor=colors[cohort],
            markeredgewidth=DATA_MARKER_EDGE_WIDTH_POINTS,
            markersize=DATA_MARKER_SIZE_POINTS,
            linewidth=1.4,
            label=head["cohort_label"],
            zorder=cohort_zorder(head),
            **plot_transform,
        )
    plot_summary_metric(
        axes[1],
        lane_rows,
        "start_score",
        LANES[0],
        colors,
        styles,
        markers,
        show_replicate_curves=True,
        include_undefined_legend=False,
    )
    axes[1].axhline(
        DEFAULT_SCORE_THRESHOLD, color="#b33a3a", linewidth=1.0, linestyle="--",
        label="score threshold 40",
    )
    configure_axis(
        axes[0],
        lane_rows,
        LANES[0],
        "Replicates with a score-qualified ranked end (%)",
        percentage=True,
    )
    configure_axis(
        axes[1], lane_rows, LANES[0], r"Top-ranked candidate score, $f_j$"
    )
    axes[0].set_title("Score-gate eligibility")
    axes[1].set_title("Score of the top-ranked candidate")
    axes[0].text(
        0.035,
        0.12,
        "Original arm, $N=20\\ldots4000$:\n"
        "two retained anchors per copy\n"
        "and no score-qualified ranked end",
        transform=axes[0].transAxes,
        ha="left",
        va="bottom",
        fontsize=6.4,
        color="#40464d",
        bbox={
            "boxstyle": "round,pad=0.25",
            "facecolor": "white",
            "edgecolor": "#b8bec5",
            "linewidth": 0.6,
            "alpha": 0.92,
        },
        zorder=20,
    )
    annotate_undefined(
        axes[1],
        lane_rows,
        "start_score",
        LANES[0],
        "No score-qualified ranked end",
    )
    add_shared_legend(figure, axes)
    set_figure_title(
        figure,
        "Score qualification before evaluating $\\operatorname{Run}_L$",
        lane_rows,
    )
    return save_figure(figure, output_dir, "run_l_eligibility_and_score")


def figure_exact_rate(
    rows: Sequence[dict[str, Any]],
    output_dir: Path,
    colors: dict[str, Any],
    styles: dict[str, str],
    markers: dict[str, str],
) -> list[Path]:
    """Plot the exact first-path predicate rate for each emulated lane width."""
    figure, axes = plt.subplots(
        1, 3, figsize=(THESIS_TEXT_WIDTH_IN, 3.80), sharey=True
    )
    figure.subplots_adjust(
        left=0.09, right=0.99, top=0.70, bottom=0.30, wspace=0.15
    )
    for axis, lanes in zip(axes, LANES):
        plot_summary_metric(
            axis,
            rows,
            "exact_run_l_rate_percent",
            lanes,
            colors,
            styles,
            markers,
            show_replicate_curves=True,
        )
        configure_axis(
            axis,
            rows,
            lanes,
            "Run$_L$ batches / loop evaluations (%)",
            percentage=True,
        )
        axis.set_title(f"$L={lanes}$")
    for axis in axes[1:]:
        axis.set_ylabel("")
    add_shared_legend(figure, axes)
    set_figure_title(
        figure,
        "Observed first-path $\\operatorname{Run}_L$ branch rate",
        rows,
        "Lines and error bars show the median and replicate IQR. "
        "Undefined arms remain in the legend.",
    )
    return save_figure(figure, output_dir, "run_l_exact_firing_rate")


def figure_conditional_and_coverage(
    rows: Sequence[dict[str, Any]],
    output_dir: Path,
    colors: dict[str, Any],
    styles: dict[str, str],
    markers: dict[str, str],
) -> list[Path]:
    """Separate bounds-passing contiguity from path coverage."""
    figure, axes = plt.subplots(
        2,
        3,
        figsize=(THESIS_TEXT_WIDTH_IN, 6.20),
        sharex="col",
    )
    figure.subplots_adjust(
        left=0.10,
        right=0.99,
        top=0.80,
        bottom=0.18,
        wspace=0.20,
        hspace=0.34,
    )
    for column, lanes in enumerate(LANES):
        plot_summary_metric(
            axes[0, column], rows, "conditional_contiguity_percent", lanes,
            colors, styles, markers, show_replicate_curves=True,
        )
        plot_summary_metric(
            axes[1, column], rows, "path_coverage_percent", lanes,
            colors, styles, markers, show_replicate_curves=True,
        )
        configure_axis(
            axes[0, column],
            rows,
            lanes,
            "Run$_L$ / bounds-passing evaluations (%)",
            percentage=True,
        )
        configure_axis(
            axes[1, column],
            rows,
            lanes,
            "Anchors covered by Run$_L$ batches (%)",
            percentage=True,
        )
        axes[0, column].set_title(f"Conditional contiguity, $L={lanes}$")
        axes[1, column].set_title(f"Path coverage, $L={lanes}$")
    for axis in axes[:, 1:].flat:
        axis.set_ylabel("")
    add_shared_legend(figure, axes.flat)
    set_figure_title(
        figure,
        "Contiguity of the first predecessor path and path coverage",
        rows,
        "Lines and error bars show the median and replicate IQR. "
        "Undefined arms remain in the legend.",
    )
    return save_figure(figure, output_dir, "run_l_contiguity_and_path_coverage")


def figure_filter_diagnostics(
    rows: Sequence[dict[str, Any]],
    output_dir: Path,
    colors: dict[str, Any],
    styles: dict[str, str],
    markers: dict[str, str],
) -> list[Path]:
    """Plot default-filter diagnostics on one observation per replicate."""
    figure, axes = plt.subplots(
        3, 1, figsize=(THESIS_TEXT_WIDTH_IN, 7.00), sharex=True
    )
    figure.subplots_adjust(
        left=0.13, right=0.98, top=0.80, bottom=0.17, hspace=0.32
    )
    lane_rows = [row for row in rows if row["lane_width"] == LANES[0]]
    specifications = (
        (
            "mid_occ_per_copy",
            r"Cutoff ratio, $\mathrm{mid\_occ}/(N+1)$",
            "Adaptive occurrence cutoff",
        ),
        (
            "retained_anchors_per_copy",
            r"Retained anchors per copy, $A/N$",
            "Retained anchors",
        ),
        (
            "default_filter_tail_budget",
            r"Tail budget, $\lceil 0.0002D\rceil$ keys",
            "Default filter tail budget",
        ),
    )
    for axis, (metric, ylabel, title) in zip(axes, specifications):
        plot_summary_metric(
            axis,
            lane_rows,
            metric,
            LANES[0],
            colors,
            styles,
            markers,
            show_replicate_curves=True,
        )
        configure_axis(axis, lane_rows, LANES[0], ylabel)
        axis.set_title(title)
    for axis in axes[:-1]:
        axis.set_xlabel("")
    add_shared_legend(figure, axes)
    set_figure_title(
        figure,
        "Default reference-frequency filter diagnostics",
        lane_rows,
        "$D$ is the number of distinct reference minimizer keys. "
        "$N+1$ is the first integer cutoff above $N$ planted copies.",
    )
    return save_figure(figure, output_dir, "run_l_filter_diagnostics")


def ensure_outputs_available(output_dir: Path, force: bool) -> None:
    """Avoid accidental replacement unless the caller opts in."""
    expected = [output_dir / "run_l_followup_merged.yaml", output_dir / "run_l_followup_plot_data.csv"]
    for stem in (
        "run_l_eligibility_and_score",
        "run_l_exact_firing_rate",
        "run_l_contiguity_and_path_coverage",
        "run_l_filter_diagnostics",
    ):
        expected.extend(output_dir / f"{stem}.{extension}" for extension in PLOT_FORMATS)
    existing = [path for path in expected if path.exists()]
    require(force or not existing, "outputs already exist. Pass --force to replace: " + ", ".join(map(str, existing)))


def create_outputs(
    inputs: Sequence[tuple[str, str, Path]], output_dir: Path, allow_partial: bool, force: bool
) -> list[Path]:
    """Stage a complete generation, then publish the merged manifest last."""
    output_dir = output_dir.resolve()
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    ensure_outputs_available(output_dir, force)
    rows, input_provenance = collect_rows(inputs, allow_partial)
    summaries = summarize(rows)
    with tempfile.TemporaryDirectory(
        prefix=f".{output_dir.name}.staging-", dir=output_dir.parent
    ) as staging_name:
        staging_dir = Path(staging_name)
        csv_path = staging_dir / "run_l_followup_plot_data.csv"
        write_csv(csv_path, rows)

        figure_rows = plotted_rows(rows)
        colors, styles, markers = style_maps(figure_rows)
        artifacts: list[Path] = [csv_path]
        artifacts += figure_eligibility_and_score(
            figure_rows, staging_dir, colors, styles, markers
        )
        artifacts += figure_exact_rate(
            figure_rows, staging_dir, colors, styles, markers
        )
        artifacts += figure_conditional_and_coverage(
            figure_rows, staging_dir, colors, styles, markers
        )
        artifacts += figure_filter_diagnostics(
            figure_rows, staging_dir, colors, styles, markers
        )

        merged_path = staging_dir / "run_l_followup_merged.yaml"
        merged = {
            "schema_version": 1,
            "created_at_utc": utc_now(),
            "experiment": {
                "title": "Follow-up evaluation of the first-path Run_L predicate",
                "predicate_latex": (
                    r"Run_L(j,c) iff j >= L-1 and c <= n-L and "
                    r"for every u in [0,L-1], p[j-u] = j-u-1"
                ),
                "score_threshold": DEFAULT_SCORE_THRESHOLD,
                "default_reference_filter_fraction": DEFAULT_FILTER_FRACTION,
                "lane_widths": list(LANES),
            },
            "methodology": {
                "sampling_unit": (
                    "one independently generated query/reference replicate, "
                    "identified by its master seed"
                ),
                "aggregation": (
                    "median and interquartile range across defined replicate values at fixed "
                    "arm, N, and L"
                ),
                "replicate_count_convention": (
                    "figure n values count all generated replicates. Undefined metric values "
                    "are omitted from that metric's median and interquartile range"
                ),
                "prohibited_aggregation": (
                    "anchor and loop counters are not pooled across replicates"
                ),
                "null_policy": (
                    "undefined rates remain YAML null, empty CSV cells, and gaps in curves. "
                    "They are never encoded as zero"
                ),
                "endpoint_only_policy": (
                    "arms sampled only at N=20 and N=4000 use unconnected markers. "
                    "Adjacent endpoint-only cohorts receive fixed 5-point horizontal "
                    "display-space offsets when needed to expose overlap. Their stored x values "
                    "remain the measured N"
                ),
                "eligibility": (
                    "score_gate_eligible is reported before Run_L rates. "
                    "It records whether the score gate found a ranked end and does not apply "
                    "minimap2's later minimum-count gate. No score-qualified ranked end "
                    "means the Run_L rates are undefined"
                ),
                "exact_rate": "fast_batches / loop_evaluations for the initial DP backtrack",
                "conditional_contiguity": (
                    "fast_batches / bounds_pass for the initial DP backtrack"
                ),
                "path_coverage": (
                    "fast_covered_anchors / primary_path_anchors for the initial DP backtrack"
                ),
                "filter_diagnostics": {
                    "mid_occ_per_copy": (
                        "mid_occ / (N + 1), where N + 1 is the first integer cutoff "
                        "above N planted copies"
                    ),
                    "retained_anchors_per_copy": "retained_anchor_count / N",
                    "default_filter_tail_budget": (
                        "ceil(0.0002 * D), where D is the number of distinct "
                        "reference minimizer keys"
                    ),
                },
            },
            "provenance": {
                "finalizer_path": str(SCRIPT_PATH),
                "finalizer_sha256": sha256(SCRIPT_PATH),
                "inputs": input_provenance,
                "dependencies": {
                    "python": sys.version.split()[0],
                    "pyyaml": yaml.__version__,
                    "numpy": np.__version__,
                    "matplotlib": matplotlib.__version__,
                },
            },
            "cohorts": [
                {
                    "cohort_id": cohort,
                    "cohort_label": next(
                        row["cohort_label"] for row in rows if row["cohort_id"] == cohort
                    ),
                    "source_role": next(
                        row["source_role"] for row in rows if row["cohort_id"] == cohort
                    ),
                    "arm": next(row["arm"] for row in rows if row["cohort_id"] == cohort),
                    "replicate_ids": sorted(
                        {
                            row["replicate_id"]
                            for row in rows
                            if row["cohort_id"] == cohort
                        }
                    ),
                }
                for cohort in cohort_order(rows)
            ],
            "replicate_level_rows": rows,
            "replicate_level_summaries": summaries,
            "artifacts": [
                {
                    "path": str(output_dir / path.name),
                    "sha256": sha256(path),
                    "size_bytes": path.stat().st_size,
                }
                for path in artifacts
            ],
        }
        merged_path.write_text(
            yaml.safe_dump(merged, sort_keys=False, width=110), encoding="utf-8"
        )

        output_dir.mkdir(parents=True, exist_ok=True)
        staged_in_publish_order = [*artifacts, merged_path]
        if not force:
            raced = [
                output_dir / path.name
                for path in staged_in_publish_order
                if (output_dir / path.name).exists()
            ]
            require(
                not raced,
                "outputs appeared while staging. Refusing to replace: "
                + ", ".join(map(str, raced)),
            )
        published: list[Path] = []
        for staged_path in staged_in_publish_order:
            published_path = output_dir / staged_path.name
            staged_path.replace(published_path)
            published.append(published_path)

    # The merged manifest is intentionally published last.  Return it first to
    # retain the command-line interface used by the earlier finalizer.
    published_merged = output_dir / "run_l_followup_merged.yaml"
    published_artifacts = [
        path for path in published if path.name != published_merged.name
    ]
    return [published_merged, *published_artifacts]


def synthetic_record(lanes: int, eligible: bool, exact: bool) -> dict[str, Any]:
    """Build one internally consistent synthetic first-path record."""
    if not eligible:
        return {
            "lanes": lanes, "stage": "dp", "call_id": 1,
            "has_ranked_end": False, "candidate_end_count": 0,
            "start_score": None, "min_score": 40, "loop_evaluations": 0,
            "bounds_pass": 0, "fast_batches": 0, "primary_path_anchors": 0,
            "fast_covered_anchors": 0, "scalar_fallbacks": 0,
            "contiguous_mismatch": 0, "emulation_error": 0,
            "scalar_validation_error": 0, "validation_errors": [],
            "rates": {
                "predicate_true_per_loop_evaluation": {
                    "defined": False, "numerator": 0, "denominator": 0,
                    "fraction": None, "percent": None,
                },
                "predicate_true_per_bounds_passing_evaluation": {
                    "defined": False, "numerator": 0, "denominator": 0,
                    "fraction": None, "percent": None,
                },
                "fast_path_anchor_coverage": {
                    "defined": False, "numerator": 0, "denominator": 0,
                    "fraction": None, "percent": None,
                },
            },
        }
    loops, bounds, fast = (5, 4, 3) if exact else (5, 4, 0)
    scalar_fallbacks = loops - fast
    contiguous_mismatch = bounds - fast
    path = lanes * fast + 2 if exact else 5
    return {
        "lanes": lanes, "stage": "dp", "call_id": 1,
        "has_ranked_end": True, "candidate_end_count": 8,
        "start_score": 90, "min_score": 40, "loop_evaluations": loops,
        "bounds_pass": bounds, "fast_batches": fast, "primary_path_anchors": path,
        "fast_covered_anchors": lanes * fast, "scalar_fallbacks": scalar_fallbacks,
        "contiguous_mismatch": contiguous_mismatch, "emulation_error": 0,
        "scalar_validation_error": 0, "validation_errors": [],
        "rates": {
            "predicate_true_per_loop_evaluation": {
                "defined": True, "numerator": fast, "denominator": loops,
                "fraction": fast / loops, "percent": 100 * fast / loops,
            },
            "predicate_true_per_bounds_passing_evaluation": {
                "defined": True, "numerator": fast, "denominator": bounds,
                "fraction": fast / bounds, "percent": 100 * fast / bounds,
            },
            "fast_path_anchor_coverage": {
                "defined": True, "numerator": lanes * fast, "denominator": path,
                "fraction": lanes * fast / path, "percent": 100 * lanes * fast / path,
            },
        },
    }


def run_self_test() -> None:
    """Exercise merging, null preservation, CSV, YAML, PDF, PNG, and SVG output."""
    with tempfile.TemporaryDirectory(prefix="runl-finalizer-test-") as directory:
        root = Path(directory)
        runs: list[dict[str, Any]] = []
        for replicate, seed in (("replicate_001", 11), ("replicate_002", 22)):
            for occurrence in (20, 100):
                eligible = occurrence == 100
                records = {str(lanes): synthetic_record(lanes, eligible, exact=True) for lanes in LANES}
                runs.append(
                    {
                        "status": "complete",
                        "run_id": f"fixture__{replicate}__N{occurrence}",
                        "arm": "dense_default_filter_4p5m_1kb_holdout",
                        "replicate_id": replicate,
                        "parameters": {"occurrence_count": occurrence, "k": 15, "w": 10},
                        "dataset": {
                            "master_seed": seed, "reference_length": 4_500_000,
                            "query_length": 1_000, "copy_fraction": occurrence / 4500,
                        },
                        "endpoint_eligibility": {
                            "has_score_qualified_endpoint": eligible,
                            "candidate_end_count": 8 if eligible else 0,
                            "start_score": 90 if eligible else None, "min_score": 40,
                        },
                        "map_diagnostics": [{
                            "query_length": 1000, "raw_query_minimizers": 180,
                            "retained_anchor_count": 178 * occurrence, "mid_occ": occurrence + 1,
                        }],
                        "index_diagnostics": {"distinct_reference_minimizers": 800_000},
                        "summary": {"initial_chaining_by_lane": records},
                    }
                )
        document = {
            "schema_version": 2,
            "experiment": {"status": "complete", "missing_run_count": 0, "failed_run_count": 0},
            "validations": {"all_observed_run_l_counter_identities_pass": True},
            "runs": runs,
        }
        fixture = root / "fixture.yaml"
        fixture.write_text(yaml.safe_dump(document, sort_keys=False), encoding="utf-8")
        for defect in ("empty_lane", "bad_rate_denominator"):
            malformed = yaml.safe_load(yaml.safe_dump(document))
            lane_record = malformed["runs"][0]["summary"]["initial_chaining_by_lane"]["4"]
            if defect == "empty_lane":
                malformed["runs"][0]["summary"]["initial_chaining_by_lane"]["4"] = {}
            else:
                lane_record["rates"]["predicate_true_per_loop_evaluation"][
                    "denominator"
                ] += 1
            try:
                validate_document(
                    malformed,
                    fixture,
                    "pilot",
                    allow_partial=False,
                )
            except ValueError:
                pass
            else:
                raise ValueError(f"self-test accepted malformed telemetry: {defect}")
        for bad_role, bad_schema in (("pilot", 1), ("control", 1), ("original", 2)):
            try:
                validate_document(
                    {"schema_version": bad_schema, "runs": []},
                    fixture,
                    bad_role,
                    allow_partial=False,
                )
            except ValueError:
                pass
            else:
                raise ValueError(
                    f"self-test accepted schema {bad_schema} for role {bad_role}"
                )
        output = root / "out"
        produced = create_outputs(
            [("compact_holdout", "synthetic", fixture)], output, allow_partial=False, force=False
        )
        require(len(produced) == 14, f"expected 14 artifacts, received {len(produced)}")
        require(all(path.is_file() and path.stat().st_size > 0 for path in produced), "empty artifact")
        merged = yaml.safe_load((output / "run_l_followup_merged.yaml").read_text(encoding="utf-8"))
        null_rows = [
            row for row in merged["replicate_level_rows"]
            if row["occurrence_count"] == 20
        ]
        require(null_rows and all(row["exact_run_l_rate_percent"] is None for row in null_rows), "null rate was lost")
        require(all(not row["score_gate_eligible"] for row in null_rows), "eligibility was altered")
        normalized_rows = merged["replicate_level_rows"]
        holdout_id = normalized_rows[0]["cohort_id"]
        x, median, _q1, _q3 = metric_series(
            normalized_rows, holdout_id, LANES[0], "exact_run_l_rate_percent"
        )
        require(x.tolist() == [20.0, 100.0], "sampled x coordinates changed")
        require(math.isnan(median[0]), "undefined metric was not preserved as a NaN gap")
        require(math.isfinite(median[1]), "defined metric was lost")
        offset_rows: list[dict[str, Any]] = []
        offset_cohorts = (
            "offset_a",
            "offset_b",
            "offset_c",
            "offset_d",
            "offset_e",
        )
        for cohort in offset_cohorts:
            for occurrence in (20, 4000):
                offset_rows.append(
                    {
                        "cohort_id": cohort,
                        "cohort_label": cohort,
                        "source_role": "control",
                        "occurrence_count": occurrence,
                        "lane_width": LANES[0],
                        "replicate_id": "replicate_001",
                    }
                )
        offsets = [
            endpoint_marker_offset_points(offset_rows, cohort, LANES[0])
            for cohort in offset_cohorts
        ]
        require(
            offsets == [-10.0, -5.0, 0.0, 5.0, 10.0],
            f"unexpected fixed endpoint offsets: {offsets}",
        )
        transform_figure, transform_axis = plt.subplots(figsize=(1.8, 3.0))
        transform_figure.subplots_adjust(left=0.15, right=0.95)
        display_positions = [
            endpoint_marker_transform(
                transform_axis, offset_rows, cohort, LANES[0]
            ).transform([[20.0, 0.0]])[0, 0]
            for cohort in offset_cohorts
        ]
        expected_pixel_step = (
            ENDPOINT_MARKER_OFFSET_POINTS * transform_figure.dpi / 72.0
        )
        require(
            np.allclose(np.diff(display_positions), expected_pixel_step),
            "endpoint display transforms do not preserve the requested point spacing",
        )
        domain_rows = list(offset_rows)
        for occurrence in (1, 5000):
            domain_rows.append(
                {
                    "cohort_id": "domain_context",
                    "cohort_label": "domain context",
                    "source_role": "original",
                    "occurrence_count": occurrence,
                    "lane_width": LANES[0],
                    "replicate_id": "original_single_replicate",
                }
            )
        configure_axis(transform_axis, domain_rows, LANES[0], "test metric")
        lower_limit, upper_limit = transform_axis.get_xlim()
        require(
            lower_limit < 1.0 and upper_limit > 5000.0,
            f"configured x-domain clips sampled N values: {(lower_limit, upper_limit)}",
        )
        plt.close(transform_figure)
        require(
            not list(root.glob(".out.staging-*")),
            "staging directory was not cleaned up",
        )
    print(
        "self-test passed: strict schema, null gaps, aggregation, atomic staging, "
        "CSV, YAML, PDF, PNG, and SVG"
    )


def build_parser() -> argparse.ArgumentParser:
    """Construct the command-line interface."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compact-holdout", type=Path, help="final compact holdout schema-v2 YAML")
    parser.add_argument(
        "--pilot", action="append", default=[], metavar="LABEL=PATH",
        help="pilot result to include. Repeatable",
    )
    parser.add_argument(
        "--control", action="append", default=[], metavar="LABEL=PATH",
        help="control result to include. Repeatable",
    )
    parser.add_argument("--original", type=Path, help="optional original schema-v1 result YAML")
    parser.add_argument("--output-dir", type=Path, default=Path("followup_publication"))
    parser.add_argument("--allow-partial", action="store_true", help="permit partial pilot/control fixtures")
    parser.add_argument("--force", action="store_true", help="replace known outputs in output-dir")
    parser.add_argument("--self-test", action="store_true", help="run a synthetic end-to-end test")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Command-line entry point."""
    args = build_parser().parse_args(argv)
    if args.self_test:
        run_self_test()
        return 0
    require(args.compact_holdout is not None, "--compact-holdout is required")
    inputs: list[tuple[str, str, Path]] = [
        ("compact_holdout", "compact_holdout", args.compact_holdout.resolve())
    ]
    inputs.extend(("pilot", label, path) for label, path in map(parse_named_path, args.pilot))
    inputs.extend(("control", label, path) for label, path in map(parse_named_path, args.control))
    if args.original:
        inputs.append(("original", "original_experiment", args.original.resolve()))
    produced = create_outputs(inputs, args.output_dir.resolve(), args.allow_partial, args.force)
    for path in produced:
        print(f"{path}\t{sha256(path)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
