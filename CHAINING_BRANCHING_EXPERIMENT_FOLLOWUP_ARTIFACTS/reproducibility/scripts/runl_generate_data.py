#!/usr/bin/env python3
"""Generate deterministic repeated-occurrence inputs for the Run_L experiment."""

from __future__ import annotations

import argparse
import hashlib
import json
import random
from pathlib import Path


REFERENCE_LENGTH = 10_000_000
OCCURRENCE_COUNTS = [
    1, 2, 3, 4, 5, 10, 20, 30, 40, 50, 100, 200, 300, 400, 500,
    1000, 2000, 3000, 4000, 5000,
]
CALIBRATION_COUNTS = [1, 2, 5, 10]
DNA = b"ACGT"
DNA_TRANSLATION = bytes(DNA[value & 3] for value in range(256))
COMPLEMENT = bytes.maketrans(b"ACGT", b"TGCA")


def uniform_dna(length: int, seed: int, label: str) -> bytes:
    """Return deterministic iid DNA4 bytes using a uniform SHAKE byte stream."""
    key = f"runl-experiment:{seed}:{label}".encode("ascii")
    return hashlib.shake_256(key).digest(length).translate(DNA_TRANSLATION)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def write_fasta(path: Path, name: str, sequence: bytes) -> None:
    with path.open("wb") as output:
        output.write(b">")
        output.write(name.encode("ascii"))
        output.write(b"\n")
        for begin in range(0, len(sequence), 80):
            output.write(sequence[begin : begin + 80])
            output.write(b"\n")


def make_nested_placements(
    reference_length: int,
    source_length: int,
    maximum_count: int,
    seed: int,
) -> tuple[list[int], int]:
    cell_width = reference_length // maximum_count
    if source_length > cell_width:
        raise ValueError("source sequence does not fit into one placement cell")
    rng = random.Random(seed)
    maximum_offset = (cell_width - source_length) // 2
    positions = [
        cell * cell_width + rng.randrange(maximum_offset + 1)
        for cell in range(maximum_count)
    ]
    rng.shuffle(positions)
    return positions, cell_width


def count_overlapping(sequence: bytes, pattern: bytes) -> int:
    count = 0
    start = 0
    while True:
        found = sequence.find(pattern, start)
        if found < 0:
            return count
        count += 1
        start = found + 1


def mutate_query(
    source: bytes,
    *,
    seed: int,
    substitution_rate: float,
    insertion_rate: float,
    deletion_rate: float,
    insertion_length_mean: float,
    insertion_length_stddev: float,
    deletion_length_mean: float,
    deletion_length_stddev: float,
    reverse_complement_rate: float,
) -> tuple[bytes, dict[str, int | bool]]:
    rng = random.Random(seed)
    reverse = rng.random() < reverse_complement_rate
    oriented = source.translate(COMPLEMENT)[::-1] if reverse else source

    substituted = bytearray(oriented)
    substitutions = 0
    for index, original in enumerate(substituted):
        if rng.random() < substitution_rate:
            replacement = original
            while replacement == original:
                replacement = DNA[rng.randrange(4)]
            substituted[index] = replacement
            substitutions += 1

    result = bytearray()
    insertion_rolls = insertion_events = inserted_bases = 0
    deletion_rolls = deletion_events = deleted_bases = 0
    index = 0
    while index < len(substituted):
        result.append(substituted[index])
        if rng.random() < insertion_rate:
            insertion_rolls += 1
            length = max(0, round(rng.gauss(insertion_length_mean, insertion_length_stddev)))
            if length > 0:
                insertion_events += 1
                inserted_bases += length
                result.extend(DNA[rng.randrange(4)] for _ in range(length))
        if rng.random() < deletion_rate:
            deletion_rolls += 1
            length = max(0, round(rng.gauss(deletion_length_mean, deletion_length_stddev)))
            actual = min(length, len(substituted) - index - 1)
            if actual > 0:
                deletion_events += 1
                deleted_bases += actual
                index += actual
        index += 1

    return bytes(result), {
        "reverse_complement": reverse,
        "substitutions": substitutions,
        "insertion_rolls": insertion_rolls,
        "insertion_events_with_positive_length": insertion_events,
        "inserted_bases": inserted_bases,
        "deletion_rolls": deletion_rolls,
        "deletion_events_with_positive_length": deletion_events,
        "deleted_bases": deleted_bases,
    }


def generate_arm(
    root: Path,
    *,
    arm_name: str,
    source_length: int,
    occurrence_counts: list[int],
    maximum_count: int,
    seed: int,
) -> dict[str, object]:
    arm_dir = root / arm_name
    arm_dir.mkdir(parents=True, exist_ok=True)
    base_reference = uniform_dna(REFERENCE_LENGTH, seed, f"{arm_name}:reference")
    positions, cell_width = make_nested_placements(
        REFERENCE_LENGTH, source_length, maximum_count, seed ^ 0x7A5B_C19D
    )
    source = base_reference[positions[0] : positions[0] + source_length]

    query, mutation_counts = mutate_query(
        source,
        seed=seed,
        substitution_rate=0.001,
        insertion_rate=0.0005,
        deletion_rate=0.0005,
        insertion_length_mean=1.0,
        insertion_length_stddev=1.0,
        deletion_length_mean=1.0,
        deletion_length_stddev=1.0,
        reverse_complement_rate=0.5,
    )
    query_path = arm_dir / "query.fa"
    write_fasta(query_path, f"{arm_name}_query", query)

    placements_path = arm_dir / "placements.tsv"
    with placements_path.open("w", encoding="utf-8") as output:
        output.write("rank\tstart\n")
        for rank, position in enumerate(positions, 1):
            output.write(f"{rank}\t{position}\n")

    references: dict[str, dict[str, object]] = {}
    for count in occurrence_counts:
        reference = bytearray(base_reference)
        for position in positions[1:count]:
            reference[position : position + source_length] = source
        reference_path = arm_dir / f"reference_N{count}.fa"
        write_fasta(reference_path, f"{arm_name}_reference_N{count}", reference)
        verified_occurrences = count_overlapping(reference, source)
        if verified_occurrences != count:
            raise RuntimeError(
                f"{arm_name} N={count}: expected {count} exact source occurrences, "
                f"found {verified_occurrences}"
            )
        references[str(count)] = {
            "path": str(reference_path.relative_to(root.parent)),
            "sha256": sha256(reference_path),
            "intended_occurrences": count,
            "verified_exact_source_occurrences": verified_occurrences,
            "copy_coverage_fraction": count * source_length / REFERENCE_LENGTH,
        }

    return {
        "name": arm_name,
        "reference_length": REFERENCE_LENGTH,
        "source_length": source_length,
        "query_length_after_edits": len(query),
        "seed": seed,
        "mutation_rng_seed": seed,
        "cell_width": cell_width,
        "minimum_full_grid_copy_gap": min(
            right - left - source_length
            for left, right in zip(sorted(positions), sorted(positions)[1:])
        ),
        "occurrence_counts": occurrence_counts,
        "reference_model": "iid_uniform_DNA4_from_SHAKE256_byte_stream",
        "placement_model": "bounded_random_offset_per_fixed_cell_then_shuffled_once_and_nested_prefix",
        "mutation_model": {
            "orientation_applied_before_errors": True,
            "substitution_rate": 0.001,
            "insertion_event_rate": 0.0005,
            "deletion_event_rate": 0.0005,
            "insertion_length_mean": 1.0,
            "insertion_length_stddev": 1.0,
            "deletion_length_mean": 1.0,
            "deletion_length_stddev": 1.0,
            "reverse_complement_rate": 0.5,
            "implementation_note": "SEAL/BioSeq-like parameters; Python RNG is deterministic but not byte-identical to BioSeq HybridTaus streams",
        },
        "realized_mutations": mutation_counts,
        "query_path": str(query_path.relative_to(root.parent)),
        "query_sha256": sha256(query_path),
        "placements_path": str(placements_path.relative_to(root.parent)),
        "placements_sha256": sha256(placements_path),
        "references": references,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=Path("experiment_data"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    manifest = {
        "schema_version": 1,
        "primary": generate_arm(
            args.output,
            arm_name="primary_1kb_hifi",
            source_length=1_000,
            occurrence_counts=OCCURRENCE_COUNTS,
            maximum_count=max(OCCURRENCE_COUNTS),
            seed=3_219_876,
        ),
        "hifi_calibration": generate_arm(
            args.output,
            arm_name="fast_long_500kb_hifi",
            source_length=500_000,
            occurrence_counts=CALIBRATION_COUNTS,
            maximum_count=max(CALIBRATION_COUNTS),
            seed=3_219_876,
        ),
    }
    manifest_path = args.output / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(manifest_path)


if __name__ == "__main__":
    main()
