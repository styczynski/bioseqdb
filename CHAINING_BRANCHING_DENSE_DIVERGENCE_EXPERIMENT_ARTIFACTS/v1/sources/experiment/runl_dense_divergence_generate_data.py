#!/usr/bin/env python3
"""Generate the dense 0--20 percent mosaic-divergence Run_L sweep.

The accepted repeat-disruption generator remains the source of sequence and
case construction.  This executable replaces only its mosaic level list for
the duration of generation.  The numeric grid contains every exact fraction
``i / 400`` for ``i = 0, ..., 80``.  These values correspond to divergence
percentages from 0 to 20 in 0.25 percentage-point steps.  One independent
control is added for each of the three established master seeds.
"""

from __future__ import annotations

import argparse
from decimal import Decimal, ROUND_HALF_UP
import json
import os
from pathlib import Path
import shutil
import tempfile
from typing import Any

import runl_repeat_generate_data as base


DEFAULT_OUTPUT = base.ROOT / "repeat_disruption_data_dense_divergence"
MASTER_SEEDS = base.DEFAULT_MASTER_SEEDS
GRID_DENOMINATOR = 400
GRID_LAST_INDEX = 80
GRID_STEP_PERCENT = Decimal("0.25")
EXPECTED_NUMERIC_LEVELS = GRID_LAST_INDEX + 1
EXPECTED_CASES_PER_SEED = EXPECTED_NUMERIC_LEVELS + 1
EXPECTED_CASE_COUNT = len(MASTER_SEEDS) * EXPECTED_CASES_PER_SEED
PROFILE = "dense_divergence_0_20_step_0p25"


def canonical_decimal(value: Decimal) -> str:
    """Return a non-exponential decimal string without redundant zeroes."""

    if value == 0:
        return "0"
    return format(value.normalize(), "f")


DENSE_RATE_LEVELS = tuple(
    canonical_decimal(Decimal(index) / Decimal(GRID_DENOMINATOR))
    for index in range(GRID_LAST_INDEX + 1)
)
DENSE_MOSAIC_LEVELS = (*DENSE_RATE_LEVELS, "independent_control")


def parse_args() -> argparse.Namespace:
    """Parse output options for the fixed three-seed dense sweep."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--replace",
        action="store_true",
        help="archive an existing output directory before publishing the new one",
    )
    return parser.parse_args()


def declared_percent(index: int) -> str:
    """Return the exact percentage represented by one grid index."""

    return format(Decimal(index) * GRID_STEP_PERCENT, ".2f")


def validate_dense_grid() -> tuple[int, ...]:
    """Validate the exact rational grid and its rounded substitution counts."""

    if MASTER_SEEDS != (7_314_159, 8_271_828, 9_161_803):
        raise AssertionError("the dense sweep must use the three established seeds")
    if base.MOSAIC_A_LENGTH != 32_768:
        raise AssertionError("the dense grid assumes the accepted 32,768-base A block")
    if base.MOSAIC_MAX_RATE != Decimal("0.20"):
        raise AssertionError("the imported mutation plan must cover 20 percent")
    if len(DENSE_RATE_LEVELS) != EXPECTED_NUMERIC_LEVELS:
        raise AssertionError("the numeric divergence grid must contain 81 levels")
    if len(set(DENSE_RATE_LEVELS)) != EXPECTED_NUMERIC_LEVELS:
        raise AssertionError("the numeric divergence levels must be unique")

    for index, level in enumerate(DENSE_RATE_LEVELS):
        expected = Decimal(index) / Decimal(GRID_DENOMINATOR)
        if Decimal(level) != expected:
            raise AssertionError(
                f"dense level {index} is {level}, expected exact fraction {index}/400"
            )

    counts = tuple(base.count_for_rate(level) for level in DENSE_RATE_LEVELS)
    if len(set(counts)) != EXPECTED_NUMERIC_LEVELS:
        raise AssertionError("rounded substitution counts must remain unique")
    if any(right <= left for left, right in zip(counts, counts[1:])):
        raise AssertionError("rounded substitution counts must increase at every level")
    expected_maximum = int(
        (
            Decimal(GRID_LAST_INDEX)
            / Decimal(GRID_DENOMINATOR)
            * Decimal(base.MOSAIC_A_LENGTH)
        ).to_integral_value(rounding=ROUND_HALF_UP)
    )
    if counts[0] != 0 or counts[-1] != expected_maximum:
        raise AssertionError("dense grid endpoints do not match 0 and 20 percent")
    return counts


def annotate_cases(cases: list[dict[str, Any]]) -> None:
    """Add exact grid coordinates to the imported case declarations."""

    index_by_level = {level: index for index, level in enumerate(DENSE_RATE_LEVELS)}
    for case in cases:
        parameters = case["parameters"]
        level = parameters["similarity_level"]
        if level == "independent_control":
            parameters.update(
                {
                    "divergence_grid_index": None,
                    "declared_divergence_fraction": None,
                    "declared_divergence_percent": None,
                    "is_independent_control": True,
                }
            )
            continue
        index = index_by_level[level]
        parameters.update(
            {
                "divergence_grid_index": index,
                "declared_divergence_fraction": level,
                "declared_divergence_percent": declared_percent(index),
                "divergence_fraction_exact": {
                    "numerator": index,
                    "denominator": GRID_DENOMINATOR,
                },
                "is_independent_control": False,
            }
        )


def generate_mosaic_cases(
    temporary_root: Path,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Generate only dense mosaic cases while restoring imported module state."""

    original_levels = base.MOSAIC_LEVELS
    cases: list[dict[str, Any]] = []
    replicate_plans: list[dict[str, Any]] = []
    try:
        base.MOSAIC_LEVELS = DENSE_MOSAIC_LEVELS
        for replicate_index, master_seed in enumerate(MASTER_SEEDS, start=1):
            replicate_cases, replicate_plan = base.make_mosaic_cases(
                temporary_root, master_seed, replicate_index
            )
            annotate_cases(replicate_cases)
            cases.extend(replicate_cases)
            replicate_plans.append(replicate_plan)
    finally:
        base.MOSAIC_LEVELS = original_levels
    if base.MOSAIC_LEVELS is not original_levels:
        raise AssertionError("the imported generator level list was not restored")
    return cases, replicate_plans


def build_manifest(temporary_root: Path) -> dict[str, Any]:
    """Generate the fixed dense sweep and return its complete manifest."""

    counts = validate_dense_grid()
    cases, replicate_plans = generate_mosaic_cases(temporary_root)
    seed_plan = {
        "namespace": base.ALGORITHM_NAMESPACE.decode("ascii"),
        "master_seeds": list(MASTER_SEEDS),
        "replicates": replicate_plans,
    }
    generator_path = Path(__file__).resolve()
    base_generator_path = Path(base.__file__).resolve()
    common_helper_path = base.ROOT / "runl_repeat_common.py"
    manifest = {
        "schema_version": base.SCHEMA_VERSION,
        "status": "complete",
        # Keep the accepted experiment identifier for runl_repeat_common compatibility.
        "experiment_id": "runl_repeat_disruption",
        "profile": PROFILE,
        "created_at_utc": base.utc_timestamp(),
        "generation": {
            "generator": generator_path.name,
            "generator_sha256": base.sha256_file(generator_path),
            "imported_base_generator": base_generator_path.name,
            "imported_base_generator_sha256": base.sha256_file(base_generator_path),
            "imported_common_helper": common_helper_path.name,
            "imported_common_helper_sha256": base.sha256_file(common_helper_path),
            "outcome_independent": True,
            "retry_policy": "no seed retries",
            "seed_count": len(MASTER_SEEDS),
            "holdout_included": False,
        },
        "lane_widths": list(base.DEFAULT_LANE_WIDTHS),
        "mapping": {
            "args": ["-x", "map-ont"],
            "threads": 1,
            "timeout_seconds": 300,
            "emit_seed_coordinates": True,
            "emit_seed_frequencies": True,
            "analysis_scope": "initial DP call, with later RMQ calls retained separately",
        },
        "seed_plan": seed_plan,
        "seed_plan_sha256": base.canonical_json_sha256(seed_plan),
        "constructions": {
            "mosaic": {
                "layout": "U0 + A + U1 + A_prime + U2",
                "unique_segment_length": base.MOSAIC_UNIQUE_LENGTH,
                "a_length": base.MOSAIC_A_LENGTH,
                "total_length": (
                    2 * base.MOSAIC_A_LENGTH + 3 * base.MOSAIC_UNIQUE_LENGTH
                ),
                "a_to_a_prime_offset": (
                    base.MOSAIC_A_LENGTH + base.MOSAIC_UNIQUE_LENGTH
                ),
                "levels": list(DENSE_MOSAIC_LEVELS),
                "divergence_grid": {
                    "exact_fraction_formula": "i / 400",
                    "first_index": 0,
                    "last_index": GRID_LAST_INDEX,
                    "numeric_level_count": EXPECTED_NUMERIC_LEVELS,
                    "first_percent": "0.00",
                    "last_percent": "20.00",
                    "step_percent": format(GRID_STEP_PERCENT, ".2f"),
                    "declared_fraction_strings": list(DENSE_RATE_LEVELS),
                    "rounded_substitution_counts": list(counts),
                    "rounding": "fraction times A length, ROUND_HALF_UP",
                },
                "independent_control_per_seed": 1,
            }
        },
        "cases": cases,
    }
    manifest["generation"]["self_validation"] = validate_generated_manifest(
        temporary_root, manifest
    )
    return manifest


def validate_generated_manifest(
    temporary_root: Path, manifest: dict[str, Any]
) -> dict[str, Any]:
    """Check case coverage, exact counts, and nested mutation prefixes."""

    cases = manifest["cases"]
    if len(cases) != EXPECTED_CASE_COUNT:
        raise AssertionError(
            f"dense manifest has {len(cases)} cases, expected {EXPECTED_CASE_COUNT}"
        )
    case_ids = [case["case_id"] for case in cases]
    if len(set(case_ids)) != EXPECTED_CASE_COUNT:
        raise AssertionError("dense manifest case identifiers must be unique")
    if any(case["group"] != "mosaic" for case in cases):
        raise AssertionError("dense manifest must contain only mosaic cases")

    expected_levels = list(DENSE_MOSAIC_LEVELS)
    expected_counts = {
        level: base.count_for_rate(level) for level in DENSE_RATE_LEVELS
    }
    for replicate_index, master_seed in enumerate(MASTER_SEEDS, start=1):
        replicate_id = f"seed_{replicate_index:02d}"
        replicate_cases = [
            case for case in cases if case["replicate_id"] == replicate_id
        ]
        if len(replicate_cases) != EXPECTED_CASES_PER_SEED:
            raise AssertionError(
                f"{replicate_id} has {len(replicate_cases)} cases, "
                f"expected {EXPECTED_CASES_PER_SEED}"
            )
        observed_levels = [
            case["parameters"]["similarity_level"] for case in replicate_cases
        ]
        if observed_levels != expected_levels:
            raise AssertionError(f"{replicate_id} does not contain the exact grid")
        for case in replicate_cases:
            parameters = case["parameters"]
            level = parameters["similarity_level"]
            if case["seed"] != master_seed:
                raise AssertionError(f"{case['case_id']} has an unexpected seed")
            if case["reference"]["sequence_sha256"] != case["query"]["sequence_sha256"]:
                raise AssertionError(f"{case['case_id']} is not an exact T=Q case")
            expected_count = (
                None if level == "independent_control" else expected_counts[level]
            )
            if parameters["substitution_count"] != expected_count:
                raise AssertionError(
                    f"{case['case_id']} has an incorrect substitution count"
                )

    replicate_plans = manifest["seed_plan"]["replicates"]
    if len(replicate_plans) != len(MASTER_SEEDS):
        raise AssertionError("dense manifest must contain one plan per seed")
    for replicate_plan, master_seed in zip(
        replicate_plans, MASTER_SEEDS, strict=True
    ):
        declaration = replicate_plan["mutation_plan"]
        plan_path = (temporary_root / declaration["path"]).resolve()
        try:
            plan_path.relative_to(temporary_root.resolve())
        except ValueError as error:
            raise AssertionError("mutation plan path escapes the output root") from error
        if base.sha256_file(plan_path) != declaration["file_sha256"]:
            raise AssertionError("mutation plan file digest does not match its declaration")
        with plan_path.open("r", encoding="utf-8") as source:
            mutation_plan = json.load(source)
        if list(mutation_plan["levels"]) != list(DENSE_RATE_LEVELS):
            raise AssertionError("mutation plan does not contain exactly the dense levels")
        positions = mutation_plan["ordered_positions"]
        replacements = mutation_plan["replacement_bases_ascii"].encode("ascii")
        if len(positions) != len(set(positions)):
            raise AssertionError("mutation plan positions must be unique")
        if len(positions) != len(replacements):
            raise AssertionError("mutation plan position and replacement counts differ")
        a_sequence = base.uniform_dna(
            base.MOSAIC_A_LENGTH, master_seed, "mosaic", "A"
        )
        if any(
            a_sequence[position] == replacement
            for position, replacement in zip(positions, replacements, strict=True)
        ):
            raise AssertionError("a declared substitution preserves its original base")

        previous_positions: list[int] = []
        for level in DENSE_RATE_LEVELS:
            level_record = mutation_plan["levels"][level]
            count = expected_counts[level]
            current_positions = positions[:count]
            current_replacements = replacements[:count]
            if current_positions[: len(previous_positions)] != previous_positions:
                raise AssertionError("mutation levels are not nested prefixes")
            if level_record["substitution_count"] != count:
                raise AssertionError(f"mutation count is incorrect for level {level}")
            if level_record["positions_prefix_sha256"] != base.canonical_json_sha256(
                current_positions
            ):
                raise AssertionError(f"position-prefix digest is incorrect for {level}")
            if level_record["replacement_bases_prefix_sha256"] != base.sha256_bytes(
                current_replacements
            ):
                raise AssertionError(f"replacement-prefix digest is incorrect for {level}")
            previous_positions = current_positions

    return {
        "status": "pass",
        "case_count": EXPECTED_CASE_COUNT,
        "numeric_level_count": EXPECTED_NUMERIC_LEVELS,
        "cases_per_seed": EXPECTED_CASES_PER_SEED,
        "unique_case_ids": True,
        "unique_rounded_substitution_counts": True,
        "nested_mutation_prefixes": True,
        "reference_equals_query_for_every_case": True,
    }


def publish_directory(temporary_root: Path, output_root: Path, replace: bool) -> None:
    """Publish atomically and archive a prior output only when requested."""

    if output_root.exists():
        if not replace:
            raise FileExistsError(
                f"output already exists: {output_root}; pass --replace to archive it"
            )
        suffix = base.utc_timestamp().replace(":", "").replace("-", "")
        archive = output_root.with_name(f"{output_root.name}.superseded.{suffix}")
        os.replace(output_root, archive)
    os.replace(temporary_root, output_root)


def main() -> int:
    """Generate, validate, and atomically publish the dense input manifest."""

    arguments = parse_args()
    output_root = arguments.output_dir.resolve()
    try:
        output_root.relative_to(base.ROOT.resolve())
    except ValueError as error:
        raise ValueError(
            f"--output-dir must resolve below {base.ROOT.resolve()}, observed {output_root}"
        ) from error
    if output_root == base.ROOT.resolve():
        raise ValueError("--output-dir must be a strict child of the experiment root")
    output_root.parent.mkdir(parents=True, exist_ok=True)
    temporary_root = Path(
        tempfile.mkdtemp(prefix=f".{output_root.name}.tmp.", dir=output_root.parent)
    )
    try:
        manifest = build_manifest(temporary_root)
        manifest_path = temporary_root / "manifest.json"
        manifest_digest = base.atomic_write_json(manifest_path, manifest)
        base.atomic_write_bytes(
            temporary_root / "manifest.sha256",
            (f"{manifest_digest}  manifest.json\n".encode("ascii"),),
        )
        publish_directory(temporary_root, output_root, arguments.replace)
    except BaseException:
        shutil.rmtree(temporary_root, ignore_errors=True)
        raise
    print(
        json.dumps(
            {
                "output_dir": str(output_root),
                "manifest": str(output_root / "manifest.json"),
                "manifest_sha256": manifest_digest,
                "case_count": len(manifest["cases"]),
                "seed_count": len(MASTER_SEEDS),
                "numeric_level_count": EXPECTED_NUMERIC_LEVELS,
                "profile": PROFILE,
                "self_validation": manifest["generation"]["self_validation"][
                    "status"
                ],
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
