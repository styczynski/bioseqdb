#!/usr/bin/env python3
"""Generate deterministic, predeclared follow-up inputs for the Run_L study.

The generator deliberately has no access to mapping results.  Every requested
replicate is generated exactly once from its declared master seed; a dataset
that violates an exact-occurrence invariant fails instead of being retried.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import random
import re
import shlex
import tempfile
from typing import Any, Iterable, Iterator


SCHEMA_VERSION = 2
ALGORITHM_NAMESPACE = "runl-followup-data-v1"
DEFAULT_ARM_ID = "dense_default_filter_4p5m_1kb"
DEFAULT_TARGET_LENGTH = 4_500_000
DEFAULT_SOURCE_LENGTH = 1_000
DEFAULT_MAX_N = 4_000
DEFAULT_N_VALUES = (
    20,
    30,
    40,
    50,
    100,
    200,
    300,
    400,
    500,
    1_000,
    2_000,
    3_000,
    4_000,
)
DEFAULT_MASTER_SEEDS = (3_219_876, 8_142_731, 2_718_281)
DNA = b"ACGT"
DNA_TRANSLATION = bytes(DNA[value & 3] for value in range(256))
COMPLEMENT = bytes.maketrans(b"ACGT", b"TGCA")
FASTA_LINE_WIDTH = 80
STREAM_CHUNK_SIZE = 1024 * 1024
SAFE_IDENTIFIER = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$")


@dataclass(frozen=True)
class Placement:
    """One copy placement in nested-prefix inclusion order."""

    rank: int
    cell_index: int
    start: int
    end: int


@dataclass(frozen=True)
class FastaWriteResult:
    """Hashes and sequence length produced by an atomic FASTA write."""

    file_sha256: str
    sequence_sha256: str
    sequence_length: int


class OverlappingMatcher:
    """Incrementally locate all, including overlapping, exact pattern matches."""

    def __init__(self, pattern: bytes) -> None:
        """Build a KMP prefix table for a nonempty byte pattern."""
        if not pattern:
            raise ValueError("the exact-occurrence pattern must not be empty")
        self._pattern = pattern
        self._prefix = self._make_prefix_table(pattern)
        self._matched = 0
        self._consumed = 0
        self.positions: list[int] = []

    @staticmethod
    def _make_prefix_table(pattern: bytes) -> list[int]:
        """Return the KMP longest-proper-prefix table for ``pattern``."""
        prefix = [0] * len(pattern)
        matched = 0
        for index in range(1, len(pattern)):
            while matched and pattern[index] != pattern[matched]:
                matched = prefix[matched - 1]
            if pattern[index] == pattern[matched]:
                matched += 1
            prefix[index] = matched
        return prefix

    def feed(self, chunk: bytes | bytearray | memoryview) -> None:
        """Consume one sequence chunk while retaining matches across boundaries."""
        for local_index, value in enumerate(chunk):
            while self._matched and value != self._pattern[self._matched]:
                self._matched = self._prefix[self._matched - 1]
            if value == self._pattern[self._matched]:
                self._matched += 1
            if self._matched == len(self._pattern):
                self.positions.append(
                    self._consumed + local_index - len(self._pattern) + 1
                )
                self._matched = self._prefix[self._matched - 1]
        self._consumed += len(chunk)


def sha256_file(path: Path) -> str:
    """Return a streaming SHA-256 digest of ``path``."""
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(STREAM_CHUNK_SIZE):
            digest.update(block)
    return digest.hexdigest()


def canonical_json_sha256(value: object) -> str:
    """Hash a value using the manifest's canonical compact JSON encoding."""
    encoded = json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def derive_seed(master_seed: int, arm_id: str, replicate_id: str, domain: str) -> int:
    """Derive an independent unsigned 64-bit seed through SHA-256 domain separation."""
    payload = {
        "arm_id": arm_id,
        "domain": domain,
        "master_seed_decimal": str(master_seed),
        "namespace": ALGORITHM_NAMESPACE,
        "replicate_id": replicate_id,
    }
    digest = hashlib.sha256(
        json.dumps(payload, separators=(",", ":"), sort_keys=True).encode("utf-8")
    ).digest()
    return int.from_bytes(digest[:8], byteorder="big", signed=False)


def uniform_dna(length: int, seed: int) -> bytes:
    """Return deterministic iid uniform DNA4 bases from a SHAKE-256 byte stream."""
    key = (
        ALGORITHM_NAMESPACE.encode("ascii")
        + b"\0reference-bases\0"
        + seed.to_bytes(8, byteorder="big", signed=False)
    )
    return hashlib.shake_256(key).digest(length).translate(DNA_TRANSLATION)


def iter_buffer_chunks(
    buffer: bytes, begin: int, end: int, chunk_size: int = STREAM_CHUNK_SIZE
) -> Iterator[memoryview]:
    """Yield bounded zero-copy views over ``buffer[begin:end]``."""
    view = memoryview(buffer)
    for offset in range(begin, end, chunk_size):
        yield view[offset : min(end, offset + chunk_size)]


def iter_reference_chunks(
    background: bytes, source: bytes, positions: Iterable[int]
) -> Iterator[bytes | memoryview]:
    """Stream a reference formed by replacing sorted nonoverlapping spans with ``source``."""
    cursor = 0
    for position in sorted(positions):
        yield from iter_buffer_chunks(background, cursor, position)
        yield source
        cursor = position + len(source)
    yield from iter_buffer_chunks(background, cursor, len(background))


def write_atomic_bytes(path: Path, chunks: Iterable[bytes]) -> str:
    """Atomically write byte chunks and return the completed file's SHA-256 digest."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    digest = hashlib.sha256()
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as output:
            temporary_path = Path(output.name)
            for chunk in chunks:
                digest.update(chunk)
                output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_path, path)
        return digest.hexdigest()
    except BaseException:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
        raise


def write_atomic_json(path: Path, value: object) -> str:
    """Atomically write a deterministic, human-readable JSON document."""
    encoded = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    return write_atomic_bytes(path, (encoded,))


def write_fasta_stream(
    path: Path,
    name: str,
    sequence_chunks: Iterable[bytes | bytearray | memoryview],
    *,
    expected_length: int,
    exact_pattern: bytes | None = None,
    expected_match_positions: list[int] | None = None,
) -> tuple[FastaWriteResult, list[int] | None]:
    """Atomically stream one wrapped FASTA record and optionally verify exact matches.

    Match detection uses KMP state carried across input chunks, so occurrences that
    overlap one another or a streaming boundary are counted correctly.  A reference
    is committed only after its full exact-position invariant succeeds.
    """
    if not name.isascii() or "\n" in name or "\r" in name:
        raise ValueError("FASTA record names must be single-line ASCII")
    if (exact_pattern is None) != (expected_match_positions is None):
        raise ValueError("exact_pattern and expected_match_positions must be supplied together")

    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    file_digest = hashlib.sha256()
    sequence_digest = hashlib.sha256()
    matcher = OverlappingMatcher(exact_pattern) if exact_pattern is not None else None
    sequence_length = 0
    column = 0

    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as output:
            temporary_path = Path(output.name)

            header = b">" + name.encode("ascii") + b"\n"
            output.write(header)
            file_digest.update(header)

            for raw_chunk in sequence_chunks:
                chunk = memoryview(raw_chunk)
                if not chunk:
                    continue
                sequence_digest.update(chunk)
                if matcher is not None:
                    matcher.feed(chunk)
                sequence_length += len(chunk)

                offset = 0
                while offset < len(chunk):
                    width = min(FASTA_LINE_WIDTH - column, len(chunk) - offset)
                    piece = chunk[offset : offset + width]
                    output.write(piece)
                    file_digest.update(piece)
                    offset += width
                    column += width
                    if column == FASTA_LINE_WIDTH:
                        output.write(b"\n")
                        file_digest.update(b"\n")
                        column = 0

            if column:
                output.write(b"\n")
                file_digest.update(b"\n")
            output.flush()
            os.fsync(output.fileno())

        if sequence_length != expected_length:
            raise RuntimeError(
                f"{path}: expected {expected_length} sequence bases, wrote {sequence_length}"
            )

        observed_positions = matcher.positions if matcher is not None else None
        if expected_match_positions is not None and observed_positions != expected_match_positions:
            raise RuntimeError(
                f"{path}: intended exact source positions {expected_match_positions}, "
                f"observed overlapping positions {observed_positions}; no retry was attempted"
            )

        os.replace(temporary_path, path)
        result = FastaWriteResult(
            file_sha256=file_digest.hexdigest(),
            sequence_sha256=sequence_digest.hexdigest(),
            sequence_length=sequence_length,
        )
        return result, observed_positions
    except BaseException:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
        raise


def make_nested_placements(
    target_length: int, source_length: int, max_n: int, seed: int
) -> tuple[list[Placement], int]:
    """Place one copy near each cell start, then shuffle once for nested prefixes.

    Restricting the offset to half of the unused cell width leaves a positive
    inter-copy gap whenever the cell is wider than the source.  For the primary
    4.5-Mbp/1-kbp geometry, adjacent starts are therefore at least 1,063 bases
    apart and no 24-base minimizer window can cross a copy boundary.
    """
    cell_width = target_length // max_n
    if source_length > cell_width:
        raise ValueError(
            "source length exceeds fixed-cell width; nonoverlapping max-N placement is impossible"
        )
    rng = random.Random(seed)
    maximum_offset = (cell_width - source_length) // 2
    placement_pairs = [
        (
            cell_index,
            cell_index * cell_width + rng.randrange(maximum_offset + 1),
        )
        for cell_index in range(max_n)
    ]
    rng.shuffle(placement_pairs)
    placements = [
        Placement(
            rank=rank,
            cell_index=cell_index,
            start=start,
            end=start + source_length,
        )
        for rank, (cell_index, start) in enumerate(placement_pairs, start=1)
    ]
    return placements, cell_width


def minimum_copy_gap(positions: Iterable[int], source_length: int) -> int | None:
    """Return the smallest uncovered gap between copies, or ``None`` for fewer than two."""
    ordered = sorted(positions)
    if len(ordered) < 2:
        return None
    return min(right - left - source_length for left, right in zip(ordered, ordered[1:]))


def minimum_start_separation(positions: Iterable[int]) -> int | None:
    """Return the smallest distance between adjacent copy starts, if one exists."""
    ordered = sorted(positions)
    if len(ordered) < 2:
        return None
    return min(right - left for left, right in zip(ordered, ordered[1:]))


def realize_orientation(source: bytes, policy: str, seed: int) -> tuple[bytes, str]:
    """Apply the declared forward, reverse-complement, or deterministic-random policy."""
    if policy == "forward":
        orientation = "forward"
    elif policy == "reverse-complement":
        orientation = "reverse-complement"
    elif policy == "random":
        orientation = (
            "reverse-complement" if random.Random(seed).getrandbits(1) else "forward"
        )
    else:
        raise ValueError(f"unsupported orientation policy: {policy}")
    if orientation == "reverse-complement":
        return source.translate(COMPLEMENT)[::-1], orientation
    return source, orientation


def mutate_query(
    oriented_source: bytes,
    *,
    seed: int,
    substitution_rate: float,
    insertion_rate: float,
    deletion_rate: float,
    insertion_length_mean: float,
    insertion_length_stddev: float,
    deletion_length_mean: float,
    deletion_length_stddev: float,
) -> tuple[bytes, dict[str, int]]:
    """Apply deterministic substitutions followed by insertion/deletion events.

    The indel pass retains each visited base, may insert after it, and may delete a
    Gaussian-distributed number of following bases.  Nonpositive sampled lengths
    are recorded as rate hits but make no edit, matching the original experiment's
    mutation semantics.
    """
    rng = random.Random(seed)
    substituted = bytearray(oriented_source)
    substitutions = 0
    for index, original in enumerate(substituted):
        if rng.random() < substitution_rate:
            original_code = DNA.index(original)
            substituted[index] = DNA[(original_code + 1 + rng.randrange(3)) & 3]
            substitutions += 1

    result = bytearray()
    insertion_rate_hits = 0
    insertion_events = 0
    inserted_bases = 0
    deletion_rate_hits = 0
    deletion_events = 0
    deleted_bases = 0
    index = 0
    while index < len(substituted):
        result.append(substituted[index])
        if rng.random() < insertion_rate:
            insertion_rate_hits += 1
            insertion_length = max(
                0, round(rng.gauss(insertion_length_mean, insertion_length_stddev))
            )
            if insertion_length:
                insertion_events += 1
                inserted_bases += insertion_length
                result.extend(DNA[rng.randrange(4)] for _ in range(insertion_length))
        if rng.random() < deletion_rate:
            deletion_rate_hits += 1
            deletion_length = max(
                0, round(rng.gauss(deletion_length_mean, deletion_length_stddev))
            )
            actual_deletion = min(deletion_length, len(substituted) - index - 1)
            if actual_deletion:
                deletion_events += 1
                deleted_bases += actual_deletion
                index += actual_deletion
        index += 1

    realized_edits = {
        "substitutions": substitutions,
        "insertion_rate_hits": insertion_rate_hits,
        "insertion_events_with_positive_length": insertion_events,
        "inserted_bases": inserted_bases,
        "deletion_rate_hits": deletion_rate_hits,
        "deletion_events_with_positive_length": deletion_events,
        "deleted_bases": deleted_bases,
        "total_positive_length_edit_events": substitutions
        + insertion_events
        + deletion_events,
        "total_edited_bases": substitutions + inserted_bases + deleted_bases,
        "net_length_change": inserted_bases - deleted_bases,
    }
    return bytes(result), realized_edits


def placement_tsv_chunks(placements: Iterable[Placement]) -> Iterator[bytes]:
    """Yield a deterministic placement table in nested-prefix rank order."""
    yield b"rank\tcell_index\tstart\tend\n"
    for placement in placements:
        yield (
            f"{placement.rank}\t{placement.cell_index}\t"
            f"{placement.start}\t{placement.end}\n"
        ).encode("ascii")


def artifact_path(path: Path, output_root: Path) -> str:
    """Return a POSIX artifact path relative to the manifest directory."""
    return path.relative_to(output_root).as_posix()


def mutation_model(args: argparse.Namespace) -> dict[str, Any]:
    """Return the fully declared mutation and orientation configuration."""
    return {
        "orientation_policy": args.orientation_policy,
        "orientation_applied_before_errors": True,
        "substitution_rate": args.substitution_rate,
        "insertion_event_rate": args.insertion_rate,
        "deletion_event_rate": args.deletion_rate,
        "insertion_length_mean": args.insertion_length_mean,
        "insertion_length_stddev": args.insertion_length_stddev,
        "deletion_length_mean": args.deletion_length_mean,
        "deletion_length_stddev": args.deletion_length_stddev,
        "implementation": (
            "substitutions over the oriented source, then independent per-visited-base "
            "insertion and following-base deletion rolls using Python Random"
        ),
    }


def generate_dataset(
    output_root: Path,
    args: argparse.Namespace,
    *,
    replicate_index: int,
    master_seed: int,
) -> dict[str, Any]:
    """Generate and validate one predeclared master-seed replicate exactly once."""
    replicate_id = f"replicate_{replicate_index:03d}"
    dataset_id = f"{args.arm_id}__{replicate_id}"
    dataset_dir = output_root / args.arm_id / replicate_id
    dataset_dir.mkdir(parents=True, exist_ok=False)

    seeds = {
        "reference": derive_seed(master_seed, args.arm_id, replicate_id, "reference"),
        "placement": derive_seed(master_seed, args.arm_id, replicate_id, "placement"),
        "mutation": derive_seed(master_seed, args.arm_id, replicate_id, "mutation"),
        "orientation": derive_seed(master_seed, args.arm_id, replicate_id, "orientation"),
    }
    background = uniform_dna(args.target_length, seeds["reference"])
    placements, cell_width = make_nested_placements(
        args.target_length, args.source_length, args.max_n, seeds["placement"]
    )
    all_positions = [placement.start for placement in placements]
    source = background[
        placements[0].start : placements[0].start + args.source_length
    ]

    source_path = dataset_dir / "source.fa"
    source_result, _ = write_fasta_stream(
        source_path,
        f"{dataset_id}__source",
        (source,),
        expected_length=args.source_length,
    )

    oriented_source, realized_orientation = realize_orientation(
        source, args.orientation_policy, seeds["orientation"]
    )
    query, edit_counts = mutate_query(
        oriented_source,
        seed=seeds["mutation"],
        substitution_rate=args.substitution_rate,
        insertion_rate=args.insertion_rate,
        deletion_rate=args.deletion_rate,
        insertion_length_mean=args.insertion_length_mean,
        insertion_length_stddev=args.insertion_length_stddev,
        deletion_length_mean=args.deletion_length_mean,
        deletion_length_stddev=args.deletion_length_stddev,
    )
    query_path = dataset_dir / "query.fa"
    query_result, _ = write_fasta_stream(
        query_path,
        f"{dataset_id}__query",
        (query,),
        expected_length=len(query),
    )

    placements_path = dataset_dir / "placements.tsv"
    placements_sha256 = write_atomic_bytes(
        placements_path, placement_tsv_chunks(placements)
    )

    references: dict[str, dict[str, Any]] = {}
    for n_value in args.n_values:
        intended_positions = sorted(all_positions[:n_value])
        intended_positions_sha256 = canonical_json_sha256(intended_positions)
        reference_path = dataset_dir / f"reference_N{n_value}.fa"
        reference_result, observed_positions = write_fasta_stream(
            reference_path,
            f"{dataset_id}__reference_N{n_value}",
            iter_reference_chunks(background, source, intended_positions),
            expected_length=args.target_length,
            exact_pattern=source,
            expected_match_positions=intended_positions,
        )
        assert observed_positions is not None
        references[str(n_value)] = {
            "path": artifact_path(reference_path, output_root),
            "sha256": reference_result.file_sha256,
            "sequence_sha256": reference_result.sequence_sha256,
            "intended_occurrences": n_value,
            "intended_positions": intended_positions,
            "intended_positions_sha256": intended_positions_sha256,
            "verified_exact_source_occurrences_overlapping": len(observed_positions),
            "verified_positions_match_intended": observed_positions == intended_positions,
            "verified_positions_sha256": canonical_json_sha256(observed_positions),
            "minimum_copy_gap": minimum_copy_gap(
                intended_positions, args.source_length
            ),
            "minimum_copy_start_separation": minimum_start_separation(
                intended_positions
            ),
            "copy_fraction": n_value * args.source_length / args.target_length,
        }

    full_grid_gap = minimum_copy_gap(all_positions, args.source_length)
    if full_grid_gap is not None and full_grid_gap < 0:
        raise RuntimeError("fixed-cell placement unexpectedly produced overlapping copies")

    realized_edits: dict[str, Any] = {
        "orientation": realized_orientation,
        "reverse_complement": realized_orientation == "reverse-complement",
        "input_length": len(source),
        "oriented_length": len(oriented_source),
        "output_length": len(query),
        **edit_counts,
    }
    return {
        "dataset_id": dataset_id,
        "arm_id": args.arm_id,
        "replicate_id": replicate_id,
        "master_seed": master_seed,
        "seeds": {
            "derivation": (
                "first unsigned 64 bits, big-endian, of SHA-256 over canonical JSON "
                "containing namespace, arm_id, replicate_id, master seed, and domain"
            ),
            **seeds,
        },
        "reference_length": args.target_length,
        "source_length": args.source_length,
        "query_length": len(query),
        "max_n": args.max_n,
        "n_values": args.n_values,
        "cell_width": cell_width,
        "unused_target_tail_bases": args.target_length - cell_width * args.max_n,
        "minimum_full_grid_copy_gap": full_grid_gap,
        "minimum_full_grid_copy_start_separation": minimum_start_separation(
            all_positions
        ),
        "reference_model": "iid_uniform_DNA4_from_SHAKE256_byte_stream",
        "placement_model": (
            "one bounded offset in the first half of each fixed-width cell's "
            "unused span, shuffled once, then nested by placement-rank prefix"
        ),
        "source": {
            "path": artifact_path(source_path, output_root),
            "sha256": source_result.file_sha256,
            "sequence_sha256": source_result.sequence_sha256,
            "length": source_result.sequence_length,
            "origin_placement_rank": 1,
            "origin_start": placements[0].start,
        },
        "query": {
            "path": artifact_path(query_path, output_root),
            "sha256": query_result.file_sha256,
            "sequence_sha256": query_result.sequence_sha256,
            "length": query_result.sequence_length,
        },
        "placements": {
            "path": artifact_path(placements_path, output_root),
            "sha256": placements_sha256,
            "count": len(placements),
            "positions_sha256": canonical_json_sha256(all_positions),
            "nested_prefix_order": True,
            "nonoverlapping": True,
        },
        "mutation_model": mutation_model(args),
        "realized_edits": realized_edits,
        "references": references,
    }


def parse_integer_list(tokens: Iterable[str], option_name: str) -> list[int]:
    """Parse whitespace- and comma-separated decimal integers from CLI tokens."""
    values: list[int] = []
    for token in tokens:
        for field in token.split(","):
            stripped = field.strip()
            if not stripped:
                continue
            try:
                values.append(int(stripped, 10))
            except ValueError as error:
                raise ValueError(
                    f"{option_name} contains a non-decimal integer: {stripped!r}"
                ) from error
    if not values:
        raise ValueError(f"{option_name} must contain at least one integer")
    return values


def parse_args() -> argparse.Namespace:
    """Parse the generic follow-up generator command line."""
    parser = argparse.ArgumentParser(
        description=(
            "Generate predeclared deterministic nested-copy Run_L follow-up datasets. "
            "The generator never runs minimap2 or retries based on mapping outcomes."
        )
    )
    parser.add_argument(
        "--output-dir",
        "--output",
        dest="output_dir",
        type=Path,
        default=Path("followup_data"),
        help="output root; the manifest is written as OUTPUT_DIR/manifest.json",
    )
    parser.add_argument("--arm-id", default=DEFAULT_ARM_ID)
    parser.add_argument("--target-length", type=int, default=DEFAULT_TARGET_LENGTH)
    parser.add_argument("--source-length", type=int, default=DEFAULT_SOURCE_LENGTH)
    parser.add_argument("--max-n", type=int, default=DEFAULT_MAX_N)
    parser.add_argument(
        "--n-list",
        nargs="+",
        default=[str(value) for value in DEFAULT_N_VALUES],
        metavar="N",
        help="strictly increasing N values; comma-separated tokens are also accepted",
    )
    parser.add_argument(
        "--master-seed-list",
        nargs="+",
        default=[str(value) for value in DEFAULT_MASTER_SEEDS],
        metavar="SEED",
        help="ordered, unique nonnegative master seeds; comma-separated tokens are accepted",
    )
    parser.add_argument(
        "--substitution-rate",
        "--base-swap-rate",
        dest="substitution_rate",
        type=float,
        default=0.001,
    )
    parser.add_argument(
        "--insertion-rate",
        "--insert-rate",
        dest="insertion_rate",
        type=float,
        default=0.0005,
    )
    parser.add_argument(
        "--deletion-rate",
        "--delete-rate",
        dest="deletion_rate",
        type=float,
        default=0.0005,
    )
    parser.add_argument("--insertion-length-mean", type=float, default=1.0)
    parser.add_argument("--insertion-length-stddev", type=float, default=1.0)
    parser.add_argument("--deletion-length-mean", type=float, default=1.0)
    parser.add_argument("--deletion-length-stddev", type=float, default=1.0)
    parser.add_argument(
        "--orientation-policy",
        choices=("forward", "reverse-complement", "random"),
        default="random",
    )
    parser.add_argument(
        "--mapping-default-args",
        default="-k 15 -w 10",
        help="shell-token string recorded for every mapping run",
    )
    parser.add_argument(
        "--mapping-arm-args",
        default="",
        help="additional shell-token string recorded for this arm",
    )
    parser.add_argument("--mapping-threads", type=int, default=1)
    parser.add_argument("--mapping-timeout-seconds", type=int, default=3_600)
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> None:
    """Normalize list arguments and reject unsafe or impossible declarations."""
    if not SAFE_IDENTIFIER.fullmatch(args.arm_id):
        raise ValueError(
            "--arm-id must be 1-128 ASCII letters, digits, dots, underscores, or hyphens "
            "and must begin with a letter or digit"
        )
    args.n_values = parse_integer_list(args.n_list, "--n-list")
    args.master_seeds = parse_integer_list(
        args.master_seed_list, "--master-seed-list"
    )
    if args.target_length <= 0 or args.source_length <= 0 or args.max_n <= 0:
        raise ValueError("target length, source length, and max N must all be positive")
    if any(value <= 0 for value in args.n_values):
        raise ValueError("every N value must be positive")
    if args.n_values != sorted(set(args.n_values)):
        raise ValueError("--n-list values must be unique and strictly increasing")
    if args.n_values[-1] > args.max_n:
        raise ValueError("the largest requested N exceeds --max-n")
    if any(seed < 0 for seed in args.master_seeds):
        raise ValueError("master seeds must be nonnegative")
    if len(args.master_seeds) != len(set(args.master_seeds)):
        raise ValueError("master seeds must be unique")
    if args.source_length > args.target_length // args.max_n:
        raise ValueError(
            "source_length must not exceed floor(target_length / max_n)"
        )
    for name in ("substitution_rate", "insertion_rate", "deletion_rate"):
        value = getattr(args, name)
        if not math.isfinite(value) or not 0.0 <= value <= 1.0:
            raise ValueError(f"--{name.replace('_', '-')} must be finite and in [0, 1]")
    for name in (
        "insertion_length_mean",
        "insertion_length_stddev",
        "deletion_length_mean",
        "deletion_length_stddev",
    ):
        value = getattr(args, name)
        if not math.isfinite(value) or value < 0.0:
            raise ValueError(f"--{name.replace('_', '-')} must be finite and nonnegative")
    if args.mapping_threads <= 0 or args.mapping_timeout_seconds <= 0:
        raise ValueError("mapping threads and timeout must be positive")
    args.mapping_default_argv = shlex.split(args.mapping_default_args, posix=True)
    args.mapping_arm_argv = shlex.split(args.mapping_arm_args, posix=True)


def declared_inputs(args: argparse.Namespace) -> dict[str, Any]:
    """Return the complete outcome-independent generation declaration."""
    return {
        "output_dir": str(args.output_dir),
        "arm_id": args.arm_id,
        "target_length": args.target_length,
        "source_length": args.source_length,
        "max_n": args.max_n,
        "n_values": args.n_values,
        "master_seeds": args.master_seeds,
        "mutation": mutation_model(args),
        "orientation_policy": args.orientation_policy,
    }


def initial_manifest(args: argparse.Namespace, script_path: Path) -> dict[str, Any]:
    """Construct the pre-generation manifest written before any replicate is sampled."""
    planned_dataset_ids = [
        f"{args.arm_id}__replicate_{index:03d}"
        for index in range(1, len(args.master_seeds) + 1)
    ]
    return {
        "schema_version": SCHEMA_VERSION,
        "status": "generation_in_progress",
        "generator": {
            "script": script_path.name,
            "script_sha256": sha256_file(script_path),
            "algorithm_namespace": ALGORITHM_NAMESPACE,
        },
        "declared_inputs": declared_inputs(args),
        "generation_policy": {
            "generation_attempts_per_replicate": 1,
            "predeclared_dataset_ids": planned_dataset_ids,
            "mapping_results_available_to_generator": False,
            "retry_or_selection_based_on_mapping_results": False,
            "retain_every_predeclared_replicate": True,
        },
        "mapping": {
            "default_args": args.mapping_default_argv,
            "arms": [{"arm_id": args.arm_id, "args": args.mapping_arm_argv}],
            "mapping_threads": args.mapping_threads,
            "timeout_seconds": args.mapping_timeout_seconds,
        },
        "datasets": [],
    }


def preflight_output(output_root: Path, args: argparse.Namespace) -> None:
    """Refuse to overwrite an existing manifest or planned replicate directory."""
    manifest_path = output_root / "manifest.json"
    if manifest_path.exists():
        raise FileExistsError(f"refusing to overwrite existing manifest: {manifest_path}")
    for index in range(1, len(args.master_seeds) + 1):
        dataset_dir = output_root / args.arm_id / f"replicate_{index:03d}"
        if dataset_dir.exists():
            raise FileExistsError(
                f"refusing to reuse existing replicate directory: {dataset_dir}"
            )


def main() -> None:
    """Validate the declaration, generate every replicate once, and finalize its manifest."""
    args = parse_args()
    validate_args(args)
    output_root = args.output_dir.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    preflight_output(output_root, args)

    script_path = Path(__file__).resolve()
    manifest_path = output_root / "manifest.json"
    manifest = initial_manifest(args, script_path)
    write_atomic_json(manifest_path, manifest)

    datasets = [
        generate_dataset(
            output_root,
            args,
            replicate_index=index,
            master_seed=master_seed,
        )
        for index, master_seed in enumerate(args.master_seeds, start=1)
    ]
    manifest["datasets"] = datasets
    manifest["status"] = "complete"
    write_atomic_json(manifest_path, manifest)
    print(manifest_path)


if __name__ == "__main__":
    main()
