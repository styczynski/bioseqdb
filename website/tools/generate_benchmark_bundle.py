#!/usr/bin/env python3
"""Discover and stage public benchmark-framework JSON for the static website.

The generated manifest contains only same-origin relative URLs. Report payloads
are copied byte-for-byte after structural and configuration-compatibility
validation. Their names never decide whether they are benchmark reports.
"""

from __future__ import annotations

import argparse
import itertools
import json
import math
import os
import re
import shutil
import stat
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, NoReturn, Sequence
from urllib.parse import quote, unquote, urlsplit


_REPORT_REQUIRED_KEYS = frozenset({
    "meta",
    "raw_evidence",
    "paired_rule_evidence",
    "rule_violations",
    "regressions",
    "sanity_warnings",
    "benchmarks",
})
_REPORT_CLAIM_KEYS = _REPORT_REQUIRED_KEYS - {"meta"}
_REPORT_META_REQUIRED_KEYS = frozenset({
    "commit", "dirty", "timestamp", "created", "fast", "history_eligible",
})
_BENCHMARK_REQUIRED_KEYS = frozenset({
    "title",
    "description",
    "location",
    "test_case_descriptions",
    "execution_tuples",
})
_BENCHMARK_OPTIONAL_KEYS = frozenset({"raw_samples"})
_RAW_EVIDENCE_REQUIRED_KEYS = frozenset({
    "schema",
    "path",
    "run_id",
    "complete",
    "record_count",
    "prefix_bytes",
    "append_order_checksum",
    "schedule_sorted_checksum",
    "outer_e2e_ns",
    "index_and_map_ns",
    "cells",
})
_RAW_EVIDENCE_CELL_REQUIRED_KEYS = frozenset({
    "benchmark",
    "test_case",
    "algorithm",
    "tuple_index",
    "group_index",
    "record_count",
    "schedule_sorted_checksum",
    "outer_e2e_ns",
    "first_repeat_outer_e2e_ns",
    "steady_state_outer_e2e_ns",
    "index_and_map_ns",
})
_RESERVED_CONFIG_KEYS = frozenset({
    "params", "test_cases", "regression_rules",
})
_PRIVATE_PATH_PARTS = frozenset({
    "credentials",
    "debug",
    "private",
    "receipts",
    "scratch",
    "secrets",
    "temp",
    "temporary",
    "tmp",
})
_TEMPORARY_SUFFIXES = (
    ".bak",
    ".incomplete",
    ".orig",
    ".partial",
    ".swp",
    ".temp",
    ".tmp",
    "~",
)
_SENSITIVE_KEYS = frozenset({
    "access_token",
    "api_key",
    "authorization",
    "credentials",
    "gh_token",
    "github_pat",
    "password",
    "passwd",
    "private_key",
    "refresh_token",
    "secret",
    "token",
})
_REMOTE_HOSTS = frozenset({"raw.githubusercontent.com"})
_COMMIT_PATTERN = re.compile(r"^(?:[0-9a-f]{40})?$")
_CHECKSUM_PATTERN = re.compile(r"^[0-9a-f]{16}$")
_RAW_SIDECAR_PATTERN = re.compile(r"^report_[0-9]+\.raw\.jsonl$")
_MAX_JSON_BYTES = 512 * 1024 * 1024
_MAX_EXCLUSION_DIAGNOSTICS = 100
_OWNER_FILENAME = ".bioseq-benchmark-bundle-owner"
_OWNER_PAYLOAD = b"bioseq.website.benchmark_bundle.v1\n"


class BundleError(RuntimeError):
    """Describe one fail-closed benchmark bundle generation error."""


class UnsupportedReport(RuntimeError):
    """Describe a structurally valid report incompatible with public config."""


@dataclass(frozen=True)
class ConfigBenchmark:
    """Hold the public matrix identities required by one benchmark block."""

    source: Path
    tuple_identities: frozenset[str]
    test_cases: frozenset[str]
    definition_identity: str


@dataclass(frozen=True)
class Artifact:
    """Hold one validated source and its deterministic staging identity."""

    kind: str
    source: Path
    repository_relative: Path
    payload: bytes
    document: dict[str, Any]
    report_timestamp: int = 0

    @property
    def manifest_url(self) -> str:
        """Return the encoded same-origin URL used by the generated manifest."""

        relative_url = quote(
            self.repository_relative.as_posix(), safe="/-._~"
        )
        return f"benchmark-data/{relative_url}"


@dataclass(frozen=True)
class DiscoveryResult:
    """Hold validated artifacts and relative-path exclusion diagnostics."""

    configurations: tuple[Artifact, ...]
    reports: tuple[Artifact, ...]
    excluded: tuple[str, ...]


def _fail(message: str) -> NoReturn:
    """Raise a bundle error with a stable command-line diagnostic."""

    raise BundleError(message)


def _unsupported_report(message: str) -> NoReturn:
    """Reject one incompatible report without aborting the complete bundle."""

    raise UnsupportedReport(message)


def _is_integer(value: object) -> bool:
    """Return whether a JSON value is an integer rather than a boolean."""

    return isinstance(value, int) and not isinstance(value, bool)


def _is_finite_number(value: object) -> bool:
    """Return whether a JSON value is a finite non-boolean number."""

    if isinstance(value, bool):
        return False
    if isinstance(value, int):
        return True
    return isinstance(value, float) and math.isfinite(value)


def _canonical_json(value: object) -> str:
    """Return the canonical identity representation used for set comparison."""

    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _strict_json(payload: bytes, display_path: str) -> object:
    """Parse RFC JSON while rejecting duplicate keys and non-finite numbers."""

    if len(payload) > _MAX_JSON_BYTES:
        _fail(
            f"{display_path}: JSON exceeds the {_MAX_JSON_BYTES}-byte public limit"
        )

    def unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                _fail(f"{display_path}: duplicate JSON key {key!r}")
            result[key] = value
        return result

    def reject_constant(value: str) -> NoReturn:
        _fail(f"{display_path}: non-finite JSON number {value!r}")

    def finite_float(value: str) -> float:
        parsed = float(value)
        if not math.isfinite(parsed):
            _fail(f"{display_path}: non-finite JSON number {value!r}")
        return parsed

    try:
        return json.loads(
            payload.decode("utf-8"),
            object_pairs_hook=unique_object,
            parse_constant=reject_constant,
            parse_float=finite_float,
        )
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as error:
        _fail(f"{display_path}: malformed UTF-8 JSON: {error}")


def _contains_sensitive_key(value: object) -> bool:
    """Return whether a JSON tree contains a credential-bearing key."""

    if isinstance(value, dict):
        for key, child in value.items():
            if key.casefold().replace("-", "_") in _SENSITIVE_KEYS:
                return True
            if _contains_sensitive_key(child):
                return True
    elif isinstance(value, list):
        return any(_contains_sensitive_key(child) for child in value)
    return False


def _path_is_private_or_temporary(relative_path: Path) -> bool:
    """Return whether a reporting path is explicitly non-public or temporary."""

    for part in relative_path.parts:
        folded = part.casefold()
        if folded.startswith(".") or folded in _PRIVATE_PATH_PARTS:
            return True
        stem = Path(folded).stem
        if folded.endswith(_TEMPORARY_SUFFIXES) or stem.endswith(
            _TEMPORARY_SUFFIXES
        ):
            return True
    return False


def _resolve_repository_path(
    path: Path,
    repository_root: Path,
    description: str,
    *,
    require_file: bool,
) -> tuple[Path, Path]:
    """Resolve a plain repository-contained file or directory without symlinks."""

    lexical = path.expanduser()
    if not lexical.is_absolute():
        lexical = Path.cwd() / lexical
    lexical = Path(os.path.abspath(lexical))
    repository_alias: Path | None = None
    for ancestor in (lexical, *lexical.parents):
        try:
            if ancestor.resolve(strict=True) == repository_root:
                repository_alias = ancestor
                break
        except OSError:
            continue
    if repository_alias is None:
        _fail(f"{description} escapes the repository root: {path}")
    if repository_alias.is_symlink():
        _fail(f"{description} must not enter through a repository symlink: {path}")
    lexical_relative = lexical.relative_to(repository_alias)

    current = repository_alias
    for part in lexical_relative.parts:
        current = current / part
        if current.is_symlink():
            _fail(
                f"{description} must not traverse a symlink: "
                f"{lexical_relative}"
            )

    try:
        resolved = lexical.resolve(strict=True)
    except OSError as error:
        _fail(f"{description} does not resolve: {path}: {error}")
    try:
        relative = resolved.relative_to(repository_root)
    except ValueError:
        _fail(f"{description} escapes the repository root: {path}")
    if relative != lexical_relative:
        _fail(f"{description} resolves through an unexpected path: {path}")

    if require_file:
        try:
            mode = resolved.stat().st_mode
        except OSError as error:
            _fail(f"{description} cannot be inspected: {relative}: {error}")
        if not stat.S_ISREG(mode):
            _fail(f"{description} is not a regular file: {relative}")
    elif not resolved.is_dir():
        _fail(f"{description} is not a directory: {relative}")
    return resolved, relative


def _claims_report(document: dict[str, Any]) -> bool:
    """Return whether a JSON object claims the benchmark report contract."""

    keys = set(document)
    return "benchmarks" in keys or (
        "meta" in keys and bool(keys & _REPORT_CLAIM_KEYS)
    )


def _claims_configuration(document: dict[str, Any]) -> bool:
    """Return whether a JSON object claims the benchmark configuration contract."""

    if "params" in document or "test_cases" in document:
        return True
    return any(
        isinstance(value, dict) and "matrix" in value
        for value in document.values()
    )


def _validate_scalar(value: object, context: str) -> None:
    """Require one JSON scalar supported by benchmark parameter identities."""

    if not (
        isinstance(value, (str, bool)) or _is_finite_number(value)
    ):
        _fail(f"{context}: expected a string, boolean, or finite number")


def _matrix_tuple_identities(
    document: dict[str, Any],
    benchmark_key: str,
    block: dict[str, Any],
    display_path: str,
) -> tuple[frozenset[str], frozenset[str]]:
    """Expand one configuration matrix into exact report tuple identities."""

    matrix = block.get("matrix")
    if not isinstance(matrix, dict):
        _fail(f"{display_path}: benchmark {benchmark_key!r} has no matrix object")
    over = matrix.get("over")
    iterations = matrix.get("for")
    metrics = matrix.get("metrics")
    if not isinstance(over, dict) or not over:
        _fail(f"{display_path}: benchmark {benchmark_key!r} has no matrix.over")
    if not isinstance(iterations, dict) or not iterations:
        _fail(f"{display_path}: benchmark {benchmark_key!r} has no matrix.for")
    if not isinstance(metrics, dict) or not metrics:
        _fail(f"{display_path}: benchmark {benchmark_key!r} has no matrix.metrics")
    if not isinstance(matrix.get("by_regions"), bool):
        _fail(f"{display_path}: benchmark {benchmark_key!r} has invalid by_regions")

    test_cases = document["test_cases"]
    for test_case, repeats in iterations.items():
        if not isinstance(test_case, str) or not test_case:
            _fail(f"{display_path}: benchmark {benchmark_key!r} has an empty test case")
        if test_case not in test_cases:
            _fail(
                f"{display_path}: benchmark {benchmark_key!r} references unknown "
                f"test case {test_case!r}"
            )
        if not _is_integer(repeats) or repeats < 1:
            _fail(
                f"{display_path}: benchmark {benchmark_key!r} test case "
                f"{test_case!r} has invalid repetitions"
            )

    params = document["params"]
    identities: set[str] = set()
    for algorithm, axes in over.items():
        if not isinstance(algorithm, str) or not algorithm:
            _fail(f"{display_path}: benchmark {benchmark_key!r} has an empty algorithm")
        parameter_specs = params.get(algorithm)
        if not isinstance(parameter_specs, dict):
            _fail(
                f"{display_path}: benchmark {benchmark_key!r} references unknown "
                f"algorithm {algorithm!r}"
            )
        if not isinstance(axes, dict):
            _fail(
                f"{display_path}: benchmark {benchmark_key!r} algorithm "
                f"{algorithm!r} has invalid matrix axes"
            )

        base_parameters: dict[str, object] = {}
        underscore = parameter_specs.get("_")
        if isinstance(underscore, dict) and "default" in underscore:
            _validate_scalar(
                underscore["default"],
                f"{display_path}: params.{algorithm}._.default",
            )
            base_parameters["_"] = underscore["default"]

        axis_names: list[str] = []
        axis_values: list[list[object]] = []
        for axis, values in axes.items():
            if not isinstance(axis, str) or not axis:
                _fail(
                    f"{display_path}: benchmark {benchmark_key!r} has an empty axis"
                )
            if axis not in parameter_specs:
                _fail(
                    f"{display_path}: benchmark {benchmark_key!r} algorithm "
                    f"{algorithm!r} references unknown axis {axis!r}"
                )
            if not isinstance(values, list) or not values:
                _fail(
                    f"{display_path}: benchmark {benchmark_key!r} axis {axis!r} "
                    "must have a non-empty value list"
                )
            for value in values:
                _validate_scalar(
                    value,
                    f"{display_path}: benchmark {benchmark_key!r} axis {axis!r}",
                )
            if len({_canonical_json(value) for value in values}) != len(values):
                _fail(
                    f"{display_path}: benchmark {benchmark_key!r} axis {axis!r} "
                    "contains duplicate values"
                )
            axis_names.append(axis)
            axis_values.append(values)

        combinations = itertools.product(*axis_values) if axis_values else [()]
        for values in combinations:
            concrete = dict(base_parameters)
            concrete.update(zip(axis_names, values, strict=True))
            identity = _canonical_json([algorithm, concrete])
            if identity in identities:
                _fail(
                    f"{display_path}: benchmark {benchmark_key!r} expands a "
                    f"duplicate tuple for {algorithm!r}"
                )
            identities.add(identity)
    return frozenset(identities), frozenset(iterations)


def _validate_configuration(
    artifact: Artifact,
) -> dict[str, ConfigBenchmark]:
    """Validate one supported configuration and return its benchmark matrices."""

    document = artifact.document
    display_path = artifact.repository_relative.as_posix()
    if not isinstance(document.get("params"), dict) or not document["params"]:
        _fail(f"{display_path}: benchmark configuration has no params object")
    if not isinstance(document.get("test_cases"), dict) or not document["test_cases"]:
        _fail(f"{display_path}: benchmark configuration has no test_cases object")
    if not isinstance(document.get("regression_rules"), list):
        _fail(f"{display_path}: benchmark configuration has no regression_rules array")

    matrices: dict[str, ConfigBenchmark] = {}
    for key, value in document.items():
        if key in _RESERVED_CONFIG_KEYS:
            continue
        if not isinstance(key, str) or not key or not isinstance(value, dict):
            _fail(f"{display_path}: invalid benchmark block {key!r}")
        tuple_identities, test_cases = _matrix_tuple_identities(
            document, key, value, display_path
        )
        matrices[key] = ConfigBenchmark(
            source=artifact.source,
            tuple_identities=tuple_identities,
            test_cases=test_cases,
            # The same benchmark key cannot silently inherit different global
            # parameter defaults or test-case definitions from another config.
            definition_identity=_canonical_json(document),
        )
    if not matrices:
        _fail(f"{display_path}: benchmark configuration declares no matrices")
    return matrices


def _validate_aggregate(value: object, context: str) -> None:
    """Validate one browser-consumable aggregated sample cell."""

    if not isinstance(value, dict):
        _fail(f"{context}: aggregate must be an object")
    required = {"samples_count", "stats", "percentiles", "k-best", "histogram"}
    if set(value) != required:
        _fail(f"{context}: aggregate fields do not match the framework schema")
    samples_count = value["samples_count"]
    if not _is_integer(samples_count) or samples_count < 1:
        _fail(f"{context}: samples_count must be a positive integer")

    stats = value["stats"]
    stat_keys = {"min", "max", "mean", "mean90", "sum", "stddev"}
    if not isinstance(stats, dict) or set(stats) != stat_keys:
        _fail(f"{context}: stats fields do not match the framework schema")
    if not all(_is_finite_number(item) for item in stats.values()):
        _fail(f"{context}: stats contains a non-finite number")

    percentiles = value["percentiles"]
    percentile_keys = {"P01", "P20", "P50", "P80", "P99"}
    if not isinstance(percentiles, dict) or set(percentiles) != percentile_keys:
        _fail(f"{context}: percentile fields do not match the framework schema")
    if not all(_is_finite_number(item) for item in percentiles.values()):
        _fail(f"{context}: percentiles contains a non-finite number")

    best = value["k-best"]
    if not isinstance(best, dict) or set(best) != {"top10", "bottom10"}:
        _fail(f"{context}: k-best fields do not match the framework schema")
    for key in ("top10", "bottom10"):
        samples = best[key]
        if (
            not isinstance(samples, list)
            or len(samples) > 10
            or not all(_is_finite_number(item) for item in samples)
        ):
            _fail(f"{context}: {key} is not a finite sample list")
    if len(best["top10"]) != len(best["bottom10"]):
        _fail(f"{context}: k-best lists have different cardinalities")

    histogram = value["histogram"]
    if not isinstance(histogram, list) or len(histogram) != 10:
        _fail(f"{context}: histogram must contain exactly ten buckets")
    finite_count = 0
    for bucket in histogram:
        if not isinstance(bucket, dict) or set(bucket) != {"from", "to", "count"}:
            _fail(f"{context}: histogram bucket fields are invalid")
        if not _is_finite_number(bucket["from"]) or not _is_finite_number(bucket["to"]):
            _fail(f"{context}: histogram bounds must be finite")
        if not _is_integer(bucket["count"]) or bucket["count"] < 0:
            _fail(f"{context}: histogram count must be a non-negative integer")
        finite_count += bucket["count"]
    if finite_count > samples_count:
        _fail(f"{context}: histogram count exceeds samples_count")


def _validate_duration_summary(value: object, context: str) -> None:
    """Validate one raw-sidecar duration summary from the framework schema."""

    keys = {"count", "min", "p50", "mean", "p95"}
    if not isinstance(value, dict) or set(value) != keys:
        _fail(f"{context}: raw duration summary fields are invalid")
    if not _is_integer(value["count"]) or value["count"] < 0:
        _fail(f"{context}: raw duration count must be non-negative")
    for key in ("min", "p50", "mean", "p95"):
        item = value[key]
        if item is not None and (
            not _is_finite_number(item) or item < 0
        ):
            _fail(f"{context}: raw duration {key} must be non-negative or null")


def _validate_raw_evidence(value: object, context: str) -> None:
    """Validate a durable raw-evidence manifest without staging JSONL."""

    if not isinstance(value, dict) or set(value) != _RAW_EVIDENCE_REQUIRED_KEYS:
        _fail(f"{context}: raw_evidence fields do not match the framework schema")
    if value["schema"] != 1 or not isinstance(value["complete"], bool):
        _fail(f"{context}: raw_evidence has an unsupported schema or completion flag")
    if not isinstance(value["path"], str) or not _RAW_SIDECAR_PATTERN.fullmatch(
        value["path"]
    ):
        _fail(f"{context}: raw_evidence path is not a public sidecar basename")
    if not isinstance(value["run_id"], str) or not value["run_id"]:
        _fail(f"{context}: raw_evidence run_id is empty")
    if not _is_integer(value["record_count"]) or value["record_count"] < 0:
        _fail(f"{context}: raw_evidence record_count must be non-negative")
    if not _is_integer(value["prefix_bytes"]) or value["prefix_bytes"] < 1:
        _fail(f"{context}: raw_evidence prefix_bytes must be positive")
    for key in ("append_order_checksum", "schedule_sorted_checksum"):
        if not isinstance(value[key], str) or not _CHECKSUM_PATTERN.fullmatch(
            value[key]
        ):
            _fail(f"{context}: raw_evidence {key} is invalid")
    for key in ("outer_e2e_ns", "index_and_map_ns"):
        _validate_duration_summary(value[key], f"{context}.raw_evidence.{key}")

    cells = value["cells"]
    if not isinstance(cells, list):
        _fail(f"{context}: raw_evidence cells must be an array")
    for index, cell in enumerate(cells):
        cell_context = f"{context}.raw_evidence.cells[{index}]"
        if not isinstance(cell, dict) or set(cell) != _RAW_EVIDENCE_CELL_REQUIRED_KEYS:
            _fail(f"{cell_context}: fields do not match the framework schema")
        for key in ("benchmark", "test_case", "algorithm"):
            if not isinstance(cell[key], str) or not cell[key]:
                _fail(f"{cell_context}.{key}: must be a non-empty string")
        for key in ("tuple_index", "group_index", "record_count"):
            if not _is_integer(cell[key]) or cell[key] < 0:
                _fail(f"{cell_context}.{key}: must be non-negative")
        if not isinstance(cell["schedule_sorted_checksum"], str) or not (
            _CHECKSUM_PATTERN.fullmatch(cell["schedule_sorted_checksum"])
        ):
            _fail(f"{cell_context}: schedule checksum is invalid")
        for key in (
            "outer_e2e_ns",
            "first_repeat_outer_e2e_ns",
            "steady_state_outer_e2e_ns",
            "index_and_map_ns",
        ):
            _validate_duration_summary(cell[key], f"{cell_context}.{key}")


def _exact_object(
    value: object, required: set[str] | frozenset[str], context: str
) -> dict[str, Any]:
    """Return an object only when its keys exactly match a schema definition."""

    if not isinstance(value, dict) or set(value) != set(required):
        _fail(f"{context}: fields do not match the framework schema")
    return value


def _nonempty_string(value: object, context: str) -> None:
    if not isinstance(value, str) or not value:
        _fail(f"{context}: must be a non-empty string")


def _nullable_finite(value: object, context: str) -> None:
    if value is not None and not _is_finite_number(value):
        _fail(f"{context}: must be a finite number or null")


def _validate_param_object(value: object, context: str) -> None:
    if not isinstance(value, dict):
        _fail(f"{context}: parameter map must be an object")
    for key, item in value.items():
        if not isinstance(key, str) or not key:
            _fail(f"{context}: parameter name must be non-empty")
        _validate_scalar(item, f"{context}.{key}")


def _validate_schedule_sample(value: object, context: str) -> None:
    schedule = _exact_object(
        value,
        {"repeat", "order_position", "shuffle_seed"},
        context,
    )
    for key in ("repeat", "order_position"):
        if not _is_integer(schedule[key]) or schedule[key] < 0:
            _fail(f"{context}.{key}: must be a non-negative integer")
    if not isinstance(schedule["shuffle_seed"], str) or not (
        _CHECKSUM_PATTERN.fullmatch(schedule["shuffle_seed"])
    ):
        _fail(f"{context}.shuffle_seed: must be a 16-hex identity")


def _validate_paired_route(value: object, context: str) -> None:
    route = _exact_object(
        value,
        {"algorithm", "params", "tuple_index", "test_case", "region", "metric"},
        context,
    )
    for key in ("algorithm", "test_case", "region", "metric"):
        _nonempty_string(route[key], f"{context}.{key}")
    _validate_param_object(route["params"], f"{context}.params")
    if not _is_integer(route["tuple_index"]) or route["tuple_index"] < 0:
        _fail(f"{context}.tuple_index: must be a non-negative integer")


def _validate_paired_evidence(value: object, context: str) -> None:
    evidence = _exact_object(
        value,
        {
            "location", "benchmark", "rule_name", "test_case", "metric",
            "comparison_mode", "numerator_route", "denominator_route",
            "comparator", "schedule_identity", "repeats", "inputs_present",
            "observed_paired_ratio", "accepted",
        },
        context,
    )
    for key in ("location", "benchmark", "rule_name", "test_case", "metric"):
        _nonempty_string(evidence[key], f"{context}.{key}")
    if evidence["comparison_mode"] != "paired_iteration_ratio":
        _fail(f"{context}.comparison_mode: unsupported paired mode")
    _validate_paired_route(evidence["numerator_route"], f"{context}.numerator_route")
    _validate_paired_route(
        evidence["denominator_route"], f"{context}.denominator_route"
    )

    comparator = _exact_object(
        evidence["comparator"],
        {
            "operator", "threshold_pct", "tolerance_pct", "aggregate",
            "paired_baseline", "warmup_repetitions", "blocking",
        },
        f"{context}.comparator",
    )
    if comparator["operator"] not in {"<", ">", "="}:
        _fail(f"{context}.comparator.operator: unsupported operator")
    for key in ("threshold_pct", "tolerance_pct"):
        value_number = comparator[key]
        if not _is_finite_number(value_number) or not 0 <= value_number <= 100:
            _fail(f"{context}.comparator.{key}: must be between 0 and 100")
    _nonempty_string(comparator["aggregate"], f"{context}.comparator.aggregate")
    if not _is_finite_number(comparator["paired_baseline"]):
        _fail(f"{context}.comparator.paired_baseline: must be finite")
    if (
        not _is_integer(comparator["warmup_repetitions"])
        or comparator["warmup_repetitions"] < 0
    ):
        _fail(f"{context}.comparator.warmup_repetitions: must be non-negative")
    if not isinstance(comparator["blocking"], bool):
        _fail(f"{context}.comparator.blocking: must be a boolean")

    schedule_identity = _exact_object(
        evidence["schedule_identity"],
        {
            "mode", "configured_seed", "benchmark_index", "test_case_index",
            "group_index",
        },
        f"{context}.schedule_identity",
    )
    if schedule_identity["mode"] not in {"interleaved_seeded", "item_major"}:
        _fail(f"{context}.schedule_identity.mode: unsupported mode")
    if not isinstance(schedule_identity["configured_seed"], str) or not (
        _CHECKSUM_PATTERN.fullmatch(schedule_identity["configured_seed"])
    ):
        _fail(f"{context}.schedule_identity.configured_seed: must be 16-hex")
    for key in ("benchmark_index", "test_case_index", "group_index"):
        if not _is_integer(schedule_identity[key]) or schedule_identity[key] < 0:
            _fail(f"{context}.schedule_identity.{key}: must be non-negative")

    repeats = evidence["repeats"]
    if not isinstance(repeats, list) or not repeats:
        _fail(f"{context}.repeats: must be a non-empty array")
    for index, value_repeat in enumerate(repeats):
        repeat_context = f"{context}.repeats[{index}]"
        repeat = _exact_object(
            value_repeat,
            {
                "repeat", "included_after_warmup", "numerator_samples",
                "denominator_samples", "numerator_value", "denominator_value",
                "ratio", "numerator_schedule", "denominator_schedule",
                "schedule_identity_matches",
            },
            repeat_context,
        )
        if not _is_integer(repeat["repeat"]) or repeat["repeat"] < 0:
            _fail(f"{repeat_context}.repeat: must be non-negative")
        if not isinstance(repeat["included_after_warmup"], bool):
            _fail(f"{repeat_context}.included_after_warmup: must be boolean")
        for key in ("numerator_samples", "denominator_samples"):
            samples = repeat[key]
            if not isinstance(samples, list):
                _fail(f"{repeat_context}.{key}: must be an array")
            for sample in samples:
                _nullable_finite(sample, f"{repeat_context}.{key}")
        for key in ("numerator_value", "denominator_value", "ratio"):
            _nullable_finite(repeat[key], f"{repeat_context}.{key}")
        for key in ("numerator_schedule", "denominator_schedule"):
            if repeat[key] is not None:
                _validate_schedule_sample(repeat[key], f"{repeat_context}.{key}")
        if not isinstance(repeat["schedule_identity_matches"], bool):
            _fail(f"{repeat_context}.schedule_identity_matches: must be boolean")

    inputs = evidence["inputs_present"]
    if not isinstance(inputs, list) or len(inputs) != 2 or not all(
        isinstance(item, bool) for item in inputs
    ):
        _fail(f"{context}.inputs_present: must contain two booleans")
    _nullable_finite(evidence["observed_paired_ratio"], f"{context}.observed_paired_ratio")
    if not isinstance(evidence["accepted"], bool):
        _fail(f"{context}.accepted: must be a boolean")


def _validate_violation(value: object, context: str) -> None:
    violation = _exact_object(
        value,
        {
            "location", "benchmark", "rule_name", "execution_tuple", "test_case",
            "metric", "comparison_mode", "warmup_repetitions", "blocking", "args",
            "args_present",
        },
        context,
    )
    for key in ("location", "benchmark", "rule_name", "test_case", "metric"):
        _nonempty_string(violation[key], f"{context}.{key}")
    execution_tuple = violation["execution_tuple"]
    if not isinstance(execution_tuple, dict) or not 1 <= len(execution_tuple) <= 2:
        _fail(f"{context}.execution_tuple: must contain one or two algorithms")
    for algorithm, params in execution_tuple.items():
        _nonempty_string(algorithm, f"{context}.execution_tuple algorithm")
        _validate_param_object(params, f"{context}.execution_tuple.{algorithm}")
    if violation["comparison_mode"] not in {
        "independent_aggregates", "paired_iteration_ratio", "paired_iteration_exact"
    }:
        _fail(f"{context}.comparison_mode: unsupported mode")
    if (
        not _is_integer(violation["warmup_repetitions"])
        or violation["warmup_repetitions"] < 0
    ):
        _fail(f"{context}.warmup_repetitions: must be non-negative")
    if not isinstance(violation["blocking"], bool):
        _fail(f"{context}.blocking: must be a boolean")
    args = violation["args"]
    if not isinstance(args, list) or len(args) != 2 or not all(
        _is_finite_number(item) for item in args
    ):
        _fail(f"{context}.args: must contain two finite numbers")
    present = violation["args_present"]
    if not isinstance(present, list) or len(present) != 2 or not all(
        isinstance(item, bool) for item in present
    ):
        _fail(f"{context}.args_present: must contain two booleans")


def _validate_regression(value: object, context: str) -> None:
    regression = _exact_object(
        value,
        {
            "location", "benchmark", "rule_name", "algorithm_name",
            "violating_tuples", "test_cases", "region", "metric", "aggregate",
            "blocking", "previously", "now", "regression_since",
            "regression_since_commit",
        },
        context,
    )
    for key in (
        "location", "benchmark", "rule_name", "algorithm_name", "region", "metric"
    ):
        _nonempty_string(regression[key], f"{context}.{key}")
    tuples = regression["violating_tuples"]
    if not isinstance(tuples, list) or not tuples:
        _fail(f"{context}.violating_tuples: must be a non-empty array")
    for index, params in enumerate(tuples):
        _validate_param_object(params, f"{context}.violating_tuples[{index}]")
    cases = regression["test_cases"]
    if not isinstance(cases, list) or not cases:
        _fail(f"{context}.test_cases: must be a non-empty array")
    for index, test_case in enumerate(cases):
        _nonempty_string(test_case, f"{context}.test_cases[{index}]")
    if regression["aggregate"] not in {"sum", "min", "max", "p01", "p50", "p99"}:
        _fail(f"{context}.aggregate: unsupported aggregate")
    if not isinstance(regression["blocking"], bool):
        _fail(f"{context}.blocking: must be a boolean")
    for key in ("previously", "now"):
        if not _is_finite_number(regression[key]):
            _fail(f"{context}.{key}: must be finite")
    for key in ("regression_since", "regression_since_commit"):
        if regression[key] is not None and not isinstance(regression[key], str):
            _fail(f"{context}.{key}: must be a string or null")


def _validate_sanity_warning(value: object, context: str) -> None:
    warning = _exact_object(
        value,
        {"type", "metric", "message", "warning_since", "warning_since_commit"},
        context,
    )
    for key in ("type", "metric", "message"):
        _nonempty_string(warning[key], f"{context}.{key}")
    for key in ("warning_since", "warning_since_commit"):
        if warning[key] is not None and not isinstance(warning[key], str):
            _fail(f"{context}.{key}: must be a string or null")


def _validate_raw_sample(value: object, context: str) -> None:
    required = {
        "schema", "benchmark", "test_case", "algorithm", "group_index",
        "group_count", "tuple_index", "repeat", "params", "outer_e2e_ns",
        "raw_record_id", "schedule", "record_checksum",
    }
    if not isinstance(value, dict) or not required.issubset(value):
        _fail(f"{context}: required raw-sample fields are missing")
    if value["schema"] != 1:
        _fail(f"{context}.schema: unsupported raw-sample schema")
    for key in ("benchmark", "test_case", "algorithm"):
        _nonempty_string(value[key], f"{context}.{key}")
    for key in ("group_index", "tuple_index", "repeat", "raw_record_id"):
        if not _is_integer(value[key]) or value[key] < 0:
            _fail(f"{context}.{key}: must be non-negative")
    if not _is_integer(value["group_count"]) or value["group_count"] < 1:
        _fail(f"{context}.group_count: must be positive")
    _validate_param_object(value["params"], f"{context}.params")
    status = value.get("tuple_status")
    if (value.get("payload_schema") == "bioseq.e2e.raw-sample"
            and isinstance(status, dict)
            and _is_integer(status.get("accepted"))
            and status["accepted"] == 0):
        # Rejected E2E executions are durable diagnostic evidence, often with
        # null timings after a connection failure. Do not publish the original
        # report (or rewrite a mixed report to hide its rejected samples).
        _unsupported_report(f"{context}: rejected E2E tuple; not a public result")
    if not _is_integer(value["outer_e2e_ns"]) or value["outer_e2e_ns"] < 0:
        _fail(f"{context}.outer_e2e_ns: must be non-negative")
    if not isinstance(value["record_checksum"], str) or not (
        _CHECKSUM_PATTERN.fullmatch(value["record_checksum"])
    ):
        _fail(f"{context}.record_checksum: must be 16-hex")
    schedule = _exact_object(
        value["schedule"],
        {
            "benchmark_index", "test_case_index", "tuple_index", "group_index",
            "repeat", "mode", "configured_seed", "shuffle_seed", "order_position",
        },
        f"{context}.schedule",
    )
    for key in (
        "benchmark_index", "test_case_index", "tuple_index", "group_index",
        "repeat", "order_position",
    ):
        if not _is_integer(schedule[key]) or schedule[key] < 0:
            _fail(f"{context}.schedule.{key}: must be non-negative")
    if schedule["mode"] not in {"item_major", "interleaved_seeded"}:
        _fail(f"{context}.schedule.mode: unsupported mode")
    for key in ("configured_seed", "shuffle_seed"):
        if not isinstance(schedule[key], str) or not _CHECKSUM_PATTERN.fullmatch(
            schedule[key]
        ):
            _fail(f"{context}.schedule.{key}: must be 16-hex")


def _parse_created(value: object, context: str) -> int:
    """Parse an ISO-8601 report timestamp and return Unix seconds."""

    if not isinstance(value, str) or not value.endswith("Z"):
        _fail(f"{context}: meta.created must be a canonical UTC ISO-8601 string")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        timestamp = int(parsed.timestamp())
    except (OSError, OverflowError, ValueError) as error:
        _fail(f"{context}: meta.created is not ISO-8601: {error}")
    if parsed.tzinfo is None:
        _fail(f"{context}: meta.created must include a timezone")
    return timestamp


def _validate_report(
    artifact: Artifact,
    matrices: dict[str, ConfigBenchmark],
) -> int | None:
    """Validate one report and return a public report timestamp or None."""

    document = artifact.document
    display_path = artifact.repository_relative.as_posix()
    if set(document) != _REPORT_REQUIRED_KEYS:
        missing = sorted(_REPORT_REQUIRED_KEYS - set(document))
        extra = sorted(set(document) - _REPORT_REQUIRED_KEYS)
        _fail(
            f"{display_path}: report fields do not match the supported schema. "
            f"missing={missing}, extra={extra}"
        )

    meta = document["meta"]
    if not isinstance(meta, dict):
        _fail(f"{display_path}: report meta must be an object")
    meta_keys = set(meta)
    if meta_keys != set(_REPORT_META_REQUIRED_KEYS):
        _fail(f"{display_path}: report meta fields do not match the supported schema")
    if not isinstance(meta["commit"], str) or not _COMMIT_PATTERN.fullmatch(meta["commit"]):
        _fail(f"{display_path}: meta.commit is not an empty or 40-hex revision")
    if not isinstance(meta["dirty"], bool) or not isinstance(meta["fast"], bool):
        _fail(f"{display_path}: meta dirty/fast flags must be booleans")
    if not isinstance(meta["history_eligible"], bool):
        _fail(f"{display_path}: meta.history_eligible must be a boolean")
    if not _is_integer(meta["timestamp"]) or meta["timestamp"] < 0:
        _fail(f"{display_path}: meta.timestamp must be a non-negative integer")
    created_timestamp = _parse_created(meta["created"], display_path)
    if abs(created_timestamp - meta["timestamp"]) > 1:
        _fail(f"{display_path}: meta.created and meta.timestamp disagree")
    is_fast_diagnostic = meta["fast"]

    raw_evidence = document["raw_evidence"]
    if raw_evidence is not None:
        _validate_raw_evidence(raw_evidence, display_path)
        if not raw_evidence["complete"]:
            _unsupported_report(
                f"{display_path}: incomplete raw evidence; not a public result"
            )
    finding_validators = {
        "paired_rule_evidence": _validate_paired_evidence,
        "rule_violations": _validate_violation,
        "regressions": _validate_regression,
        "sanity_warnings": _validate_sanity_warning,
    }
    for key, validator in finding_validators.items():
        findings = document[key]
        if not isinstance(findings, list):
            _fail(f"{display_path}: report {key} must be an object array")
        for index, finding in enumerate(findings):
            validator(finding, f"{display_path}.{key}[{index}]")

    benchmarks = document["benchmarks"]
    if not isinstance(benchmarks, dict) or not benchmarks:
        _fail(f"{display_path}: report benchmarks must be a non-empty object")
    for benchmark_key, benchmark in benchmarks.items():
        if benchmark_key not in matrices:
            _unsupported_report(
                f"{display_path}: report benchmark {benchmark_key!r} has no "
                "supported public configuration"
            )
        if not isinstance(benchmark, dict):
            _fail(f"{display_path}: benchmark {benchmark_key!r} must be an object")
        keys = set(benchmark)
        if (
            not _BENCHMARK_REQUIRED_KEYS.issubset(keys)
            or not keys.issubset(_BENCHMARK_REQUIRED_KEYS | _BENCHMARK_OPTIONAL_KEYS)
        ):
            _fail(
                f"{display_path}: benchmark {benchmark_key!r} fields do not "
                "match the framework schema"
            )
        for key in ("title", "description", "location"):
            if not isinstance(benchmark[key], str):
                _fail(
                    f"{display_path}: benchmark {benchmark_key!r}.{key} "
                    "must be a string"
                )
        descriptions = benchmark["test_case_descriptions"]
        if not isinstance(descriptions, dict) or not all(
            isinstance(key, str) and isinstance(value, str)
            for key, value in descriptions.items()
        ):
            _fail(
                f"{display_path}: benchmark {benchmark_key!r} has invalid "
                "test_case_descriptions"
            )
        if "raw_samples" in benchmark:
            raw_samples = benchmark["raw_samples"]
            if not isinstance(raw_samples, list):
                _fail(
                    f"{display_path}: benchmark {benchmark_key!r}.raw_samples "
                    "is invalid"
                )
            for sample_index, raw_sample in enumerate(raw_samples):
                _validate_raw_sample(
                    raw_sample,
                    f"{display_path}: benchmark {benchmark_key!r}.raw_samples"
                    f"[{sample_index}]",
                )

        execution_tuples = benchmark["execution_tuples"]
        if not isinstance(execution_tuples, list) or not execution_tuples:
            _fail(
                f"{display_path}: benchmark {benchmark_key!r} has no execution tuples"
            )
        observed_identities: set[str] = set()
        expected = matrices[benchmark_key]
        for tuple_index, record in enumerate(execution_tuples):
            context = (
                f"{display_path}: benchmark {benchmark_key!r} tuple {tuple_index}"
            )
            if not isinstance(record, dict) or set(record) != {"tuple", "test_cases"}:
                _fail(f"{context}: fields do not match the framework schema")
            concrete_tuple = record["tuple"]
            if not isinstance(concrete_tuple, dict) or len(concrete_tuple) != 1:
                _fail(f"{context}: tuple must contain exactly one algorithm")
            algorithm, parameters = next(iter(concrete_tuple.items()))
            if not isinstance(algorithm, str) or not isinstance(parameters, dict):
                _fail(f"{context}: tuple algorithm/parameters are invalid")
            for parameter, value in parameters.items():
                if not isinstance(parameter, str) or not parameter:
                    _fail(f"{context}: tuple contains an empty parameter name")
                _validate_scalar(value, f"{context}.{parameter}")
            identity = _canonical_json([algorithm, parameters])
            if identity in observed_identities:
                _fail(f"{context}: duplicate execution tuple")
            if identity not in expected.tuple_identities:
                _unsupported_report(
                    f"{context}: execution tuple is incompatible with public config"
                )
            observed_identities.add(identity)

            test_case_blocks = record["test_cases"]
            if (
                not isinstance(test_case_blocks, dict)
                or not test_case_blocks
                or not set(test_case_blocks).issubset(expected.test_cases)
            ):
                _unsupported_report(
                    f"{context}: test-case set is incompatible with public config"
                )
            for test_case, regions in test_case_blocks.items():
                if not isinstance(regions, dict) or not regions:
                    _fail(f"{context}.{test_case}: region set is empty")
                for region, metrics in regions.items():
                    if not isinstance(region, str) or not isinstance(metrics, dict) or not metrics:
                        _fail(f"{context}.{test_case}: invalid region metrics")
                    for metric, aggregate in metrics.items():
                        if not isinstance(metric, str) or not metric:
                            _fail(f"{context}.{test_case}.{region}: empty metric name")
                        _validate_aggregate(
                            aggregate,
                            f"{context}.{test_case}.{region}.{metric}",
                        )
    # Fast runs still claim the supported report schema, so validate them in
    # full above.  They are diagnostic rather than complete public evidence and
    # therefore are deliberately absent from the generated website inventory.
    return None if is_fast_diagnostic else meta["timestamp"]


def _artifact_from_file(
    source: Path,
    repository_root: Path,
    kind: str,
) -> Artifact:
    """Read one contained source and preserve its original bytes."""

    resolved, relative = _resolve_repository_path(
        source, repository_root, f"benchmark {kind}", require_file=True
    )
    if resolved.stat().st_size > _MAX_JSON_BYTES:
        _fail(
            f"{relative}: JSON exceeds the {_MAX_JSON_BYTES}-byte public limit"
        )
    try:
        payload = resolved.read_bytes()
    except OSError as error:
        _fail(f"{relative}: cannot read benchmark JSON: {error}")
    document = _strict_json(payload, relative.as_posix())
    if not isinstance(document, dict):
        _fail(f"{relative}: supported benchmark JSON must be an object")
    return Artifact(
        kind=kind,
        source=resolved,
        repository_relative=relative,
        payload=payload,
        document=document,
    )


def discover(
    repository_root: Path,
    reporting_roots: Sequence[Path],
    configuration_paths: Sequence[Path],
) -> DiscoveryResult:
    """Discover, classify, and validate every public benchmark artifact."""

    root = repository_root.resolve(strict=True)
    if not root.is_dir():
        _fail(f"repository root is not a directory: {repository_root}")

    configurations: list[Artifact] = []
    seen_sources: set[Path] = set()
    for path in configuration_paths:
        artifact = _artifact_from_file(path, root, "configuration")
        if not _claims_configuration(artifact.document) or _claims_report(artifact.document):
            _fail(
                f"{artifact.repository_relative}: configured public input does not "
                "claim exactly the benchmark configuration schema"
            )
        if _contains_sensitive_key(artifact.document):
            _fail(f"{artifact.repository_relative}: configuration contains credentials")
        configurations.append(artifact)
        seen_sources.add(artifact.source)

    candidates: list[Path] = []
    excluded: list[str] = []
    for reporting_root in reporting_roots:
        resolved_root, relative_root = _resolve_repository_path(
            reporting_root,
            root,
            "benchmark reporting root",
            require_file=False,
        )
        for candidate in sorted(resolved_root.rglob("*.json")):
            relative_to_reporting = candidate.relative_to(resolved_root)
            repository_relative = relative_root / relative_to_reporting
            if _path_is_private_or_temporary(relative_to_reporting):
                excluded.append(
                    f"{repository_relative.as_posix()}: private or temporary path"
                )
                continue
            resolved_candidate, _ = _resolve_repository_path(
                candidate,
                root,
                "benchmark reporting candidate",
                require_file=True,
            )
            if resolved_candidate not in seen_sources:
                candidates.append(resolved_candidate)
                seen_sources.add(resolved_candidate)

    reports_pending: list[Artifact] = []
    for candidate in sorted(candidates):
        artifact = _artifact_from_file(candidate, root, "reporting candidate")
        claims_report = _claims_report(artifact.document)
        claims_configuration = _claims_configuration(artifact.document)
        if claims_report and claims_configuration:
            _fail(f"{artifact.repository_relative}: ambiguous report/configuration JSON")
        if not claims_report and not claims_configuration:
            excluded.append(
                f"{artifact.repository_relative.as_posix()}: unsupported JSON schema"
            )
            continue
        if _contains_sensitive_key(artifact.document):
            _fail(f"{artifact.repository_relative}: supported JSON contains credentials")
        if claims_configuration:
            configurations.append(
                Artifact(
                    kind="configuration",
                    source=artifact.source,
                    repository_relative=artifact.repository_relative,
                    payload=artifact.payload,
                    document=artifact.document,
                )
            )
        else:
            reports_pending.append(
                Artifact(
                    kind="report",
                    source=artifact.source,
                    repository_relative=artifact.repository_relative,
                    payload=artifact.payload,
                    document=artifact.document,
                )
            )

    configurations.sort(key=lambda item: item.repository_relative.as_posix())
    matrices: dict[str, ConfigBenchmark] = {}
    for artifact in configurations:
        for benchmark_key, matrix in _validate_configuration(artifact).items():
            previous = matrices.get(benchmark_key)
            if previous is not None and previous.definition_identity != matrix.definition_identity:
                _fail(
                    f"{artifact.repository_relative}: benchmark {benchmark_key!r} "
                    f"conflicts with {previous.source.relative_to(root)}"
                )
            matrices.setdefault(benchmark_key, matrix)
    if not configurations:
        _fail("no supported public benchmark configurations were discovered")

    reports: list[Artifact] = []
    for artifact in reports_pending:
        try:
            timestamp = _validate_report(artifact, matrices)
        except UnsupportedReport as error:
            excluded.append(str(error))
            continue
        if timestamp is None:
            excluded.append(
                f"{artifact.repository_relative.as_posix()}: "
                "fast diagnostic report"
            )
            continue
        reports.append(
            Artifact(
                kind="report",
                source=artifact.source,
                repository_relative=artifact.repository_relative,
                payload=artifact.payload,
                document=artifact.document,
                report_timestamp=timestamp,
            )
        )
    reports.sort(
        key=lambda item: (
            -item.report_timestamp,
            item.repository_relative.as_posix(),
        )
    )
    return DiscoveryResult(
        configurations=tuple(configurations),
        reports=tuple(reports),
        excluded=tuple(sorted(excluded)),
    )


def _manifest_sources(document: object, display_path: str) -> list[str]:
    """Extract and validate a public-raw override source list."""

    if isinstance(document, list):
        entries = document
    elif isinstance(document, dict):
        unknown = set(document) - {"_comment", "sources", "files"}
        if unknown:
            _fail(f"{display_path}: override has unsupported keys {sorted(unknown)}")
        if "sources" in document and "files" in document:
            _fail(f"{display_path}: override must not declare both sources and files")
        entries = document.get("sources", document.get("files", []))
    else:
        _fail(f"{display_path}: override must be a list or object")
    if not isinstance(entries, list) or not entries:
        _fail(f"{display_path}: override has no non-empty source list")

    sources: list[str] = []
    for index, entry in enumerate(entries):
        if isinstance(entry, str):
            source = entry
        elif isinstance(entry, dict) and set(entry).issubset({"url", "path"}):
            if "url" in entry and "path" in entry:
                _fail(f"{display_path}: override entry {index} is ambiguous")
            source = entry.get("url", entry.get("path", ""))
        else:
            _fail(f"{display_path}: override entry {index} is invalid")
        if not isinstance(source, str) or not source:
            _fail(f"{display_path}: override entry {index} is empty")
        try:
            parsed = urlsplit(source)
        except ValueError as error:
            _fail(f"{display_path}: override entry {index} is invalid: {error}")
        decoded_path = unquote(parsed.path)
        decoded_parts = tuple(part for part in decoded_path.split("/") if part)
        try:
            port = parsed.port
        except ValueError:
            port = -1
        revision_is_pinned = (
            len(decoded_parts) >= 4
            and re.fullmatch(r"[0-9a-f]{40}", decoded_parts[2]) is not None
        )
        if (
            parsed.scheme != "https"
            or parsed.hostname not in _REMOTE_HOSTS
            or port is not None
            or parsed.username is not None
            or parsed.password is not None
            or parsed.query
            or parsed.fragment
            or ".." in decoded_parts
            or "\\" in decoded_path
            or "%" in decoded_path
            or "/blob/" in parsed.path
            or not revision_is_pinned
        ):
            _fail(
                f"{display_path}: override entry {index} is not a safe public raw URL"
            )
        sources.append(source)
    if len(set(sources)) != len(sources):
        _fail(f"{display_path}: override contains duplicate sources")
    return sources


def _validated_override(path: Path, repository_root: Path) -> bytes:
    """Validate an optional public-raw manifest and preserve its bytes."""

    del repository_root  # Overrides are build inputs and may live outside source.
    try:
        resolved = path.expanduser().resolve(strict=True)
    except OSError as error:
        _fail(f"benchmark manifest override does not resolve: {path}: {error}")
    if path.is_symlink() or not resolved.is_file():
        _fail(f"benchmark manifest override is not a plain regular file: {path}")
    payload = resolved.read_bytes()
    display_path = resolved.name
    document = _strict_json(payload, display_path)
    _manifest_sources(document, display_path)
    return payload


def _prepare_output_root(
    output_root: Path, repository_root: Path, build_root: Path | None = None,
) -> tuple[Path, Path]:
    """Validate the build-owned output root and return it with its parent."""

    lexical = output_root.expanduser()
    if not lexical.is_absolute():
        lexical = Path.cwd() / lexical
    lexical = Path(os.path.abspath(lexical))
    if lexical.is_symlink():
        _fail(f"benchmark bundle output must not be a symlink: {lexical}")
    lexical.parent.mkdir(parents=True, exist_ok=True)
    parent = lexical.parent.resolve(strict=True)
    resolved = parent / lexical.name
    configured_build_output = False
    try:
        resolved.relative_to(repository_root)
    except ValueError:
        pass
    else:
        if build_root is None:
            _fail("benchmark bundle output must remain outside the repository source tree")
        build = build_root.expanduser().resolve(strict=True)
        cache = build / "CMakeCache.txt"
        if (build == repository_root or resolved == build
                or not resolved.is_relative_to(build)
                or cache.is_symlink() or not cache.is_file()):
            _fail("benchmark bundle output must be below the declared CMake build root")
        owners = [line.split("=", 1)[1] for line in cache.read_text().splitlines()
                  if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL=")]
        if len(owners) != 1 or Path(owners[0]).resolve() != repository_root:
            _fail("benchmark bundle CMake build root belongs to a different source tree")
        configured_build_output = True
    if resolved.exists():
        if resolved.is_symlink() or not resolved.is_dir():
            _fail(f"benchmark bundle output is not a plain directory: {resolved}")
        # Ninja creates parents for declared BYPRODUCTS before invoking us.
        # Only an empty directory in the verified CMake binary tree is a safe
        # first-use case; every nonempty replacement still needs our marker.
        if configured_build_output and not any(resolved.iterdir()):
            return resolved, parent
        owner = resolved / _OWNER_FILENAME
        if owner.is_symlink() or not owner.is_file():
            _fail(
                "refusing to replace an existing benchmark output without "
                f"the bundle ownership marker: {resolved}"
            )
        try:
            owner_payload = owner.read_bytes()
        except OSError as error:
            _fail(f"cannot read benchmark bundle ownership marker: {error}")
        if owner_payload != _OWNER_PAYLOAD:
            _fail(f"benchmark bundle ownership marker is invalid: {resolved}")
        allowed_top_level = {
            _OWNER_FILENAME,
            "benchmark-data",
            "benchmarks-manifest.json",
        }
        unexpected = sorted(
            entry.name for entry in resolved.iterdir()
            if entry.name not in allowed_top_level
        )
        if unexpected:
            _fail(
                f"refusing to replace benchmark output with unknown entries: "
                f"{unexpected}"
            )
        for entry in resolved.rglob("*"):
            if entry.is_symlink() or (not entry.is_dir() and not entry.is_file()):
                _fail(
                    "refusing to replace benchmark output containing a symlink "
                    f"or special file: {entry.relative_to(resolved)}"
                )
    return resolved, parent


def write_bundle(
    output_root: Path,
    repository_root: Path,
    discovery: DiscoveryResult,
    override_path: Path | None,
    build_root: Path | None = None,
) -> None:
    """Replace the generated bundle only after validated byte copies are ready."""

    resolved_output, output_parent = _prepare_output_root(
        output_root, repository_root, build_root
    )
    override_payload = (
        _validated_override(override_path, repository_root)
        if override_path is not None
        else None
    )
    artifacts = (
        ()
        if override_payload is not None
        else (*discovery.configurations, *discovery.reports)
    )
    destinations: set[Path] = set()

    temporary = Path(
        tempfile.mkdtemp(prefix=".bioseq-benchmark-bundle-", dir=output_parent)
    )
    staged_root = temporary / "bundle"
    data_root = staged_root / "benchmark-data"
    try:
        data_root.mkdir(parents=True)
        for artifact in artifacts:
            destination = data_root / artifact.repository_relative
            if destination in destinations:
                _fail(
                    f"duplicate benchmark staging destination: "
                    f"{artifact.repository_relative}"
                )
            destinations.add(destination)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(artifact.payload)
            if destination.read_bytes() != artifact.payload:
                _fail(
                    f"staged benchmark bytes differ: {artifact.repository_relative}"
                )

        manifest_path = staged_root / "benchmarks-manifest.json"
        if override_payload is not None:
            manifest_path.write_bytes(override_payload)
        else:
            manifest = {
                "_comment": (
                    "Generated from validated repository benchmark artifacts. "
                    "Do not edit this build output."
                ),
                "sources": [artifact.manifest_url for artifact in artifacts],
            }
            manifest_path.write_text(
                json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )
        (staged_root / _OWNER_FILENAME).write_bytes(_OWNER_PAYLOAD)

        if resolved_output.exists():
            _prepare_output_root(resolved_output, repository_root, build_root)
            shutil.rmtree(resolved_output)
        os.replace(staged_root, resolved_output)
    finally:
        shutil.rmtree(temporary, ignore_errors=True)


def _parser() -> argparse.ArgumentParser:
    """Build the closed command-line parser for website generation."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository-root", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument("--build-root", type=Path,
                        help="Allow output below this configured CMake binary directory")
    parser.add_argument(
        "--reporting-root", required=True, action="append", type=Path
    )
    parser.add_argument(
        "--configuration", required=True, action="append", type=Path
    )
    parser.add_argument("--manifest-override", type=Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Generate one deterministic benchmark bundle and print a bounded summary."""

    args = _parser().parse_args(argv)
    try:
        repository_root = args.repository_root.resolve(strict=True)
        discovery = discover(
            repository_root,
            args.reporting_root,
            args.configuration,
        )
        write_bundle(
            args.output_root,
            repository_root,
            discovery,
            args.manifest_override,
            args.build_root,
        )
    except (BundleError, OSError) as error:
        print(f"generate_benchmark_bundle: {error}", file=sys.stderr)
        return 2

    summary = {
        "configuration_count": len(discovery.configurations),
        "report_count": len(discovery.reports),
        "excluded_count": len(discovery.excluded),
        "excluded": list(discovery.excluded[:_MAX_EXCLUSION_DIAGNOSTICS]),
        "excluded_omitted": max(
            0, len(discovery.excluded) - _MAX_EXCLUSION_DIAGNOSTICS
        ),
    }
    print(json.dumps(summary, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
