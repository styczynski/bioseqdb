#!/usr/bin/env python3
"""Validate and immutably publish the dense Run_L divergence sweep.

The experiment is run below ``original-minimap2-code``, which is disposable.
This program validates every input and output, copies the complete evidence to
a versioned workspace artifact directory, repeats validation against the
staged copy, and finally publishes a compact top-level YAML index.  Publication
never replaces an existing file or directory.
"""

from __future__ import annotations

import argparse
import csv
import ctypes
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
import errno
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import tempfile
from typing import Any, Iterable, Sequence
import uuid

import yaml


SOURCE_ROOT = Path(__file__).resolve().parent
WORKSPACE_ROOT = SOURCE_ROOT.parent

DEFAULT_DATA_ROOT = SOURCE_ROOT / "dense_divergence_data"
DEFAULT_MANIFEST = DEFAULT_DATA_ROOT / "manifest.json"
DEFAULT_RAW = SOURCE_ROOT / "dense_divergence_raw"
DEFAULT_SHARDS = SOURCE_ROOT / "dense_divergence_shards"
DEFAULT_AGGREGATE = SOURCE_ROOT / "dense_divergence_results.yaml"
DEFAULT_AUDIT = SOURCE_ROOT / "dense_divergence_audit.yaml"
DEFAULT_COORDINATES = SOURCE_ROOT / "dense_divergence_coordinates.tex"
DEFAULT_PROGRESS = SOURCE_ROOT / "dense_divergence_progress.jsonl"
DEFAULT_MINIMAP2 = SOURCE_ROOT / "minimap2"
DEFAULT_ARTIFACT_DIR = (
    WORKSPACE_ROOT / "CHAINING_BRANCHING_DENSE_DIVERGENCE_EXPERIMENT_ARTIFACTS" / "v1"
)
DEFAULT_RESULTS_YAML = (
    WORKSPACE_ROOT / "CHAINING_BRANCHING_DENSE_DIVERGENCE_EXPERIMENT_RESULTS.yaml"
)

EXPECTED_REPLICATES = 3
EXPECTED_GRID_LEVELS = 81
EXPECTED_INDEPENDENT_CONTROLS = 3
GRID_START = Decimal("0")
GRID_STOP = Decimal("0.20")
GRID_STEP = Decimal("0.0025")
PASS_WORDS = {"complete_pass", "validation_passed", "passed", "pass"}
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
VERSION_RE = re.compile(r"^v[1-9][0-9]*$")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-root", type=Path, default=DEFAULT_DATA_ROOT)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--raw", type=Path, default=DEFAULT_RAW)
    parser.add_argument("--shards", type=Path, default=DEFAULT_SHARDS)
    parser.add_argument("--runner-aggregate", type=Path, default=DEFAULT_AGGREGATE)
    parser.add_argument("--audit-summary", type=Path, default=DEFAULT_AUDIT)
    parser.add_argument("--coordinates", type=Path, default=DEFAULT_COORDINATES)
    parser.add_argument("--progress-log", type=Path, default=DEFAULT_PROGRESS)
    parser.add_argument("--minimap2", type=Path, default=DEFAULT_MINIMAP2)
    parser.add_argument("--artifact-dir", type=Path, default=DEFAULT_ARTIFACT_DIR)
    parser.add_argument("--results-yaml", type=Path, default=DEFAULT_RESULTS_YAML)
    parser.add_argument(
        "--source-script",
        type=Path,
        action="append",
        default=[],
        help="additional experiment source to preserve, repeatable",
    )
    parser.add_argument(
        "--expected-replicates", type=int, default=EXPECTED_REPLICATES
    )
    parser.add_argument(
        "--expected-grid-levels", type=int, default=EXPECTED_GRID_LEVELS
    )
    parser.add_argument(
        "--expected-independent-controls",
        type=int,
        default=EXPECTED_INDEPENDENT_CONTROLS,
    )
    parser.add_argument(
        "--validate-only",
        action="store_true",
        help="perform all source validations without copying or publishing",
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
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("ascii")
    return sha256_bytes(encoded)


def load_json(path: Path) -> dict[str, Any]:
    require(path.is_file(), f"JSON file is missing: {path}")
    with path.open("r", encoding="utf-8") as source:
        value = json.load(source)
    require(isinstance(value, dict), f"JSON root is not a mapping: {path}")
    return value


def load_yaml(path: Path) -> dict[str, Any]:
    require(path.is_file(), f"YAML file is missing: {path}")
    with path.open("r", encoding="utf-8") as source:
        value = yaml.safe_load(source)
    require(isinstance(value, dict), f"YAML root is not a mapping: {path}")
    return value


def require_regular_file(path: Path, label: str, *, nonempty: bool = True) -> None:
    require(path.exists(), f"{label} is missing: {path}")
    require(path.is_file(), f"{label} is not a regular file: {path}")
    require(not path.is_symlink(), f"{label} is a symlink: {path}")
    if nonempty:
        require(path.stat().st_size > 0, f"{label} is empty: {path}")


def require_tree(path: Path, label: str) -> None:
    require(path.exists(), f"{label} is missing: {path}")
    require(path.is_dir(), f"{label} is not a directory: {path}")
    require(not path.is_symlink(), f"{label} is a symlink: {path}")
    for item in path.rglob("*"):
        require(not item.is_symlink(), f"{label} contains a symlink: {item}")
        require(
            item.is_dir() or item.is_file(),
            f"{label} contains a non-regular entry: {item}",
        )


def path_below(path: Path, parent: Path, label: str) -> Path:
    try:
        return path.resolve().relative_to(parent.resolve())
    except ValueError as error:
        raise ValueError(f"{label} must be below {parent}: {path}") from error


def safe_relative_path(value: Any, context: str) -> Path:
    require(isinstance(value, str) and value, f"{context}: invalid path")
    relative = Path(value)
    require(not relative.is_absolute(), f"{context}: absolute path is forbidden")
    require(
        relative.parts and all(part not in {"", ".", ".."} for part in relative.parts),
        f"{context}: unsafe relative path {value!r}",
    )
    return relative


def tree_rows(root: Path) -> list[dict[str, Any]]:
    require_tree(root, "tree")
    rows: list[dict[str, Any]] = []
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        rows.append(
            {
                "path": path.relative_to(root).as_posix(),
                "sha256": sha256_file(path),
                "size_bytes": path.stat().st_size,
            }
        )
    return rows


def tree_summary(root: Path) -> dict[str, Any]:
    rows = tree_rows(root)
    return {
        "file_count": len(rows),
        "size_bytes": sum(int(row["size_bytes"]) for row in rows),
        "canonical_manifest_sha256": canonical_json_sha256(rows),
    }


def decimal_level(value: Any, context: str) -> Decimal:
    require(isinstance(value, (str, int, float)), f"{context}: missing level")
    try:
        level = Decimal(str(value))
    except InvalidOperation as error:
        raise ValueError(f"{context}: invalid divergence {value!r}") from error
    require(level.is_finite(), f"{context}: divergence is not finite")
    return level


def expected_grid(level_count: int) -> tuple[Decimal, ...]:
    require(level_count > 0, "expected grid level count must be positive")
    levels = tuple(GRID_START + GRID_STEP * index for index in range(level_count))
    require(levels[-1] == GRID_STOP, "expected grid does not end at 20 percent")
    return levels


def verify_declaration(base: Path, declaration: Any, context: str) -> Path:
    require(isinstance(declaration, dict), f"{context}: declaration is not a mapping")
    relative = safe_relative_path(declaration.get("path"), context)
    path = base / relative
    require_regular_file(path, context, nonempty=False)
    expected_sha = declaration.get("sha256") or declaration.get("file_sha256")
    require(
        isinstance(expected_sha, str) and SHA256_RE.fullmatch(expected_sha),
        f"{context}: invalid SHA-256 declaration",
    )
    require(sha256_file(path) == expected_sha, f"{context}: SHA-256 mismatch")
    if "size_bytes" in declaration:
        require(
            path.stat().st_size == int(declaration["size_bytes"]),
            f"{context}: size mismatch",
        )
    return path


def validate_manifest(
    manifest_path: Path,
    data_root: Path,
    expected_replicates: int,
    expected_grid_levels: int,
    expected_controls: int,
) -> dict[str, Any]:
    require_regular_file(manifest_path, "manifest")
    require_tree(data_root, "generated data root")
    require(manifest_path.parent == data_root, "manifest must be directly in data root")
    manifest = load_json(manifest_path)
    require(manifest.get("schema_version") == 1, "manifest schema version differs")
    require(manifest.get("status") == "complete", "manifest status is not complete")
    require(
        manifest.get("experiment_id") == "runl_repeat_disruption",
        "manifest experiment_id is not runl_repeat_disruption",
    )
    require(
        manifest.get("profile") == "dense_divergence_0_20_step_0p25",
        "manifest profile is not the dense divergence sweep",
    )
    require(
        manifest.get("lane_widths") == [2, 4, 8, 16, 32, 64],
        "manifest lane widths differ",
    )
    seed_plan = manifest.get("seed_plan")
    require(isinstance(seed_plan, dict), "manifest seed_plan is not a mapping")
    require(
        manifest.get("seed_plan_sha256") == canonical_json_sha256(seed_plan),
        "manifest seed plan digest differs",
    )
    cases = manifest.get("cases")
    require(isinstance(cases, list), "manifest cases is not a list")
    expected_case_count = expected_replicates * expected_grid_levels + expected_controls
    require(
        len(cases) == expected_case_count,
        f"manifest has {len(cases)} cases, expected {expected_case_count}",
    )
    case_ids: list[str] = []
    by_replicate: dict[str, dict[str, Any]] = {}
    controls: list[str] = []
    expected_levels = set(expected_grid(expected_grid_levels))
    for case in cases:
        require(isinstance(case, dict), "manifest case is not a mapping")
        case_id = case.get("case_id")
        replicate = case.get("replicate_id")
        require(isinstance(case_id, str) and case_id, "manifest case lacks case_id")
        require(isinstance(replicate, str) and replicate, f"{case_id}: lacks replicate_id")
        require(case.get("group") == "mosaic", f"{case_id}: group is not mosaic")
        params = case.get("parameters")
        require(isinstance(params, dict), f"{case_id}: parameters is not a mapping")
        require(params.get("construction") == "mosaic", f"{case_id}: wrong construction")
        level_value = params.get("similarity_level")
        entry = by_replicate.setdefault(replicate, {"levels": set(), "controls": 0})
        if level_value == "independent_control":
            entry["controls"] += 1
            controls.append(case_id)
            require(
                params.get("substitution_count") is None,
                f"{case_id}: independent control has substitution_count",
            )
            require(
                params.get("is_independent_control") is True
                and params.get("divergence_grid_index") is None,
                f"{case_id}: independent-control annotations differ",
            )
        else:
            level = decimal_level(level_value, case_id)
            require(level in expected_levels, f"{case_id}: level is outside dense grid")
            require(level not in entry["levels"], f"{case_id}: duplicate level in replicate")
            entry["levels"].add(level)
            a_length = int(params.get("a_length", 0))
            substitutions = int(params.get("substitution_count", -1))
            require(a_length > 0, f"{case_id}: invalid a_length")
            expected_substitutions = int(
                (level * a_length).to_integral_value(rounding=ROUND_HALF_UP)
            )
            require(
                substitutions == expected_substitutions,
                f"{case_id}: substitution_count does not use half-up rounding",
            )
            grid_index = int((level / GRID_STEP).to_integral_value())
            require(
                params.get("divergence_grid_index") == grid_index
                and decimal_level(params.get("declared_divergence_fraction"), case_id)
                == level
                and params.get("is_independent_control") is False,
                f"{case_id}: dense-grid annotations differ",
            )
        for key in ("reference", "query", "construction_artifact"):
            verify_declaration(data_root, case.get(key), f"{case_id} {key}")
        case_ids.append(case_id)
    require(len(set(case_ids)) == len(case_ids), "manifest contains duplicate case ids")
    require(
        len(by_replicate) == expected_replicates,
        f"manifest has {len(by_replicate)} replicates, expected {expected_replicates}",
    )
    for replicate, entry in by_replicate.items():
        require(
            entry["levels"] == expected_levels,
            f"{replicate}: does not contain the exact 0--20 percent grid",
        )
        expected_control_count = expected_controls // expected_replicates
        require(
            entry["controls"] == expected_control_count,
            f"{replicate}: independent-control count differs",
        )
    require(
        len(controls) == expected_controls,
        f"manifest has {len(controls)} independent controls, expected {expected_controls}",
    )
    companion = data_root / "manifest.sha256"
    require_regular_file(companion, "manifest checksum companion")
    manifest_sha = sha256_file(manifest_path)
    require(
        companion.read_text(encoding="ascii") == f"{manifest_sha}  manifest.json\n",
        "manifest checksum companion is invalid",
    )
    generation = manifest.get("generation")
    require(isinstance(generation, dict), "manifest generation is not a mapping")
    self_validation = generation.get("self_validation")
    require(isinstance(self_validation, dict), "manifest lacks generator self-validation")
    require(self_validation.get("status") == "pass", "generator self-validation did not pass")
    require(
        int(self_validation.get("case_count", -1)) == expected_case_count
        and int(self_validation.get("numeric_level_count", -1)) == expected_grid_levels
        and self_validation.get("nested_mutation_prefixes") is True
        and self_validation.get("reference_equals_query_for_every_case") is True
        and self_validation.get("unique_case_ids") is True
        and self_validation.get("unique_rounded_substitution_counts") is True,
        "generator self-validation summary differs",
    )
    source_pairs = (
        ("generator", "generator_sha256"),
        ("imported_base_generator", "imported_base_generator_sha256"),
        ("imported_common_helper", "imported_common_helper_sha256"),
    )
    generation_sources: list[Path] = []
    for name_key, digest_key in source_pairs:
        name = generation.get(name_key)
        digest = generation.get(digest_key)
        if name_key != "generator" and name is None and digest is None:
            continue
        require(isinstance(name, str), f"manifest lacks {name_key}")
        source_path = SOURCE_ROOT / safe_relative_path(name, name_key)
        require_regular_file(source_path, f"generation source {name_key}")
        require(
            isinstance(digest, str) and sha256_file(source_path) == digest,
            f"generation source differs from manifest provenance: {name_key}",
        )
        generation_sources.append(source_path)
    return {
        "document": manifest,
        "sha256": manifest_sha,
        "case_ids": tuple(sorted(case_ids)),
        "case_count": len(case_ids),
        "replicates": tuple(sorted(by_replicate)),
        "control_case_ids": tuple(sorted(controls)),
        "generator_path": generation_sources[0],
        "generation_source_paths": tuple(generation_sources),
        "data_tree": tree_summary(data_root),
    }


def validate_progress(path: Path, case_ids: set[str], manifest_sha: str, binary_sha: str) -> dict[str, Any]:
    require_regular_file(path, "progress log")
    records: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as source:
        for line_number, raw in enumerate(source, start=1):
            require(raw.strip(), f"progress log has blank line {line_number}")
            value = json.loads(raw)
            require(isinstance(value, dict), f"progress line {line_number} is not a mapping")
            records.append(value)
    require(records, "progress log is empty")
    starts = [record for record in records if record.get("event") == "runner_start"]
    stops = [record for record in records if record.get("event") == "runner_stop"]
    completes = [record for record in records if record.get("event") == "case_complete"]
    require(starts and stops, "progress log lacks runner boundaries")
    require(stops[-1].get("interrupted") is False, "last runner stop is interrupted")
    for record in starts:
        require(record.get("manifest_sha256") == manifest_sha, "progress manifest digest differs")
        require(record.get("minimap2_sha256") == binary_sha, "progress binary digest differs")
    latest: dict[str, str] = {}
    for record in completes:
        case_id = record.get("case_id")
        require(case_id in case_ids, f"progress has unknown case {case_id!r}")
        latest[str(case_id)] = str(record.get("status"))
    require(set(latest) == case_ids, "progress does not complete every manifest case")
    require(all(value == "accepted" for value in latest.values()), "progress has rejected cases")
    return {
        "record_count": len(records),
        "runner_start_count": len(starts),
        "runner_stop_count": len(stops),
        "completed_case_count": len(latest),
        "sha256": sha256_file(path),
    }


def validate_raw_case(raw_case: Path, shard: dict[str, Any], case_id: str) -> None:
    require_tree(raw_case, f"raw case {case_id}")
    declared: set[Path] = set()
    raw_artifacts = shard.get("raw_artifacts")
    require(isinstance(raw_artifacts, dict), f"{case_id}: raw_artifacts is not a mapping")
    for name, declaration in raw_artifacts.items():
        path = verify_declaration(raw_case, declaration, f"{case_id} raw {name}")
        declared.add(path.relative_to(raw_case))
    validation = shard.get("validation")
    require(isinstance(validation, dict), f"{case_id}: validation is not a mapping")
    dumps = validation.get("dump_artifacts")
    require(isinstance(dumps, list) and dumps, f"{case_id}: lacks dump artifacts")
    for index, declaration in enumerate(dumps):
        path = verify_declaration(raw_case, declaration, f"{case_id} dump {index}")
        declared.add(path.relative_to(raw_case))
    result_path = raw_case / "case_result.json"
    require_regular_file(result_path, f"{case_id} raw case_result")
    declared.add(Path("case_result.json"))
    actual = {
        path.relative_to(raw_case)
        for path in raw_case.rglob("*")
        if path.is_file()
    }
    require(actual == declared, f"{case_id}: raw directory has missing or unexpected files")


def validate_outputs(
    manifest_summary: dict[str, Any],
    aggregate_path: Path,
    shards_root: Path,
    raw_root: Path,
    binary_path: Path,
) -> dict[str, Any]:
    require_regular_file(aggregate_path, "runner aggregate")
    require_regular_file(binary_path, "minimap2 binary")
    mode = binary_path.stat().st_mode
    require(mode & stat.S_IXUSR, "minimap2 binary is not owner-executable")
    require_tree(shards_root, "shard root")
    require_tree(raw_root, "raw root")
    aggregate = load_yaml(aggregate_path)
    require(aggregate.get("schema_version") == 1, "aggregate schema version differs")
    case_ids = set(manifest_summary["case_ids"])
    binary_sha = sha256_file(binary_path)
    require(
        aggregate.get("experiment_id") == "runl_repeat_disruption",
        "aggregate experiment_id differs",
    )
    require(aggregate.get("experiment_status") == "complete_pass", "aggregate did not pass")
    require(
        aggregate.get("lane_widths") == [2, 4, 8, 16, 32, 64],
        "aggregate lane widths differ from the instrumented sweep",
    )
    manifest_decl = aggregate.get("manifest")
    binary_decl = aggregate.get("minimap2")
    require(isinstance(manifest_decl, dict), "aggregate lacks manifest provenance")
    require(isinstance(binary_decl, dict), "aggregate lacks minimap2 provenance")
    require(
        manifest_decl.get("sha256") == manifest_summary["sha256"],
        "aggregate manifest digest differs",
    )
    require(binary_decl.get("sha256") == binary_sha, "aggregate minimap2 digest differs")
    selection = aggregate.get("selection")
    require(isinstance(selection, dict), "aggregate lacks selection")
    require(selection.get("is_full_manifest") is True, "aggregate is not a full-manifest run")
    selected = selection.get("selected_case_ids")
    require(isinstance(selected, list), "aggregate lacks selected case ids")
    require(set(selected) == case_ids and len(selected) == len(case_ids), "aggregate selection differs")
    summary = aggregate.get("summary")
    require(isinstance(summary, dict), "aggregate lacks summary")
    for key in ("manifest_case_count", "selected_case_count", "accepted", "attempted_global"):
        require(int(summary.get(key, -1)) == len(case_ids), f"aggregate summary {key} differs")
    require(int(summary.get("rejected", -1)) == 0, "aggregate has rejected cases")
    require(int(summary.get("pending_global", -1)) == 0, "aggregate has pending cases")
    runs = aggregate.get("runs")
    require(isinstance(runs, list), "aggregate runs is not a list")
    require(len(runs) == len(case_ids), "aggregate run count differs")
    run_by_id: dict[str, dict[str, Any]] = {}
    source_digest_sets: set[tuple[tuple[str, str], ...]] = set()
    for run in runs:
        require(isinstance(run, dict), "aggregate run is not a mapping")
        case_id = run.get("case_id")
        require(case_id in case_ids, f"aggregate has unknown case {case_id!r}")
        require(case_id not in run_by_id, f"aggregate duplicates {case_id}")
        require(run.get("status") == "accepted", f"{case_id}: aggregate status is not accepted")
        process = run.get("process")
        validation = run.get("validation")
        require(isinstance(process, dict), f"{case_id}: lacks process record")
        require(isinstance(validation, dict), f"{case_id}: lacks validation record")
        require(process.get("exit_code") == 0, f"{case_id}: nonzero exit code")
        require(process.get("timed_out") is False, f"{case_id}: timed out")
        require(process.get("interrupted") is False, f"{case_id}: interrupted")
        require(validation.get("accepted") is True, f"{case_id}: validation did not accept")
        require(validation.get("errors") == [], f"{case_id}: validation has errors")
        provenance = run.get("provenance")
        require(isinstance(provenance, dict), f"{case_id}: lacks provenance")
        require(
            provenance.get("manifest_sha256") == manifest_summary["sha256"],
            f"{case_id}: manifest provenance differs",
        )
        require(
            provenance.get("minimap2_sha256") == binary_sha,
            f"{case_id}: binary provenance differs",
        )
        source_digests = provenance.get("analysis_source_sha256")
        require(isinstance(source_digests, dict) and source_digests, f"{case_id}: lacks source digests")
        source_digest_sets.add(tuple(sorted((str(k), str(v)) for k, v in source_digests.items())))
        run_by_id[str(case_id)] = run
    require(set(run_by_id) == case_ids, "aggregate does not contain every case")
    require(len(source_digest_sets) == 1, "aggregate cases used different analysis sources")
    source_digests = dict(next(iter(source_digest_sets)))
    for name, digest in source_digests.items():
        require(SHA256_RE.fullmatch(digest) is not None, f"invalid source digest for {name}")
        source_path = SOURCE_ROOT / safe_relative_path(name, "analysis source")
        require_regular_file(source_path, f"analysis source {name}")
        require(sha256_file(source_path) == digest, f"analysis source differs: {name}")

    shard_files = sorted(path for path in shards_root.rglob("*") if path.is_file())
    require(
        all(path.parent == shards_root and path.suffix == ".json" for path in shard_files)
        and {path.stem for path in shard_files} == case_ids
        and len(shard_files) == len(case_ids),
        "shard file set differs from manifest cases",
    )
    raw_dirs = {path.name for path in raw_root.iterdir() if path.is_dir()}
    raw_files = [path for path in raw_root.iterdir() if path.is_file()]
    require(not raw_files, "raw root contains files outside case directories")
    require(raw_dirs == case_ids, "raw case directory set differs from manifest cases")
    for shard_path in shard_files:
        case_id = shard_path.stem
        shard = load_json(shard_path)
        require(shard.get("case_id") == case_id, f"{case_id}: shard identity differs")
        require(shard == run_by_id[case_id], f"{case_id}: shard differs from aggregate run")
        raw_case = raw_root / case_id
        validate_raw_case(raw_case, shard, case_id)
        require(
            (raw_case / "case_result.json").read_bytes() == shard_path.read_bytes(),
            f"{case_id}: raw case_result differs byte-for-byte from shard",
        )
    return {
        "document": aggregate,
        "sha256": sha256_file(aggregate_path),
        "case_count": len(case_ids),
        "lane_widths": aggregate.get("lane_widths"),
        "resource_policy": aggregate.get("resource_policy"),
        "summary": summary,
        "binary_sha256": binary_sha,
        "binary_size_bytes": binary_path.stat().st_size,
        "binary_mode_octal": oct(stat.S_IMODE(mode)),
        "analysis_source_sha256": source_digests,
        "shard_tree": tree_summary(shards_root),
        "raw_tree": tree_summary(raw_root),
    }


def status_values(value: Any) -> Iterable[str]:
    if isinstance(value, dict):
        for key, child in value.items():
            if str(key).lower() in {"status", "audit_status", "verdict"}:
                yield str(child).lower()
            yield from status_values(child)
    elif isinstance(value, list):
        for child in value:
            yield from status_values(child)


def validate_audit(
    path: Path,
    manifest_sha: str,
    aggregate_sha: str,
    coordinates_sha: str,
    binary_sha: str,
    expected_case_count: int,
    runner_source_digests: dict[str, str],
    manifest_generator_sha: str,
) -> dict[str, Any]:
    require_regular_file(path, "audit summary")
    audit = load_yaml(path)
    require(audit.get("schema_version") == 1, "audit schema version differs")
    require(
        audit.get("experiment_id") == "runl_dense_divergence_audit",
        "audit experiment_id differs",
    )
    require(audit.get("status") == "complete_pass", "audit status is not complete_pass")
    statuses = set(status_values(audit))
    inputs = audit.get("inputs")
    design = audit.get("design")
    validation = audit.get("validation")
    provenance = audit.get("provenance")
    require(isinstance(inputs, dict), "audit lacks input provenance")
    require(isinstance(design, dict), "audit lacks design summary")
    require(isinstance(validation, dict), "audit lacks validation summary")
    require(isinstance(provenance, dict), "audit lacks source provenance")
    require(inputs.get("manifest_sha256") == manifest_sha, "audit manifest digest differs")
    require(
        inputs.get("runner_aggregate_sha256") == aggregate_sha,
        "audit runner aggregate digest differs",
    )
    require(inputs.get("coordinates_sha256") == coordinates_sha, "audit coordinate digest differs")
    require(inputs.get("minimap2_sha256") == binary_sha, "audit minimap2 digest differs")
    require(int(design.get("replicate_count", -1)) == 3, "audit replicate count differs")
    require(int(design.get("nested_level_count", -1)) == 81, "audit grid count differs")
    require(
        int(design.get("accepted_case_count", -1)) == expected_case_count,
        "audit accepted case count differs",
    )
    require(float(design.get("nominal_divergence_start_percent", -1)) == 0.0, "audit grid start differs")
    require(float(design.get("nominal_divergence_stop_percent", -1)) == 20.0, "audit grid stop differs")
    require(float(design.get("nominal_divergence_step_percent", -1)) == 0.25, "audit grid step differs")
    for key in (
        "manifest_and_input_digests",
        "aggregate_shard_and_raw_bindings",
        "exact_full_diagonal_expectations",
        "qualified_paf_expectations",
        "query_and_reference_filter_expectations",
        "independent_Run_4_replay_from_predecessor_dumps",
    ):
        require(validation.get(key) == "passed", f"audit validation did not pass: {key}")
    require(validation.get("all_cases_accepted") is True, "audit did not accept all cases")
    require(
        validation.get("nested_mutation_sets_verified_from_fasta") is True,
        "audit did not verify nested mutation sets",
    )
    auditor_path = SOURCE_ROOT / "runl_dense_divergence_audit.py"
    require_regular_file(auditor_path, "audit source")
    require(
        provenance.get("auditor_sha256") == sha256_file(auditor_path),
        "audit source digest differs",
    )
    audit_runner_sources = provenance.get("runner_analysis_source_sha256")
    require(
        isinstance(audit_runner_sources, dict) and audit_runner_sources,
        "audit lacks runner source digests",
    )
    normalized_audit_sources = {
        str(name): str(digest) for name, digest in audit_runner_sources.items()
    }
    require(
        normalized_audit_sources == runner_source_digests,
        "audit runner source digest mapping differs",
    )
    require(
        provenance.get("manifest_generator_sha256") == manifest_generator_sha,
        "audit manifest generator digest differs",
    )
    return {
        "document": audit,
        "sha256": sha256_file(path),
        "passing_status_values": sorted(statuses & PASS_WORDS),
        "coordinates_sha256_bound": True,
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


def source_tree_provenance(root: Path, *, exact_archive: bool = False) -> dict[str, Any]:
    require(root.is_dir(), f"minimap2 source root is not a directory: {root}")
    require(not root.is_symlink(), f"minimap2 source root is a symlink: {root}")
    if exact_archive:
        require_tree(root, "archived minimap2 source root")
    selected = minimap_source_paths(root)
    require(selected, "minimap2 source selection is empty")
    require(
        all(path.is_file() and not path.is_symlink() for path in selected),
        "minimap2 source selection contains a non-regular file or symlink",
    )
    if exact_archive:
        all_files = sorted(path for path in root.rglob("*") if path.is_file())
        require(all_files == selected, "archived minimap2 source has unexpected files")
    rows = [
        {
            "path": path.name,
            "sha256": sha256_file(path),
            "size_bytes": path.stat().st_size,
        }
        for path in selected
    ]
    names = {row["path"] for row in rows}
    for required in ("lchain.c", "map.c", "minimap.h", "Makefile", "LICENSE.txt"):
        require(required in names, f"minimap2 source selection lacks {required}")
    return {
        "file_count": len(rows),
        "size_bytes": sum(int(row["size_bytes"]) for row in rows),
        "canonical_manifest_sha256": canonical_json_sha256(rows),
        "files": rows,
    }


def source_scripts(
    manifest_summary: dict[str, Any], output_summary: dict[str, Any], extra: Sequence[Path]
) -> list[Path]:
    paths: set[Path] = {
        Path(__file__).resolve(),
    }
    paths.update(path.resolve() for path in manifest_summary["generation_source_paths"])
    for name in output_summary["analysis_source_sha256"]:
        paths.add((SOURCE_ROOT / name).resolve())
    for path in SOURCE_ROOT.glob("runl_dense*.py"):
        if path.is_file():
            require(not path.is_symlink(), f"dense source script is a symlink: {path}")
            paths.add(path.resolve())
    for path in extra:
        require_regular_file(path, "--source-script")
        resolved = path.resolve()
        path_below(resolved, SOURCE_ROOT, "--source-script")
        paths.add(resolved)
    result = sorted(paths, key=lambda path: path.name)
    require(len({path.name for path in result}) == len(result), "source script basenames collide")
    for path in result:
        path_below(path, SOURCE_ROOT, "source script")
        require_regular_file(path, "source script")
    return result


def scripts_provenance(paths: Sequence[Path]) -> list[dict[str, Any]]:
    return [
        {
            "path": path.name,
            "sha256": sha256_file(path),
            "size_bytes": path.stat().st_size,
        }
        for path in paths
    ]


def validate_evidence(
    *,
    data_root: Path,
    manifest: Path,
    raw: Path,
    shards: Path,
    aggregate: Path,
    audit: Path,
    coordinates: Path,
    progress: Path,
    minimap2: Path,
    expected_replicates: int,
    expected_grid_levels: int,
    expected_controls: int,
) -> dict[str, Any]:
    manifest_summary = validate_manifest(
        manifest,
        data_root,
        expected_replicates,
        expected_grid_levels,
        expected_controls,
    )
    output_summary = validate_outputs(
        manifest_summary, aggregate, shards, raw, minimap2
    )
    require_regular_file(coordinates, "coordinate TeX")
    coordinate_text = coordinates.read_text(encoding="utf-8")
    require(coordinate_text.strip(), "coordinate TeX is blank")
    coordinates_sha = sha256_file(coordinates)
    audit_summary = validate_audit(
        audit,
        manifest_summary["sha256"],
        output_summary["sha256"],
        coordinates_sha,
        output_summary["binary_sha256"],
        manifest_summary["case_count"],
        output_summary["analysis_source_sha256"],
        sha256_file(manifest_summary["generator_path"]),
    )
    progress_summary = validate_progress(
        progress,
        set(manifest_summary["case_ids"]),
        manifest_summary["sha256"],
        output_summary["binary_sha256"],
    )
    return {
        "manifest": manifest_summary,
        "outputs": output_summary,
        "audit": audit_summary,
        "coordinates": {
            "sha256": coordinates_sha,
            "size_bytes": coordinates.stat().st_size,
        },
        "progress": progress_summary,
    }


def copy_file_verified(source: Path, destination: Path) -> None:
    require_regular_file(source, "copy source", nonempty=False)
    before = sha256_file(source)
    size = source.stat().st_size
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    require(sha256_file(source) == before, f"source changed during copy: {source}")
    require(sha256_file(destination) == before, f"copy digest differs: {source}")
    require(destination.stat().st_size == size, f"copy size differs: {source}")


def copy_tree_verified(source: Path, destination: Path) -> None:
    require_tree(source, "copy source tree")
    before = tree_summary(source)
    for path in sorted(item for item in source.rglob("*") if item.is_file()):
        copy_file_verified(path, destination / path.relative_to(source))
    require(tree_summary(source) == before, f"source tree changed during copy: {source}")
    require(tree_summary(destination) == before, f"copied tree differs: {source}")


def render_inventory(rows: Sequence[dict[str, Any]]) -> bytes:
    destination = io.StringIO(newline="")
    writer = csv.DictWriter(
        destination,
        fieldnames=("path", "sha256", "size_bytes"),
        lineterminator="\n",
    )
    writer.writeheader()
    writer.writerows(rows)
    return destination.getvalue().encode("utf-8")


def artifact_inventory(root: Path) -> list[dict[str, Any]]:
    excluded = {
        Path("inventory.csv"),
        Path("inventory.sha256"),
        Path("canonical_results.yaml"),
        Path("canonical_results.yaml.sha256"),
    }
    rows: list[dict[str, Any]] = []
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        relative = path.relative_to(root)
        if relative in excluded:
            continue
        rows.append(
            {
                "path": relative.as_posix(),
                "sha256": sha256_file(path),
                "size_bytes": path.stat().st_size,
            }
        )
    return rows


def artifact_reference(artifact_dir: Path, relative: str) -> str:
    return f"{artifact_dir.relative_to(WORKSPACE_ROOT).as_posix()}/{relative}"


def public_document(
    evidence: dict[str, Any],
    artifact_dir: Path,
    inventory_sha: str,
    inventory_count: int,
    source_provenance: dict[str, Any],
) -> dict[str, Any]:
    manifest = evidence["manifest"]
    outputs = evidence["outputs"]
    return {
        "schema_version": 1,
        "experiment_id": "runl_dense_divergence_sweep",
        "experiment_status": "complete_pass",
        "design": {
            "divergence_unit": "fraction of substituted bases in A-prime",
            "nominal_start": str(GRID_START),
            "nominal_stop": str(GRID_STOP),
            "nominal_step": str(GRID_STEP),
            "grid_level_count_per_replicate": (
                manifest["case_count"] - len(manifest["control_case_ids"])
            )
            // len(manifest["replicates"]),
            "replicate_count": len(manifest["replicates"]),
            "nested_case_count": manifest["case_count"] - len(manifest["control_case_ids"]),
            "independent_control_count": len(manifest["control_case_ids"]),
            "total_case_count": manifest["case_count"],
        },
        "validation": {
            "manifest_status": "complete",
            "aggregate_status": "complete_pass",
            "all_cases_accepted": True,
            "manifest_shard_aggregate_case_sets_equal": True,
            "aggregate_runs_equal_shards": True,
            "raw_case_results_equal_shards_byte_for_byte": True,
            "all_declared_raw_hashes_verified": True,
            "all_declared_dump_hashes_verified": True,
            "audit_passing_status_values": evidence["audit"]["passing_status_values"],
            "audit_coordinates_sha256_bound": evidence["audit"]["coordinates_sha256_bound"],
        },
        "runner": {
            "lane_widths": outputs["lane_widths"],
            "resource_policy": outputs["resource_policy"],
            "summary": outputs["summary"],
        },
        "audit": evidence["audit"]["document"],
        "provenance": {
            **source_provenance,
            "archive_paths": {
                "minimap2_binary": artifact_reference(artifact_dir, "binary/minimap2"),
                "minimap2_source_tree": artifact_reference(
                    artifact_dir, "sources/minimap2"
                ),
                "experiment_sources": artifact_reference(
                    artifact_dir, "sources/experiment"
                ),
                "source_provenance": artifact_reference(
                    artifact_dir, "sources/provenance.yaml"
                ),
            },
        },
        "artifact_root": {
            "path": artifact_dir.relative_to(WORKSPACE_ROOT).as_posix(),
            "inventory": artifact_reference(artifact_dir, "inventory.csv"),
            "inventory_sha256": inventory_sha,
            "inventory_entry_count": inventory_count,
        },
        "evidence": {
            "data": artifact_reference(artifact_dir, "evidence/data"),
            "manifest": artifact_reference(artifact_dir, "evidence/data/manifest.json"),
            "manifest_sha256": manifest["sha256"],
            "raw": artifact_reference(artifact_dir, "evidence/raw"),
            "raw_tree": outputs["raw_tree"],
            "shards": artifact_reference(artifact_dir, "evidence/shards"),
            "shard_tree": outputs["shard_tree"],
            "runner_aggregate": artifact_reference(
                artifact_dir, "evidence/runner_aggregate.yaml"
            ),
            "runner_aggregate_sha256": outputs["sha256"],
            "audit_summary": artifact_reference(artifact_dir, "evidence/audit_summary.yaml"),
            "audit_summary_sha256": evidence["audit"]["sha256"],
            "coordinates": artifact_reference(artifact_dir, "evidence/coordinates.tex"),
            "coordinates_sha256": evidence["coordinates"]["sha256"],
            "progress_log": artifact_reference(artifact_dir, "evidence/progress.jsonl"),
            "progress_log_sha256": evidence["progress"]["sha256"],
        },
    }


def validate_inventory(root: Path) -> dict[str, Any]:
    require_tree(root, "artifact bundle")
    inventory_path = root / "inventory.csv"
    checksum_path = root / "inventory.sha256"
    canonical_path = root / "canonical_results.yaml"
    canonical_checksum = root / "canonical_results.yaml.sha256"
    for path in (inventory_path, checksum_path, canonical_path, canonical_checksum):
        require_regular_file(path, "published metadata")
    inventory_sha = sha256_file(inventory_path)
    require(
        checksum_path.read_text(encoding="ascii") == f"{inventory_sha}  inventory.csv\n",
        "inventory checksum companion differs",
    )
    canonical_sha = sha256_file(canonical_path)
    require(
        canonical_checksum.read_text(encoding="ascii")
        == f"{canonical_sha}  canonical_results.yaml\n",
        "canonical checksum companion differs",
    )
    with inventory_path.open("r", encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source)
        require(
            reader.fieldnames == ["path", "sha256", "size_bytes"],
            "inventory header differs",
        )
        rows = list(reader)
    declarations: dict[Path, tuple[str, int]] = {}
    for row in rows:
        relative = safe_relative_path(row["path"], "inventory")
        require(relative not in declarations, f"inventory duplicates {relative}")
        digest = row["sha256"]
        require(SHA256_RE.fullmatch(digest) is not None, f"invalid inventory digest for {relative}")
        declarations[relative] = (digest, int(row["size_bytes"]))
    excluded = {
        Path("inventory.csv"),
        Path("inventory.sha256"),
        Path("canonical_results.yaml"),
        Path("canonical_results.yaml.sha256"),
    }
    actual = {
        path.relative_to(root)
        for path in root.rglob("*")
        if path.is_file() and path.relative_to(root) not in excluded
    }
    require(actual == set(declarations), "artifact file set differs from inventory")
    for relative, (digest, size) in declarations.items():
        path = root / relative
        require(path.stat().st_size == size, f"inventory size differs for {relative}")
        require(sha256_file(path) == digest, f"inventory digest differs for {relative}")
    return {
        "inventory_sha256": inventory_sha,
        "inventory_entry_count": len(rows),
        "canonical_results_sha256": canonical_sha,
    }


def fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def fsync_tree(root: Path) -> None:
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        with path.open("rb") as source:
            os.fsync(source.fileno())
    directories = sorted(
        (item for item in root.rglob("*") if item.is_dir()),
        key=lambda path: len(path.parts),
        reverse=True,
    )
    for path in directories:
        fsync_directory(path)
    fsync_directory(root)


def rename_noreplace(source: Path, destination: Path) -> None:
    libc = ctypes.CDLL(None, use_errno=True)
    renameat2 = getattr(libc, "renameat2", None)
    if renameat2 is None:
        raise RuntimeError("atomic renameat2(RENAME_NOREPLACE) is unavailable")
    renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    renameat2.restype = ctypes.c_int
    result = renameat2(
        -100,
        os.fsencode(source),
        -100,
        os.fsencode(destination),
        1,
    )
    if result == 0:
        return
    error = ctypes.get_errno()
    if error == errno.EEXIST:
        raise ValueError(f"refusing to overwrite {destination}")
    raise OSError(error, os.strerror(error), str(destination))


def prepare_root_temp(path: Path, data: bytes) -> tuple[Path, tuple[int, int]]:
    temporary = path.parent / f".{path.name}.tmp.{uuid.uuid4().hex}"
    with temporary.open("xb") as destination:
        destination.write(data)
        destination.flush()
        os.fsync(destination.fileno())
        metadata = os.fstat(destination.fileno())
        identity = (metadata.st_dev, metadata.st_ino)
    return temporary, identity


def publication_lock() -> int:
    lock_path = WORKSPACE_ROOT / ".runl_dense_divergence_publication.lock"
    descriptor = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o600)
    fcntl.flock(descriptor, fcntl.LOCK_EX)
    return descriptor


def inode_identity(path: Path) -> tuple[int, int]:
    metadata = path.stat(follow_symlinks=False)
    return metadata.st_dev, metadata.st_ino


def destination_is_owned(path: Path, identity: tuple[int, int] | None) -> bool:
    if identity is None:
        return False
    try:
        return inode_identity(path) == identity
    except FileNotFoundError:
        return False


def validate_paths(arguments: argparse.Namespace) -> dict[str, Path]:
    paths = {
        "data_root": arguments.data_root.resolve(),
        "manifest": arguments.manifest.resolve(),
        "raw": arguments.raw.resolve(),
        "shards": arguments.shards.resolve(),
        "aggregate": arguments.runner_aggregate.resolve(),
        "audit": arguments.audit_summary.resolve(),
        "coordinates": arguments.coordinates.resolve(),
        "progress": arguments.progress_log.resolve(),
        "minimap2": arguments.minimap2.resolve(),
        "artifact_dir": arguments.artifact_dir.resolve(),
        "results_yaml": arguments.results_yaml.resolve(),
    }
    for key in (
        "data_root",
        "manifest",
        "raw",
        "shards",
        "aggregate",
        "audit",
        "coordinates",
        "progress",
        "minimap2",
    ):
        path_below(paths[key], SOURCE_ROOT, f"--{key.replace('_', '-')}")
    path_below(paths["artifact_dir"], WORKSPACE_ROOT, "--artifact-dir")
    path_below(paths["results_yaml"], WORKSPACE_ROOT, "--results-yaml")
    require(
        not paths["artifact_dir"].is_relative_to(SOURCE_ROOT),
        "artifact directory must be outside disposable source root",
    )
    require(
        not paths["results_yaml"].is_relative_to(SOURCE_ROOT),
        "results YAML must be outside disposable source root",
    )
    require(VERSION_RE.fullmatch(paths["artifact_dir"].name) is not None, "artifact directory must end in vN")
    require(paths["artifact_dir"] != WORKSPACE_ROOT, "artifact destination cannot be workspace root")
    require(paths["results_yaml"].parent == WORKSPACE_ROOT, "results YAML must be at workspace root")
    require(paths["results_yaml"].suffix == ".yaml", "results YAML must have .yaml suffix")
    return paths


def main() -> int:
    arguments = parse_args()
    require(arguments.expected_replicates > 0, "expected replicate count must be positive")
    require(arguments.expected_independent_controls >= 0, "expected control count is negative")
    require(
        arguments.expected_independent_controls % arguments.expected_replicates == 0,
        "independent controls must divide evenly among replicates",
    )
    paths = validate_paths(arguments)
    evidence = validate_evidence(
        data_root=paths["data_root"],
        manifest=paths["manifest"],
        raw=paths["raw"],
        shards=paths["shards"],
        aggregate=paths["aggregate"],
        audit=paths["audit"],
        coordinates=paths["coordinates"],
        progress=paths["progress"],
        minimap2=paths["minimap2"],
        expected_replicates=arguments.expected_replicates,
        expected_grid_levels=arguments.expected_grid_levels,
        expected_controls=arguments.expected_independent_controls,
    )
    scripts = source_scripts(evidence["manifest"], evidence["outputs"], arguments.source_script)
    script_rows = scripts_provenance(scripts)
    minimap_sources = source_tree_provenance(SOURCE_ROOT)
    source_provenance = {
        "minimap2_binary": {
            "sha256": evidence["outputs"]["binary_sha256"],
            "size_bytes": evidence["outputs"]["binary_size_bytes"],
            "mode_octal": evidence["outputs"]["binary_mode_octal"],
        },
        "minimap2_source_tree": minimap_sources,
        "binary_source_relationship": {
            "build_receipt_available": False,
            "classification": "contemporaneous instrumented source snapshot",
            "claim": "The hashes preserve the executable and source state separately. They do not prove that this executable was built from this snapshot.",
        },
        "experiment_sources": script_rows,
    }
    if arguments.validate_only:
        print(
            json.dumps(
                {
                    "status": "validation_passed",
                    "case_count": evidence["manifest"]["case_count"],
                    "replicate_count": len(evidence["manifest"]["replicates"]),
                    "grid_level_count": arguments.expected_grid_levels,
                    "independent_control_count": len(evidence["manifest"]["control_case_ids"]),
                    "manifest_sha256": evidence["manifest"]["sha256"],
                    "runner_aggregate_sha256": evidence["outputs"]["sha256"],
                    "audit_summary_sha256": evidence["audit"]["sha256"],
                    "coordinates_sha256": evidence["coordinates"]["sha256"],
                    "minimap2_binary_sha256": evidence["outputs"]["binary_sha256"],
                    "minimap2_source_tree_sha256": minimap_sources["canonical_manifest_sha256"],
                    "source_script_count": len(scripts),
                },
                sort_keys=True,
            )
        )
        return 0

    lock_fd = publication_lock()
    staging: Path | None = None
    root_temp: Path | None = None
    published_artifact = False
    published_results = False
    artifact_identity: tuple[int, int] | None = None
    results_identity: tuple[int, int] | None = None
    try:
        require(not paths["artifact_dir"].exists(), f"refusing existing artifact destination: {paths['artifact_dir']}")
        require(not paths["results_yaml"].exists(), f"refusing existing results destination: {paths['results_yaml']}")
        paths["artifact_dir"].parent.mkdir(parents=True, exist_ok=True)
        staging = Path(
            tempfile.mkdtemp(
                prefix=f".{paths['artifact_dir'].name}.tmp.",
                dir=paths["artifact_dir"].parent,
            )
        )
        artifact_identity = inode_identity(staging)
        copy_tree_verified(paths["data_root"], staging / "evidence" / "data")
        copy_tree_verified(paths["raw"], staging / "evidence" / "raw")
        copy_tree_verified(paths["shards"], staging / "evidence" / "shards")
        copy_file_verified(paths["aggregate"], staging / "evidence" / "runner_aggregate.yaml")
        copy_file_verified(paths["audit"], staging / "evidence" / "audit_summary.yaml")
        copy_file_verified(paths["coordinates"], staging / "evidence" / "coordinates.tex")
        copy_file_verified(paths["progress"], staging / "evidence" / "progress.jsonl")
        copy_file_verified(paths["minimap2"], staging / "binary" / "minimap2")
        for source in minimap_source_paths(SOURCE_ROOT):
            copy_file_verified(source, staging / "sources" / "minimap2" / source.name)
        for source in scripts:
            copy_file_verified(source, staging / "sources" / "experiment" / source.name)
        provenance_bytes = yaml.safe_dump(
            source_provenance, sort_keys=False, allow_unicode=False, width=120
        ).encode("utf-8")
        provenance_path = staging / "sources" / "provenance.yaml"
        provenance_path.parent.mkdir(parents=True, exist_ok=True)
        provenance_path.write_bytes(provenance_bytes)

        staged_evidence = validate_evidence(
            data_root=staging / "evidence" / "data",
            manifest=staging / "evidence" / "data" / "manifest.json",
            raw=staging / "evidence" / "raw",
            shards=staging / "evidence" / "shards",
            aggregate=staging / "evidence" / "runner_aggregate.yaml",
            audit=staging / "evidence" / "audit_summary.yaml",
            coordinates=staging / "evidence" / "coordinates.tex",
            progress=staging / "evidence" / "progress.jsonl",
            minimap2=staging / "binary" / "minimap2",
            expected_replicates=arguments.expected_replicates,
            expected_grid_levels=arguments.expected_grid_levels,
            expected_controls=arguments.expected_independent_controls,
        )
        require(
            staged_evidence["manifest"]["sha256"] == evidence["manifest"]["sha256"]
            and staged_evidence["outputs"]["sha256"] == evidence["outputs"]["sha256"]
            and staged_evidence["audit"]["sha256"] == evidence["audit"]["sha256"]
            and staged_evidence["coordinates"] == evidence["coordinates"]
            and staged_evidence["progress"] == evidence["progress"],
            "staged evidence differs from source validation",
        )
        archived_source = source_tree_provenance(
            staging / "sources" / "minimap2", exact_archive=True
        )
        require(archived_source == minimap_sources, "archived minimap2 source differs")
        archived_scripts = sorted((staging / "sources" / "experiment").iterdir())
        require(scripts_provenance(archived_scripts) == script_rows, "archived experiment sources differ")
        require(
            load_yaml(provenance_path) == source_provenance,
            "persisted source provenance differs",
        )

        inventory_rows = artifact_inventory(staging)
        inventory_bytes = render_inventory(inventory_rows)
        inventory_path = staging / "inventory.csv"
        inventory_path.write_bytes(inventory_bytes)
        inventory_sha = sha256_bytes(inventory_bytes)
        (staging / "inventory.sha256").write_text(
            f"{inventory_sha}  inventory.csv\n", encoding="ascii"
        )
        document = public_document(
            staged_evidence,
            paths["artifact_dir"],
            inventory_sha,
            len(inventory_rows),
            source_provenance,
        )
        canonical_bytes = yaml.safe_dump(
            document, sort_keys=False, allow_unicode=False, width=120
        ).encode("utf-8")
        canonical_sha = sha256_bytes(canonical_bytes)
        (staging / "canonical_results.yaml").write_bytes(canonical_bytes)
        (staging / "canonical_results.yaml.sha256").write_text(
            f"{canonical_sha}  canonical_results.yaml\n", encoding="ascii"
        )
        persisted = validate_inventory(staging)
        require(persisted["inventory_sha256"] == inventory_sha, "staged inventory digest differs")
        require(persisted["canonical_results_sha256"] == canonical_sha, "staged canonical digest differs")
        require(not paths["artifact_dir"].exists(), "artifact destination appeared during staging")
        require(not paths["results_yaml"].exists(), "results destination appeared during staging")
        fsync_tree(staging)
        root_temp, results_identity = prepare_root_temp(
            paths["results_yaml"], canonical_bytes
        )
        rename_noreplace(staging, paths["artifact_dir"])
        staging = None
        published_artifact = True
        fsync_directory(paths["artifact_dir"].parent)
        rename_noreplace(root_temp, paths["results_yaml"])
        root_temp = None
        published_results = True
        fsync_directory(paths["results_yaml"].parent)
        require(
            sha256_file(paths["results_yaml"]) == canonical_sha,
            "published top-level results digest differs",
        )
        final_check = validate_inventory(paths["artifact_dir"])
        require(final_check == persisted, "published artifact validation differs from staging")
        print(
            json.dumps(
                {
                    "status": "complete_pass",
                    "artifact_dir": str(paths["artifact_dir"]),
                    "results_yaml": str(paths["results_yaml"]),
                    "results_yaml_sha256": canonical_sha,
                    "inventory_sha256": inventory_sha,
                    "inventory_entry_count": len(inventory_rows),
                    "case_count": evidence["manifest"]["case_count"],
                },
                sort_keys=True,
            )
        )
    except BaseException as publication_error:
        rollback_errors: list[str] = []
        if published_results or destination_is_owned(paths["results_yaml"], results_identity):
            try:
                require(
                    results_identity is not None
                    and inode_identity(paths["results_yaml"]) == results_identity,
                    "published results identity changed before rollback",
                )
                rollback_result = (
                    paths["results_yaml"].parent
                    / f".{paths['results_yaml'].name}.rollback.{uuid.uuid4().hex}"
                )
                rename_noreplace(paths["results_yaml"], rollback_result)
                root_temp = rollback_result
                published_results = False
                fsync_directory(paths["results_yaml"].parent)
            except BaseException as rollback_error:
                rollback_errors.append(f"results rollback failed: {rollback_error}")
        if published_artifact or destination_is_owned(paths["artifact_dir"], artifact_identity):
            try:
                require(
                    artifact_identity is not None
                    and inode_identity(paths["artifact_dir"]) == artifact_identity,
                    "published artifact identity changed before rollback",
                )
                rollback_artifact = (
                    paths["artifact_dir"].parent
                    / f".{paths['artifact_dir'].name}.rollback.{uuid.uuid4().hex}"
                )
                rename_noreplace(paths["artifact_dir"], rollback_artifact)
                staging = rollback_artifact
                published_artifact = False
                fsync_directory(paths["artifact_dir"].parent)
            except BaseException as rollback_error:
                rollback_errors.append(f"artifact rollback failed: {rollback_error}")
        if rollback_errors:
            raise RuntimeError(
                "publication failed and rollback was incomplete: "
                + "; ".join(rollback_errors)
            ) from publication_error
        raise
    finally:
        if staging is not None and staging.exists():
            require(
                destination_is_owned(staging, artifact_identity),
                f"refusing to clean a staging directory with changed identity: {staging}",
            )
            shutil.rmtree(staging)
        if root_temp is not None and root_temp.exists():
            require(
                destination_is_owned(root_temp, results_identity),
                f"refusing to clean a result temporary file with changed identity: {root_temp}",
            )
            root_temp.unlink()
        os.close(lock_fd)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
