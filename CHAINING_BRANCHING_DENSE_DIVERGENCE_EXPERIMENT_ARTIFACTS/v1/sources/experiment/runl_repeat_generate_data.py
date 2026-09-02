#!/usr/bin/env python3
"""Generate deterministic pilot inputs for the repeat-disruption Run_L study.

Two complementary constructions are emitted.  The mosaic construction is an
exact full-length reference/query match containing a controlled approximate
repeat.  The spaced-copy construction repeats a short exact source in the
query, with N guards wider than minimap2's default chaining gap between copy
starts.  Generation is independent of mapping results and never retries a
seed, which prevents outcome-dependent input selection.
"""

from __future__ import annotations

import argparse
from decimal import Decimal, ROUND_HALF_UP
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
from typing import Any

from runl_repeat_common import (
    DEFAULT_LANE_WIDTHS,
    ROOT,
    SCHEMA_VERSION,
    atomic_write_bytes,
    atomic_write_json,
    canonical_json_sha256,
    sha256_bytes,
    sha256_file,
    utc_timestamp,
)


ALGORITHM_NAMESPACE = b"runl-repeat-disruption-data-v1"
DEFAULT_OUTPUT = ROOT / "repeat_disruption_data_pilot"
DEFAULT_MASTER_SEEDS = (7_314_159, 8_271_828, 9_161_803)
DNA = b"ACGT"
DNA_TRANSLATION = bytes(DNA[value & 3] for value in range(256))
FASTA_LINE_WIDTH = 80

MOSAIC_A_LENGTH = 32_768
MOSAIC_UNIQUE_LENGTH = 512
MOSAIC_LEVELS = (
    "independent_control",
    "0.20",
    "0.10",
    "0.05",
    "0.02",
    "0.01",
    "0.005",
    "0.002",
    "0",
)
MOSAIC_MAX_RATE = Decimal("0.20")

SPACED_SOURCE_LENGTH = 4_096
SPACED_GUARD_LENGTH = 16_384
SPACED_COPY_COUNTS = (1, 2, 4, 8, 16, 32, 64)


def parse_args() -> argparse.Namespace:
    """Parse generator options."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--master-seeds",
        default=",".join(str(seed) for seed in DEFAULT_MASTER_SEEDS),
        help="comma-separated deterministic nonnegative integers",
    )
    parser.add_argument(
        "--replace",
        action="store_true",
        help="archive an existing output directory before publishing the new one",
    )
    return parser.parse_args()


def parse_master_seeds(value: str) -> tuple[int, ...]:
    """Parse a unique, nonempty list of nonnegative master seeds."""

    try:
        seeds = tuple(int(item.strip()) for item in value.split(",") if item.strip())
    except ValueError as error:
        raise ValueError("--master-seeds must contain only integers") from error
    if not seeds or len(set(seeds)) != len(seeds) or any(seed < 0 for seed in seeds):
        raise ValueError("master seeds must be unique nonnegative integers")
    return seeds


def domain_material(master_seed: int, construction: str, domain: str) -> bytes:
    """Derive stable seed material with explicit domain separation."""

    payload = canonical_json_bytes(
        {
            "namespace": ALGORITHM_NAMESPACE.decode("ascii"),
            "master_seed_decimal": str(master_seed),
            "construction": construction,
            "domain": domain,
        }
    )
    return hashlib.sha256(payload).digest()


def canonical_json_bytes(value: object) -> bytes:
    """Serialize seed-derivation inputs deterministically."""

    return json.dumps(
        value,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def seed_record(master_seed: int, construction: str, domain: str) -> dict[str, Any]:
    """Describe one derivation without exposing platform-dependent RNG state."""

    material = domain_material(master_seed, construction, domain)
    return {
        "domain": domain,
        "derivation_input_sha256": hashlib.sha256(
            canonical_json_bytes(
                {
                    "namespace": ALGORITHM_NAMESPACE.decode("ascii"),
                    "master_seed_decimal": str(master_seed),
                    "construction": construction,
                    "domain": domain,
                }
            )
        ).hexdigest(),
        "derived_material_sha256": hashlib.sha256(material).hexdigest(),
        "derived_uint64_decimal": str(int.from_bytes(material[:8], "big")),
    }


def uniform_dna(length: int, master_seed: int, construction: str, domain: str) -> bytes:
    """Generate deterministic iid DNA4 from a SHAKE-256 stream."""

    material = domain_material(master_seed, construction, domain)
    stream_key = ALGORITHM_NAMESPACE + b"\0dna\0" + material
    return hashlib.shake_256(stream_key).digest(length).translate(DNA_TRANSLATION)


def mutation_rank(master_seed: int, position: int) -> bytes:
    """Return a stable pseudorandom sort key for one A position."""

    material = domain_material(master_seed, "mosaic", "mutation-order")
    return hashlib.sha256(
        ALGORITHM_NAMESPACE
        + b"\0mutation-position\0"
        + material
        + position.to_bytes(8, "big")
    ).digest()


def mutation_replacement(master_seed: int, position: int, original: int) -> int:
    """Choose one of the three bases different from ``original``."""

    material = domain_material(master_seed, "mosaic", "mutation-replacement")
    choice = hashlib.sha256(
        ALGORITHM_NAMESPACE
        + b"\0replacement\0"
        + material
        + position.to_bytes(8, "big")
    ).digest()[0] % 3
    alternatives = [base for base in DNA if base != original]
    return alternatives[choice]


def count_for_rate(rate_text: str) -> int:
    """Round a declared decimal substitution fraction to the nearest base count."""

    if rate_text == "0":
        return 0
    return int(
        (Decimal(rate_text) * Decimal(MOSAIC_A_LENGTH)).to_integral_value(
            rounding=ROUND_HALF_UP
        )
    )


def build_mutation_plan(master_seed: int, a_sequence: bytes) -> dict[str, Any]:
    """Build one nested mutation prefix shared by every mosaic similarity level."""

    maximum_count = count_for_rate(str(MOSAIC_MAX_RATE))
    ordered = sorted(range(len(a_sequence)), key=lambda position: mutation_rank(master_seed, position))
    positions = ordered[:maximum_count]
    replacements = bytes(
        mutation_replacement(master_seed, position, a_sequence[position])
        for position in positions
    )
    levels: dict[str, Any] = {}
    for level in MOSAIC_LEVELS:
        if level == "independent_control":
            continue
        count = count_for_rate(level)
        prefix_positions = positions[:count]
        prefix_replacements = replacements[:count]
        levels[level] = {
            "substitution_count": count,
            "realized_fraction": count / MOSAIC_A_LENGTH,
            "positions_prefix_sha256": canonical_json_sha256(prefix_positions),
            "replacement_bases_prefix_sha256": sha256_bytes(prefix_replacements),
        }
    return {
        "schema_version": 1,
        "master_seed": master_seed,
        "ordering": "ascending SHA-256 rank of every A position",
        "rounding": "decimal fraction times A length, ROUND_HALF_UP",
        "a_length": len(a_sequence),
        "maximum_fraction": str(MOSAIC_MAX_RATE),
        "maximum_substitution_count": maximum_count,
        "ordered_positions": positions,
        "ordered_positions_sha256": canonical_json_sha256(positions),
        "replacement_bases_ascii": replacements.decode("ascii"),
        "replacement_bases_sha256": sha256_bytes(replacements),
        "levels": levels,
    }


def apply_mutation_prefix(
    a_sequence: bytes, mutation_plan: dict[str, Any], rate_text: str
) -> bytes:
    """Apply exactly the mutation-plan prefix declared for ``rate_text``."""

    count = mutation_plan["levels"][rate_text]["substitution_count"]
    positions = mutation_plan["ordered_positions"][:count]
    replacements = mutation_plan["replacement_bases_ascii"].encode("ascii")[:count]
    mutated = bytearray(a_sequence)
    for position, replacement in zip(positions, replacements, strict=True):
        if mutated[position] == replacement:
            raise AssertionError("mutation replacement unexpectedly equals the original base")
        mutated[position] = replacement
    return bytes(mutated)


def fasta_bytes(name: str, sequence: bytes) -> bytes:
    """Encode one wrapped FASTA record."""

    if not name.isascii() or not name or any(character.isspace() for character in name):
        raise ValueError(f"invalid FASTA name: {name!r}")
    lines = [b">" + name.encode("ascii")]
    lines.extend(
        sequence[offset : offset + FASTA_LINE_WIDTH]
        for offset in range(0, len(sequence), FASTA_LINE_WIDTH)
    )
    return b"\n".join(lines) + b"\n"


def write_fasta(path: Path, name: str, sequence: bytes) -> dict[str, Any]:
    """Write one FASTA record and return its immutable manifest declaration."""

    encoded = fasta_bytes(name, sequence)
    file_digest = atomic_write_bytes(path, (encoded,))
    return {
        "path": str(path),
        "file_sha256": file_digest,
        "sequence_sha256": sha256_bytes(sequence),
        "name": name,
        "length": len(sequence),
        "alphabet": "ACGTN" if b"N" in sequence else "ACGT",
    }


def relative_artifact(artifact: dict[str, Any], root: Path) -> dict[str, Any]:
    """Replace an artifact's path with a manifest-relative path."""

    value = dict(artifact)
    value["path"] = str(Path(value["path"]).resolve().relative_to(root.resolve()))
    return value


def write_construction(path: Path, value: dict[str, Any], root: Path) -> dict[str, Any]:
    """Write a construction JSON file and return its manifest declaration."""

    digest = atomic_write_json(path, value)
    return {
        "path": str(path.resolve().relative_to(root.resolve())),
        "file_sha256": digest,
        "content_sha256": canonical_json_sha256(value),
    }


def mosaic_segments() -> list[dict[str, Any]]:
    """Return the fixed five-segment mosaic geometry."""

    lengths = (
        ("U0", MOSAIC_UNIQUE_LENGTH),
        ("A", MOSAIC_A_LENGTH),
        ("U1", MOSAIC_UNIQUE_LENGTH),
        ("A_prime", MOSAIC_A_LENGTH),
        ("U2", MOSAIC_UNIQUE_LENGTH),
    )
    cursor = 0
    segments: list[dict[str, Any]] = []
    for name, length in lengths:
        segments.append({"name": name, "start": cursor, "end": cursor + length, "length": length})
        cursor += length
    return segments


def make_mosaic_cases(
    temporary_root: Path, master_seed: int, replicate_index: int
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Generate all nested-similarity mosaic cases for one pilot seed."""

    replicate_id = f"seed_{replicate_index:02d}"
    u0 = uniform_dna(MOSAIC_UNIQUE_LENGTH, master_seed, "mosaic", "U0")
    a = uniform_dna(MOSAIC_A_LENGTH, master_seed, "mosaic", "A")
    u1 = uniform_dna(MOSAIC_UNIQUE_LENGTH, master_seed, "mosaic", "U1")
    independent = uniform_dna(
        MOSAIC_A_LENGTH, master_seed, "mosaic", "A-prime-independent"
    )
    u2 = uniform_dna(MOSAIC_UNIQUE_LENGTH, master_seed, "mosaic", "U2")
    mutation_plan = build_mutation_plan(master_seed, a)
    plan_path = temporary_root / "replicates" / replicate_id / "mosaic_mutation_plan.json"
    plan_digest = atomic_write_json(plan_path, mutation_plan)
    segments = mosaic_segments()
    total_length = segments[-1]["end"]
    a_start = next(segment["start"] for segment in segments if segment["name"] == "A")
    a_prime_start = next(
        segment["start"] for segment in segments if segment["name"] == "A_prime"
    )
    offset = a_prime_start - a_start
    if offset != 33_280 or offset <= 20_000:
        raise AssertionError("mosaic offset must remain outside default long-join bandwidth")

    cases: list[dict[str, Any]] = []
    for level in MOSAIC_LEVELS:
        if level == "independent_control":
            a_prime = independent
            substitutions = None
            level_id = "independent"
        else:
            a_prime = apply_mutation_prefix(a, mutation_plan, level)
            substitutions = mutation_plan["levels"][level]["substitution_count"]
            level_id = "d" + level.replace(".", "p")
        sequence = u0 + a + u1 + a_prime + u2
        if len(sequence) != total_length:
            raise AssertionError("mosaic sequence length does not match its declared geometry")
        case_id = f"mosaic_{replicate_id}_{level_id}"
        case_root = temporary_root / "cases" / case_id
        reference_name = f"ref_{case_id}"
        query_name = f"query_{case_id}"
        reference = write_fasta(case_root / "reference.fa", reference_name, sequence)
        query = write_fasta(case_root / "query.fa", query_name, sequence)
        if reference["sequence_sha256"] != query["sequence_sha256"]:
            raise AssertionError("mosaic reference and query must be byte-identical sequences")
        construction = {
            "schema_version": 1,
            "construction_type": "mosaic_exact_full_diagonal",
            "case_id": case_id,
            "master_seed": master_seed,
            "level": level,
            "declared_substitution_count_between_A_and_A_prime": substitutions,
            "segments": segments,
            "total_length": total_length,
            "a_to_a_prime_offset": offset,
            "off_diagonal_tracks": [
                {
                    "from": "A",
                    "to": "A_prime",
                    "query_minus_target_offset": offset,
                },
                {
                    "from": "A_prime",
                    "to": "A",
                    "query_minus_target_offset": -offset,
                },
            ],
            "default_bw_long": 20_000,
            "off_diagonal_tracks_outside_default_bw_long": offset > 20_000,
            "mutation_plan": {
                "path": str(plan_path.resolve().relative_to(temporary_root.resolve())),
                "file_sha256": plan_digest,
                "ordered_positions_sha256": mutation_plan["ordered_positions_sha256"],
                "applied_prefix_count": substitutions,
            },
            "component_sequence_sha256": {
                "U0": sha256_bytes(u0),
                "A": sha256_bytes(a),
                "U1": sha256_bytes(u1),
                "A_prime": sha256_bytes(a_prime),
                "U2": sha256_bytes(u2),
            },
        }
        construction_artifact = write_construction(
            case_root / "construction.json", construction, temporary_root
        )
        cases.append(
            {
                "case_id": case_id,
                "replicate_id": replicate_id,
                "seed": master_seed,
                "group": "mosaic",
                "parameters": {
                    "construction": "mosaic",
                    "similarity_level": level,
                    "substitution_count": substitutions,
                    "a_length": MOSAIC_A_LENGTH,
                    "a_to_a_prime_offset": offset,
                },
                "reference": relative_artifact(reference, temporary_root),
                "query": relative_artifact(query, temporary_root),
                "construction_artifact": construction_artifact,
                "expectation": {
                    "minimum_path_anchors": max(DEFAULT_LANE_WIDTHS),
                    "require_unique_top_score": True,
                    "require_no_query_minimizer_filter_loss": True,
                    "seed_frequency": {
                        "allowed_initial_reference_occurrences": [1, 2],
                        "required_initial_filtered_count": 0,
                    },
                    "primary_path": {
                        "target_name": reference_name,
                        "strand": "+",
                        "diagonal": 0,
                        "target_interval": [0, total_length],
                        "query_interval": [0, total_length],
                        "minimum_fraction_on_declared_path": 1.0,
                    },
                    "paf": {
                        "minimum_record_count": 1,
                        "required_target_name": reference_name,
                        "required_query_name": query_name,
                        "required_strand": "+",
                        "minimum_query_coverage_fraction": 0.99,
                        "minimum_target_coverage_fraction": 0.99,
                    },
                    "mosaic": {
                        "a_to_a_prime_offset": offset,
                        "segment_count": 5,
                        "expected_group_boundaries": segments,
                    },
                },
            }
        )

    replicate_plan = {
        "replicate_id": replicate_id,
        "master_seed": master_seed,
        "construction": "mosaic",
        "seed_derivations": [
            seed_record(master_seed, "mosaic", domain)
            for domain in (
                "U0",
                "A",
                "U1",
                "A-prime-independent",
                "U2",
                "mutation-order",
                "mutation-replacement",
            )
        ],
        "mutation_plan": {
            "path": str(plan_path.resolve().relative_to(temporary_root.resolve())),
            "file_sha256": plan_digest,
            "ordered_positions": mutation_plan["ordered_positions"],
            "ordered_positions_sha256": mutation_plan["ordered_positions_sha256"],
            "replacement_bases_sha256": mutation_plan["replacement_bases_sha256"],
            "levels": mutation_plan["levels"],
        },
    }
    return cases, replicate_plan


def make_spaced_copy_cases(
    temporary_root: Path, master_seed: int, replicate_index: int
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Generate exact spaced-copy controls for one pilot seed."""

    replicate_id = f"seed_{replicate_index:02d}"
    source = uniform_dna(
        SPACED_SOURCE_LENGTH, master_seed, "spaced-copy", "source"
    )
    guard = b"N" * SPACED_GUARD_LENGTH
    pitch = SPACED_SOURCE_LENGTH + SPACED_GUARD_LENGTH
    if pitch <= 20_000:
        raise AssertionError("copy pitch must exceed minimap2's default long-join bandwidth")
    cases: list[dict[str, Any]] = []
    for copies in SPACED_COPY_COUNTS:
        reference_sequence = source + guard
        query_sequence = (source + guard) * copies
        expected_length = copies * pitch
        if len(query_sequence) != expected_length:
            raise AssertionError("spaced-copy query length mismatch")
        case_id = f"spaced_{replicate_id}_R{copies:02d}"
        case_root = temporary_root / "cases" / case_id
        reference_name = f"ref_{case_id}"
        query_name = f"query_{case_id}"
        reference = write_fasta(
            case_root / "reference.fa", reference_name, reference_sequence
        )
        query = write_fasta(case_root / "query.fa", query_name, query_sequence)
        copy_intervals = [
            {
                "copy_index": index,
                "start": index * pitch,
                "end": index * pitch + SPACED_SOURCE_LENGTH,
                "query_minus_target_diagonal": index * pitch,
            }
            for index in range(copies)
        ]
        guard_intervals = [
            {
                "guard_index": index,
                "start": index * pitch + SPACED_SOURCE_LENGTH,
                "end": (index + 1) * pitch,
            }
            for index in range(copies)
        ]
        construction = {
            "schema_version": 1,
            "construction_type": "exact_spaced_query_copies",
            "case_id": case_id,
            "master_seed": master_seed,
            "copy_count": copies,
            "source_length": SPACED_SOURCE_LENGTH,
            "guard_length": SPACED_GUARD_LENGTH,
            "copy_pitch": pitch,
            "default_max_chain_gap": 5_000,
            "default_bw_long": 20_000,
            "copy_pitch_exceeds_default_max_chain_gap": pitch > 5_000,
            "copy_pitch_exceeds_default_bw_long": pitch > 20_000,
            "copy_intervals": copy_intervals,
            "guard_intervals": guard_intervals,
            "reference_source_interval": [0, SPACED_SOURCE_LENGTH],
            "reference_guard_interval": [SPACED_SOURCE_LENGTH, pitch],
            "guard_base": "N",
            "source_sequence_sha256": sha256_bytes(source),
        }
        construction_artifact = write_construction(
            case_root / "construction.json", construction, temporary_root
        )
        cases.append(
            {
                "case_id": case_id,
                "replicate_id": replicate_id,
                "seed": master_seed,
                "group": "spaced_copy_control",
                "parameters": {
                    "construction": "spaced_copy",
                    "copy_count": copies,
                    "source_length": SPACED_SOURCE_LENGTH,
                    "guard_length": SPACED_GUARD_LENGTH,
                    "copy_pitch": pitch,
                },
                "reference": relative_artifact(reference, temporary_root),
                "query": relative_artifact(query, temporary_root),
                "construction_artifact": construction_artifact,
                "expectation": {
                    "minimum_path_anchors": max(DEFAULT_LANE_WIDTHS),
                    "require_unique_top_score": copies == 1,
                    "expected_maximum_score_candidate_multiplicity": copies,
                    "require_no_query_minimizer_filter_loss": True,
                    "seed_frequency": {
                        "allowed_initial_reference_occurrences": [1],
                        "required_initial_filtered_count": 0,
                    },
                    "primary_path_alternatives": [
                        {
                            "copy_index": interval["copy_index"],
                            "target_name": reference_name,
                            "strand": "+",
                            "diagonal": interval["query_minus_target_diagonal"],
                            "target_interval": [0, SPACED_SOURCE_LENGTH],
                            "query_interval": [interval["start"], interval["end"]],
                            "minimum_fraction_on_declared_path": 1.0,
                        }
                        for interval in copy_intervals
                    ],
                    "paf": {
                        "minimum_record_count": 1,
                        "required_target_name": reference_name,
                        "required_query_name": query_name,
                        "required_strand": "+",
                        "minimum_query_span": int(0.9 * SPACED_SOURCE_LENGTH),
                        "minimum_target_span": int(0.9 * SPACED_SOURCE_LENGTH),
                    },
                    "spaced_copy": {
                        "copy_count": copies,
                        "copy_pitch": pitch,
                        "copy_intervals": copy_intervals,
                        "closed_form_runl_oracle": {
                            "path_length_symbol": "M",
                            "when_R_equals_1": {
                                "fast_batches": "floor(M/L)",
                                "fast_covered_anchors": "L*floor(M/L)",
                            },
                            "when_R_at_least_2": {
                                "fast_batches_for_L_at_least_2": 0,
                                "fast_covered_anchors_for_L_at_least_2": 0,
                            },
                        },
                    },
                },
            }
        )
    replicate_plan = {
        "replicate_id": replicate_id,
        "master_seed": master_seed,
        "construction": "spaced_copy",
        "seed_derivations": [seed_record(master_seed, "spaced-copy", "source")],
        "source_sequence_sha256": sha256_bytes(source),
    }
    return cases, replicate_plan


def build_manifest(temporary_root: Path, master_seeds: tuple[int, ...]) -> dict[str, Any]:
    """Generate every pilot case and return its complete manifest."""

    cases: list[dict[str, Any]] = []
    replicate_plans: list[dict[str, Any]] = []
    for replicate_index, master_seed in enumerate(master_seeds, start=1):
        mosaic_cases, mosaic_plan = make_mosaic_cases(
            temporary_root, master_seed, replicate_index
        )
        spaced_cases, spaced_plan = make_spaced_copy_cases(
            temporary_root, master_seed, replicate_index
        )
        cases.extend(mosaic_cases)
        cases.extend(spaced_cases)
        replicate_plans.extend((mosaic_plan, spaced_plan))
    seed_plan = {
        "namespace": ALGORITHM_NAMESPACE.decode("ascii"),
        "master_seeds": list(master_seeds),
        "replicates": replicate_plans,
    }
    return {
        "schema_version": SCHEMA_VERSION,
        "status": "complete",
        "experiment_id": "runl_repeat_disruption",
        "profile": "pilot",
        "created_at_utc": utc_timestamp(),
        "generation": {
            "generator": Path(__file__).name,
            "generator_sha256": sha256_file(Path(__file__)),
            "outcome_independent": True,
            "retry_policy": "no seed retries",
            "pilot_seed_count": len(master_seeds),
            "holdout_included": False,
        },
        "lane_widths": list(DEFAULT_LANE_WIDTHS),
        "mapping": {
            "args": ["-x", "map-ont"],
            "threads": 1,
            "timeout_seconds": 300,
            "emit_seed_coordinates": True,
            "emit_seed_frequencies": True,
            "analysis_scope": "initial DP call, with later RMQ calls retained separately",
        },
        "seed_plan": seed_plan,
        "seed_plan_sha256": canonical_json_sha256(seed_plan),
        "constructions": {
            "mosaic": {
                "layout": "U0 + A + U1 + A_prime + U2",
                "unique_segment_length": MOSAIC_UNIQUE_LENGTH,
                "a_length": MOSAIC_A_LENGTH,
                "total_length": 2 * MOSAIC_A_LENGTH + 3 * MOSAIC_UNIQUE_LENGTH,
                "a_to_a_prime_offset": MOSAIC_A_LENGTH + MOSAIC_UNIQUE_LENGTH,
                "levels": list(MOSAIC_LEVELS),
            },
            "spaced_copy": {
                "source_length": SPACED_SOURCE_LENGTH,
                "guard_length": SPACED_GUARD_LENGTH,
                "copy_pitch": SPACED_SOURCE_LENGTH + SPACED_GUARD_LENGTH,
                "copy_counts": list(SPACED_COPY_COUNTS),
            },
        },
        "cases": cases,
    }


def publish_directory(temporary_root: Path, output_root: Path, replace: bool) -> None:
    """Atomically publish generated data, archiving prior output on request."""

    if output_root.exists():
        if not replace:
            raise FileExistsError(
                f"output already exists: {output_root}; pass --replace to archive it"
            )
        suffix = utc_timestamp().replace(":", "").replace("-", "")
        archive = output_root.with_name(f"{output_root.name}.superseded.{suffix}")
        os.replace(output_root, archive)
    os.replace(temporary_root, output_root)


def main() -> int:
    """Generate and atomically publish the three-seed pilot manifest."""

    arguments = parse_args()
    master_seeds = parse_master_seeds(arguments.master_seeds)
    output_root = arguments.output_dir.resolve()
    try:
        output_root.relative_to(ROOT.resolve())
    except ValueError as error:
        raise ValueError(
            f"--output-dir must resolve below {ROOT.resolve()}, observed {output_root}"
        ) from error
    if output_root == ROOT.resolve():
        raise ValueError("--output-dir must be a strict child of the experiment root")
    output_root.parent.mkdir(parents=True, exist_ok=True)
    temporary_root = Path(
        tempfile.mkdtemp(prefix=f".{output_root.name}.tmp.", dir=output_root.parent)
    )
    try:
        manifest = build_manifest(temporary_root, master_seeds)
        manifest_path = temporary_root / "manifest.json"
        manifest_digest = atomic_write_json(manifest_path, manifest)
        atomic_write_bytes(
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
                "pilot_seed_count": len(master_seeds),
                "holdout_included": False,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
