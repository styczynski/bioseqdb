#!/usr/bin/env python3
"""Generate and validate a concrete FASTA realization of the repaired P/G/C/R witness.

This is an isolated proof-of-concept for the Run_L experiment.  It does not
modify minimap2.  Each intended anchor is represented by one unique 25-base
DNA island surrounded by at least ``w`` ambiguous bases.  For k=15,w=10 the
island has eleven k-mers.  The desired k-mer is the unique minimum among the
first ten, while the eleventh has a still smaller hash.  On the eleventh
k-mer, minimap2 emits the previous minimum; the following N guard discards the
new pending minimum.  Thus every island emits exactly one minimizer.

This is a coordinate/island adversarial control, not evidence about the
frequency of biological repeats.  Its purpose is to realize an exact anchor
geometry whose expected predecessor array is known in advance.

The construction repairs the commented thesis witness so that the first
score-ranked predecessor path starts at e_m rather than at h:

* 40 P anchors give f(h)=600;
* 10 G links with (dr,dq)=(1500,1000) give f(g_10)=110;
* c_0 follows by (1275,1000), giving f(c_0)=88;
* C links alternate (3500,3500-100*s_k), retaining score 88;
* each R_k has 39 anchors, starts at offset (500,500-450*s_k), and advances
  by (38,38).  Its scores are 45,60,...,615, so e_k has score 615.

The script contains a direct non-HPC port of minimap2's mm_sketch routine and
validates both generated FASTA sequences before optionally invoking the
already-built, experiment-instrumented standalone minimap2.  A successful
real run additionally requires exact SD coordinates, exact f/p arrays, exact
Run_L counters, and at least one PAF record.

The Python gap-score arithmetic is a non-contracted binary32 prediction of
the C expressions.  Compiler/architecture contraction can change a value at
an integer boundary, so the real minimap2 f/p dump—not this prediction—is the
ground truth whenever mapping validation is requested.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
from typing import Iterable


K = 15
W = 10
ISLAND_LENGTH = K + W
DESIRED_KMER_OFFSET = 5
DESIRED_END_OFFSET = DESIRED_KMER_OFFSET + K - 1
UINT64_MAX = (1 << 64) - 1
RUN_L_WIDTHS = (2, 4, 8, 16, 32, 64)
SCRIPT_DIRECTORY = Path(__file__).resolve().parent
DEFAULT_MAPPING_TIMEOUT_SECONDS = 300.0


def hash64(key: int, mask: int) -> int:
    """Port minimap2 sketch.c::hash64 exactly for a 2*k-bit key."""

    key = (~key + (key << 21)) & mask
    key ^= key >> 24
    key = ((key + (key << 3)) + (key << 8)) & mask
    key ^= key >> 14
    key = ((key + (key << 2)) + (key << 4)) & mask
    key ^= key >> 28
    key = (key + (key << 31)) & mask
    return key


def kmer_hashes(sequence: str, k: int = K) -> list[tuple[int, int]]:
    """Return minimap2 canonical hashes and strands for every sequence k-mer."""

    nt = {"A": 0, "C": 1, "G": 2, "T": 3}
    mask = (1 << (2 * k)) - 1
    shift = 2 * (k - 1)
    forward = 0
    reverse = 0
    result: list[tuple[int, int]] = []
    for index, base in enumerate(sequence):
        code = nt[base]
        forward = ((forward << 2) | code) & mask
        reverse = (reverse >> 2) | ((3 ^ code) << shift)
        if index + 1 < k:
            continue
        if forward == reverse:
            raise ValueError("symmetric k-mer unexpectedly encountered")
        strand = 0 if forward < reverse else 1
        result.append((hash64(forward if strand == 0 else reverse, mask), strand))
    return result


def sketch_minimizers(sequence: str, w: int = W, k: int = K) -> list[dict[str, int]]:
    """Direct non-HPC port of minimap2's mm_sketch, including N-boundary behavior."""

    nt = {"A": 0, "C": 1, "G": 2, "T": 3}
    shift = 2 * (k - 1)
    mask = (1 << (2 * k)) - 1
    forward = 0
    reverse = 0
    valid_length = 0
    buffer_position = 0
    minimum_position = 0
    buffer: list[tuple[int, int]] = [(UINT64_MAX, UINT64_MAX) for _ in range(w)]
    minimum = (UINT64_MAX, UINT64_MAX)
    output: list[tuple[int, int]] = []

    for position, base in enumerate(sequence):
        code = nt.get(base, 4)
        info = (UINT64_MAX, UINT64_MAX)
        if code < 4:
            span = min(valid_length + 1, k)
            forward = ((forward << 2) | code) & mask
            reverse = (reverse >> 2) | ((3 ^ code) << shift)
            if forward == reverse:
                # k is odd here, so this branch is unreachable, but it mirrors C.
                continue
            strand = 0 if forward < reverse else 1
            valid_length += 1
            if valid_length >= k:
                canonical = forward if strand == 0 else reverse
                info = ((hash64(canonical, mask) << 8) | span, (position << 1) | strand)
        else:
            valid_length = 0

        buffer[buffer_position] = info
        if valid_length == w + k - 1 and minimum[0] != UINT64_MAX:
            for scan_position in range(buffer_position + 1, w):
                if minimum[0] == buffer[scan_position][0] and buffer[scan_position][1] != minimum[1]:
                    output.append(buffer[scan_position])
            for scan_position in range(0, buffer_position):
                if minimum[0] == buffer[scan_position][0] and buffer[scan_position][1] != minimum[1]:
                    output.append(buffer[scan_position])

        if info[0] <= minimum[0]:
            if valid_length >= w + k and minimum[0] != UINT64_MAX:
                output.append(minimum)
            minimum = info
            minimum_position = buffer_position
        elif buffer_position == minimum_position:
            if valid_length >= w + k - 1 and minimum[0] != UINT64_MAX:
                output.append(minimum)
            minimum = (UINT64_MAX, UINT64_MAX)
            for scan_position in range(buffer_position + 1, w):
                if minimum[0] >= buffer[scan_position][0]:
                    minimum = buffer[scan_position]
                    minimum_position = scan_position
            for scan_position in range(0, buffer_position + 1):
                if minimum[0] >= buffer[scan_position][0]:
                    minimum = buffer[scan_position]
                    minimum_position = scan_position
            if valid_length >= w + k - 1 and minimum[0] != UINT64_MAX:
                for scan_position in range(buffer_position + 1, w):
                    if minimum[0] == buffer[scan_position][0] and minimum[1] != buffer[scan_position][1]:
                        output.append(buffer[scan_position])
                for scan_position in range(0, buffer_position + 1):
                    if minimum[0] == buffer[scan_position][0] and minimum[1] != buffer[scan_position][1]:
                        output.append(buffer[scan_position])

        buffer_position += 1
        if buffer_position == w:
            buffer_position = 0

    if minimum[0] != UINT64_MAX:
        output.append(minimum)

    return [
        {
            "hash": packed_x >> 8,
            "span": packed_x & 0xFF,
            "position": packed_y >> 1,
            "strand": packed_y & 1,
        }
        for packed_x, packed_y in output
    ]


def float32(value: float) -> float:
    """Round through IEEE-754 binary32, as minimap2's scoring code does."""

    return struct.unpack("=f", struct.pack("=f", value))[0]


def mg_log2(value: float) -> float:
    """Predict mmpriv.h::mg_log2 using explicit, non-contracted binary32 steps."""

    value = float32(value)
    bits = struct.unpack("=I", struct.pack("=f", value))[0]
    log_2 = float32(((bits >> 23) & 255) - 128)
    bits &= ~(255 << 23)
    bits += 127 << 23
    normalized = struct.unpack("=f", struct.pack("=I", bits))[0]
    inner = float32(float32(float32(-0.34484843) * normalized) + float32(2.02466578))
    polynomial = float32(float32(inner * normalized) - float32(0.67487759))
    return float32(log_2 + polynomial)


def ordinary_penalty(diagonal_width: int) -> int:
    """Predict the integer ordinary map-ont gap penalty for dg>=15."""

    linear = float32(float32(0.12) * float32(diagonal_width))
    logarithmic = mg_log2(float(diagonal_width + 1)) if diagonal_width >= 1 else 0.0
    total = float32(linear + float32(float32(0.5) * logarithmic))
    return int(total)


def find_island(
    rng: random.Random,
    used_kmer_hashes: set[int],
    maximum_attempts: int = 2_000_000,
) -> tuple[str, int, int, int]:
    """Find one unique island and return sequence, emitted hash, strand, attempts."""

    alphabet = "ACGT"
    for attempt in range(1, maximum_attempts + 1):
        island = "".join(rng.choice(alphabet) for _ in range(ISLAND_LENGTH))
        hashes = kmer_hashes(island)
        values = [entry[0] for entry in hashes]
        if len(set(values)) != len(values):
            continue
        first_window_minimum = min(range(W), key=values.__getitem__)
        if first_window_minimum != DESIRED_KMER_OFFSET:
            continue
        if values[W] >= values[first_window_minimum]:
            continue
        if any(value in used_kmer_hashes for value in values):
            continue
        used_kmer_hashes.update(values)
        chosen_hash, chosen_strand = hashes[first_window_minimum]
        return island, chosen_hash, chosen_strand, attempt
    raise RuntimeError(f"unable to find a minimizer island after {maximum_attempts} attempts")


def build_anchor_geometry(m: int) -> list[dict[str, object]]:
    """Construct the repaired anchor list in exact sorted reference order."""

    if m < 1:
        raise ValueError("m must be positive")
    anchors: list[dict[str, object]] = []
    label_to_index: dict[str, int] = {}

    def add(label: str, reference: int, query: int, score: int, predecessor: str | None) -> None:
        if predecessor is not None and predecessor not in label_to_index:
            raise AssertionError(f"unknown predecessor {predecessor} for {label}")
        index = len(anchors)
        anchors.append(
            {
                "index": index,
                "label": label,
                "reference": reference,
                "query": query,
                "score": score,
                "predecessor": -1 if predecessor is None else label_to_index[predecessor],
            }
        )
        label_to_index[label] = index

    reference = 1_000
    query = 1_000
    for position in range(40):
        if position > 0:
            reference += 100
            query += 100
        add(f"p{position:02d}", reference, query, 15 * (position + 1), None if position == 0 else f"p{position - 1:02d}")

    previous_label = "p39"
    for position in range(1, 11):
        reference += 1_500
        query += 1_000
        label = f"g{position:02d}"
        add(label, reference, query, 600 - 49 * position, previous_label)
        previous_label = label

    reference += 1_275
    query += 1_000
    add("c000", reference, query, 88, "g10")
    previous_c = "c000"

    for group in range(1, m + 1):
        sign = 1 if group % 2 == 1 else -1
        reference += 3_500
        query += 3_500 - 100 * sign
        c_label = f"c{group:03d}"
        add(c_label, reference, query, 88, previous_c)
        c_reference = reference
        c_query = query
        previous_r = c_label
        for branch_position in range(1, 40):
            branch_reference = c_reference + 500 + 38 * (branch_position - 1)
            branch_query = c_query + 500 - 450 * sign + 38 * (branch_position - 1)
            r_label = f"r{group:03d}_{branch_position:02d}"
            add(r_label, branch_reference, branch_query, 45 + 15 * (branch_position - 1), previous_r)
            previous_r = r_label
        previous_c = c_label

    expected_anchor_count = 40 * m + 51
    if len(anchors) != expected_anchor_count:
        raise AssertionError(f"anchor-count identity failed: {len(anchors)} != {expected_anchor_count}")
    if anchors != sorted(anchors, key=lambda anchor: (int(anchor["reference"]), int(anchor["query"]))):
        raise AssertionError("constructed anchors are not already in minimap2 reference/query order")
    for previous, current in zip(anchors, anchors[1:]):
        if int(current["query"]) <= int(previous["query"]):
            raise AssertionError(f"query order is not increasing at {previous['label']} -> {current['label']}")
    return anchors


def place_island(sequence: list[str], start: int, island: str, label: str) -> None:
    """Place one island and reject any accidental overlap."""

    if start < 0 or start + len(island) > len(sequence):
        raise AssertionError(f"island {label} lies outside its sequence")
    if any(base != "N" for base in sequence[start : start + len(island)]):
        raise AssertionError(f"island {label} overlaps an earlier island")
    sequence[start : start + len(island)] = island


def write_fasta(path: Path, name: str, sequence: str) -> None:
    """Write one wrapped FASTA record."""

    with path.open("w", encoding="ascii") as output:
        output.write(f">{name}\n")
        for offset in range(0, len(sequence), 80):
            output.write(sequence[offset : offset + 80] + "\n")


def sha256_file(path: Path) -> str:
    """Return the SHA-256 digest of a file's exact bytes."""

    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def describe_minimap2(minimap2: Path, timeout_seconds: float) -> dict[str, str]:
    """Hash an executable minimap2 and capture its reported version."""

    if not minimap2.is_file():
        raise ValueError(f"minimap2 executable not found: {minimap2}")
    if not os.access(minimap2, os.X_OK):
        raise ValueError(f"minimap2 is not executable: {minimap2}")
    try:
        version_run = subprocess.run(
            [str(minimap2), "--version"],
            capture_output=True,
            text=True,
            check=False,
            timeout=min(timeout_seconds, 30.0),
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"minimap2 --version exceeded its timeout: {minimap2}") from error
    version_lines = (version_run.stdout.strip() or version_run.stderr.strip()).splitlines()
    if version_run.returncode != 0 or not version_lines:
        raise RuntimeError(
            f"unable to obtain minimap2 version from {minimap2}; status={version_run.returncode}"
        )
    return {
        "path": str(minimap2),
        "sha256": sha256_file(minimap2),
        "version": version_lines[0],
    }


def expected_path_profile(m: int) -> tuple[int, tuple[int, ...]]:
    """Reconstruct the e_m path and its maximal unit-predecessor runs.

    A unit relation is ``p[j] == j - 1``.  This deliberately includes the
    terminal relation ``p[0] == -1``.  At m=1 the front and tail portions meet
    through adjacent c_1 and c_0 anchors and therefore form one 91-link run;
    for m>=2, nonunit C predecessors separate the 39- and 52-link runs.
    """

    anchors = build_anchor_geometry(m)
    predecessors = [int(anchor["predecessor"]) for anchor in anchors]
    current = len(anchors) - 1
    path_length = 0
    current_unit_run = 0
    unit_runs: list[int] = []
    visited: set[int] = set()
    while current >= 0:
        if current >= len(predecessors) or current in visited:
            raise AssertionError(f"invalid expected predecessor path at anchor {current}")
        visited.add(current)
        predecessor = predecessors[current]
        if predecessor < -1 or predecessor >= current:
            raise AssertionError(
                f"invalid expected predecessor {predecessor} at anchor {current}"
            )
        path_length += 1
        if predecessor == current - 1:
            current_unit_run += 1
        elif current_unit_run > 0:
            unit_runs.append(current_unit_run)
            current_unit_run = 0
        current = predecessor
    if current_unit_run > 0:
        unit_runs.append(current_unit_run)
    return path_length, tuple(unit_runs)


def expected_runl(m: int, lanes: int) -> dict[str, int]:
    """Emulate every fixed MM_RUNL field over the declared f/p arrays."""

    if lanes < 1:
        raise ValueError("Run_L lane width must be positive")
    anchors = build_anchor_geometry(m)
    anchor_count = len(anchors)
    predecessors = [int(anchor["predecessor"]) for anchor in anchors]
    scores = [int(anchor["score"]) for anchor in anchors]
    min_score = 40
    eligible_ends = sorted(
        (score, index) for index, score in enumerate(scores) if score >= min_score
    )
    start_index = eligible_ends[-1][1] if eligible_ends else -1
    start_score = eligible_ends[-1][0] if eligible_ends else -1

    scalar_path_length = 0
    scalar_validation_error = 0
    scalar_current = start_index
    while scalar_current >= 0 and scalar_path_length < anchor_count:
        if scalar_current >= anchor_count:
            scalar_validation_error = 1
            break
        predecessor = predecessors[scalar_current]
        scalar_path_length += 1
        if predecessor < -1 or predecessor >= scalar_current:
            scalar_validation_error = 1
            break
        scalar_current = predecessor
    if scalar_current >= 0 and scalar_path_length >= anchor_count:
        scalar_validation_error = 1

    current = start_index
    consumed = 0
    loop_evaluations = 0
    bounds_pass = 0
    bounds_fail_current = 0
    bounds_fail_capacity = 0
    bounds_fail_both = 0
    contiguous_mismatch = 0
    fast_batches = 0
    scalar_fallbacks = 0
    emulation_error = 0
    while current >= 0 and consumed < anchor_count:
        position_ok = current >= lanes - 1
        capacity_ok = consumed <= anchor_count - lanes
        contiguous = True
        loop_evaluations += 1
        if not position_ok or not capacity_ok:
            if not position_ok and not capacity_ok:
                bounds_fail_both += 1
            elif not position_ok:
                bounds_fail_current += 1
            else:
                bounds_fail_capacity += 1
        else:
            bounds_pass += 1
            for offset in range(lanes):
                if predecessors[current - offset] != current - offset - 1:
                    contiguous = False
                    break

        if position_ok and capacity_ok and contiguous:
            fast_batches += 1
            current -= lanes
            consumed += lanes
        else:
            if position_ok and capacity_ok:
                contiguous_mismatch += 1
            scalar_fallbacks += 1
            if current >= anchor_count:
                emulation_error = 1
                break
            predecessor = predecessors[current]
            consumed += 1
            if predecessor < -1 or predecessor >= current:
                emulation_error = 1
                break
            current = predecessor

    path_length, unit_runs = expected_path_profile(m)
    maximal_run_fast_batches = sum(run_length // lanes for run_length in unit_runs)
    if fast_batches != maximal_run_fast_batches:
        raise AssertionError(
            f"greedy/maximal-run fast-batch disagreement for m={m}, L={lanes}: "
            f"greedy={fast_batches}, runs={unit_runs}, run_count={maximal_run_fast_batches}"
        )
    if scalar_path_length != path_length:
        raise AssertionError(
            f"scalar/profile path-length disagreement for m={m}: "
            f"scalar={scalar_path_length}, profile={path_length}"
        )

    return {
        "schema_version": 1,
        "anchor_count": anchor_count,
        "candidate_end_count": len(eligible_ends),
        "start_index": start_index,
        "start_score": start_score,
        "loop_evaluations": loop_evaluations,
        "bounds_pass": bounds_pass,
        "bounds_fail_current": bounds_fail_current,
        "bounds_fail_capacity": bounds_fail_capacity,
        "bounds_fail_both": bounds_fail_both,
        "contiguous_mismatch": contiguous_mismatch,
        "fast_batches": fast_batches,
        "scalar_fallbacks": scalar_fallbacks,
        "primary_path_anchors": consumed,
        "fast_covered_anchors": lanes * fast_batches,
        "scalar_path_length": scalar_path_length,
        "scalar_validation_error": scalar_validation_error,
        "emulation_error": emulation_error,
        "min_score": min_score,
        "has_ranked_end": int(bool(eligible_ends)),
        "has_fast_batch": int(fast_batches > 0),
    }


def run_expected_runl_regression_tests() -> None:
    """Check fixed m=1,2,4 oracle cases at every instrumented width."""

    expected_profiles = {
        1: (91, (91,)),
        2: (92, (39, 52)),
        4: (94, (39, 52)),
    }
    counter_fields = (
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
    expected_counters = {
        1: {
            2: (46, 45, 0, 0, 1, 0, 45, 1, 91, 90, 91, 0, 0, 1, 1),
            4: (25, 22, 0, 0, 3, 0, 22, 3, 91, 88, 91, 0, 0, 1, 1),
            8: (14, 11, 0, 0, 3, 0, 11, 3, 91, 88, 91, 0, 0, 1, 1),
            16: (16, 5, 0, 0, 11, 0, 5, 11, 91, 80, 91, 0, 0, 1, 1),
            32: (29, 2, 0, 0, 27, 0, 2, 27, 91, 64, 91, 0, 0, 1, 1),
            64: (28, 1, 0, 0, 27, 0, 1, 27, 91, 64, 91, 0, 0, 1, 1),
        },
        2: {
            2: (47, 47, 0, 0, 0, 2, 45, 2, 92, 90, 92, 0, 0, 1, 1),
            4: (26, 26, 0, 0, 0, 4, 22, 4, 92, 88, 92, 0, 0, 1, 1),
            8: (22, 18, 4, 0, 0, 8, 10, 12, 92, 80, 92, 0, 0, 1, 1),
            16: (17, 13, 4, 0, 0, 8, 5, 12, 92, 80, 92, 0, 0, 1, 1),
            32: (30, 10, 20, 0, 0, 8, 2, 28, 92, 64, 92, 0, 0, 1, 1),
            64: (92, 40, 28, 0, 24, 40, 0, 92, 92, 0, 92, 0, 0, 1, 0),
        },
        4: {
            2: (49, 49, 0, 0, 0, 4, 45, 4, 94, 90, 94, 0, 0, 1, 1),
            4: (28, 28, 0, 0, 0, 6, 22, 6, 94, 88, 94, 0, 0, 1, 1),
            8: (24, 20, 4, 0, 0, 10, 10, 14, 94, 80, 94, 0, 0, 1, 1),
            16: (19, 15, 4, 0, 0, 10, 5, 14, 94, 80, 94, 0, 0, 1, 1),
            32: (32, 12, 20, 0, 0, 10, 2, 30, 94, 64, 94, 0, 0, 1, 1),
            64: (94, 42, 52, 0, 0, 42, 0, 94, 94, 0, 94, 0, 0, 1, 0),
        },
    }
    for m, (expected_path_length, expected_unit_runs) in expected_profiles.items():
        observed_profile = expected_path_profile(m)
        if observed_profile != (expected_path_length, expected_unit_runs):
            raise AssertionError(
                f"Run_L path-profile regression for m={m}: "
                f"expected {(expected_path_length, expected_unit_runs)}, observed {observed_profile}"
            )
        anchors = build_anchor_geometry(m)
        scores = [int(anchor["score"]) for anchor in anchors]
        top_score = max(scores)
        top_indices = [index for index, score in enumerate(scores) if score == top_score]
        expected_top_indices = [
            index for index, anchor in enumerate(anchors) if anchor["label"].startswith("r") and anchor["label"].endswith("_39")
        ]
        if top_score != 615 or top_indices != expected_top_indices or len(top_indices) != m:
            raise AssertionError(
                f"top-score tie regression for m={m}: score={top_score}, indices={top_indices}, "
                f"expected_indices={expected_top_indices}"
            )
        if top_indices[-1] != len(anchors) - 1:
            raise AssertionError(f"decreasing-position tie rule would not select e_m for m={m}")
        for lanes in RUN_L_WIDTHS:
            observed = expected_runl(m, lanes)
            observed_counters = tuple(observed[field] for field in counter_fields)
            if observed_counters != expected_counters[m][lanes]:
                raise AssertionError(
                    f"Run_L counter regression for m={m}, L={lanes}: "
                    f"expected {expected_counters[m][lanes]}, observed {observed_counters}"
                )
            expected_identities = {
                "schema_version": 1,
                "anchor_count": 40 * m + 51,
                "candidate_end_count": 40 * m + 49,
                "start_index": 40 * m + 50,
                "start_score": 615,
                "min_score": 40,
            }
            for field, expected_value in expected_identities.items():
                if observed[field] != expected_value:
                    raise AssertionError(
                        f"Run_L identity regression for m={m}, L={lanes}, {field}: "
                        f"expected {expected_value}, observed {observed[field]}"
                    )


def parse_dump(
    path: Path,
) -> tuple[dict[str, int | str], list[tuple[int, int]], list[tuple[int, int]]]:
    """Parse one experiment f/p dump."""

    metadata: dict[str, int | str] = {}
    candidates: list[tuple[int, int]] = []
    rows: list[tuple[int, int]] = []
    with path.open("r", encoding="ascii") as source:
        for raw_line in source:
            fields = raw_line.rstrip("\n").split("\t")
            if fields[0] == "META":
                metadata = {
                    "stage": fields[1],
                    "call_id": int(fields[2]),
                    "anchor_count": int(fields[3]),
                    "candidate_end_count": int(fields[4]),
                    "min_score": int(fields[5]),
                }
            elif fields[0] == "Z":
                index = int(fields[1])
                if index != len(candidates):
                    raise AssertionError(f"nonconsecutive Z row {index} in {path}")
                candidates.append((int(fields[2]), int(fields[3])))
            elif fields[0] == "P":
                index = int(fields[1])
                if index != len(rows):
                    raise AssertionError(f"nonconsecutive P row {index} in {path}")
                rows.append((int(fields[2]), int(fields[3])))
    if not metadata:
        raise AssertionError(f"no META row in {path}")
    return metadata, candidates, rows


def parse_sd(stderr: str) -> list[tuple[int, str, int, int]]:
    """Extract initial sorted-anchor coordinates from --print-seeds stderr."""

    coordinates: list[tuple[int, str, int, int]] = []
    for line in stderr.splitlines():
        if not line.startswith("SD\t"):
            continue
        fields = line.split("\t")
        coordinates.append((int(fields[2]), fields[3], int(fields[4]), int(fields[5])))
    return coordinates


def parse_runl(stderr: str) -> list[dict[str, object]]:
    """Extract MM_RUNL JSON records."""

    records: list[dict[str, object]] = []
    for line in stderr.splitlines():
        if line.startswith("MM_RUNL\t"):
            records.append(json.loads(line.split("\t", 1)[1]))
    return records


def parse_intended_paf_records(
    stdout: str,
    expected_query_name: str,
    expected_target_name: str,
    expected_query_length: int,
    expected_target_length: int,
    required_query_start: int,
    required_query_end: int,
    required_target_start: int,
    required_target_end: int,
) -> tuple[list[dict[str, int | str]], list[dict[str, int | str]]]:
    """Parse all valid intended-pair PAF records and its qualifying subset.

    PAF intervals are zero-based and half-open.  The span requirement therefore
    uses ``start <= first_endpoint`` and ``end > last_endpoint`` on both query
    and target.  Other alignments, including the independent P-prefix alignment,
    are not evidence that the comb itself mapped and do not satisfy this gate.
    """

    intended: list[dict[str, int | str]] = []
    qualifying: list[dict[str, int | str]] = []
    for line_number, raw_line in enumerate(stdout.splitlines(), start=1):
        if not raw_line:
            continue
        fields = raw_line.split("\t")
        if fields[0] != expected_query_name:
            continue
        if len(fields) < 12:
            raise AssertionError(
                f"intended-query PAF line {line_number} has {len(fields)} fields; expected at least 12"
            )
        if fields[5] != expected_target_name:
            raise AssertionError(
                f"unexpected target for intended query on PAF line {line_number}: {fields[5]}"
            )
        try:
            query_length = int(fields[1])
            query_start = int(fields[2])
            query_end = int(fields[3])
            target_length = int(fields[6])
            target_start = int(fields[7])
            target_end = int(fields[8])
            residue_matches = int(fields[9])
            alignment_block_length = int(fields[10])
            mapping_quality = int(fields[11])
        except ValueError as error:
            raise AssertionError(f"noninteger core PAF field on line {line_number}") from error
        if query_length != expected_query_length or target_length != expected_target_length:
            raise AssertionError(
                f"unexpected sequence length on intended PAF line {line_number}: "
                f"query={query_length}, target={target_length}"
            )
        if fields[4] != "+":
            raise AssertionError(f"unexpected strand on intended PAF line {line_number}: {fields[4]}")
        if not (0 <= query_start < query_end <= query_length):
            raise AssertionError(f"invalid query interval on intended PAF line {line_number}")
        if not (0 <= target_start < target_end <= target_length):
            raise AssertionError(f"invalid target interval on intended PAF line {line_number}")
        if not (0 < residue_matches <= alignment_block_length):
            raise AssertionError(f"invalid match/block counts on intended PAF line {line_number}")
        if not (0 <= mapping_quality <= 255):
            raise AssertionError(f"invalid mapping quality on intended PAF line {line_number}")
        record: dict[str, int | str] = {
            "query_name": fields[0],
            "query_start": query_start,
            "query_end": query_end,
            "strand": fields[4],
            "target_name": fields[5],
            "target_start": target_start,
            "target_end": target_end,
            "residue_matches": residue_matches,
            "alignment_block_length": alignment_block_length,
            "mapping_quality": mapping_quality,
            "field_count": len(fields),
        }
        intended.append(record)
        if (
            query_start <= required_query_start
            and query_end > required_query_end
            and target_start <= required_target_start
            and target_end > required_target_end
        ):
            qualifying.append(record)
    return intended, qualifying


def parse_qualifying_paf(
    stdout: str,
    expected_query_name: str,
    expected_target_name: str,
    expected_query_length: int,
    expected_target_length: int,
    required_query_start: int,
    required_query_end: int,
    required_target_start: int,
    required_target_end: int,
) -> list[dict[str, int | str]]:
    """Compatibility wrapper returning only the branch-spanning subset."""

    _, qualifying = parse_intended_paf_records(
        stdout,
        expected_query_name,
        expected_target_name,
        expected_query_length,
        expected_target_length,
        required_query_start,
        required_query_end,
        required_target_start,
        required_target_end,
    )
    return qualifying


def validate_real_run(
    case_directory: Path,
    minimap2: Path,
    anchors: list[dict[str, object]],
    m: int,
    reference_length: int,
    query_length: int,
    reference_fasta_sha256: str,
    query_fasta_sha256: str,
    minimap2_description: dict[str, str],
    timeout_seconds: float,
) -> dict[str, object]:
    """Run standalone minimap2 and validate coordinates, f/p, Run_L, and PAF."""

    dump_directory = case_directory / "dumps"
    dump_directory.mkdir()
    command = [
        str(minimap2),
        "-x",
        "map-ont",
        "-k",
        str(K),
        "-w",
        str(W),
        "-t",
        "1",
        "--print-seeds",
        str(case_directory / "reference.fa"),
        str(case_directory / "query.fa"),
    ]
    environment = os.environ.copy()
    environment["MM_RUNL_EXPERIMENT"] = "1"
    environment["MM_RUNL_DUMP_DIR"] = str(dump_directory)
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            env=environment,
            check=False,
            timeout=timeout_seconds,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"minimap2 mapping exceeded {timeout_seconds:g} seconds for m={m}") from error
    (case_directory / "mapping.paf").write_text(completed.stdout, encoding="utf-8")
    (case_directory / "mapping.stderr").write_text(completed.stderr, encoding="utf-8")
    if completed.returncode != 0:
        raise RuntimeError(f"minimap2 failed with status {completed.returncode}; see {case_directory / 'mapping.stderr'}")
    first_comb_endpoint = next(
        int(anchor["reference"]) for anchor in anchors if anchor["label"] == f"r{m:03d}_02"
    )
    last_comb_endpoint = next(
        int(anchor["reference"]) for anchor in anchors if anchor["label"] == f"r{m:03d}_39"
    )
    first_comb_query_endpoint = next(
        int(anchor["query"]) for anchor in anchors if anchor["label"] == f"r{m:03d}_02"
    )
    last_comb_query_endpoint = next(
        int(anchor["query"]) for anchor in anchors if anchor["label"] == f"r{m:03d}_39"
    )
    intended_paf_records, paf_records = parse_intended_paf_records(
        completed.stdout,
        expected_query_name=f"comb_query_m{m}",
        expected_target_name=f"comb_reference_m{m}",
        expected_query_length=query_length,
        expected_target_length=reference_length,
        required_query_start=first_comb_query_endpoint,
        required_query_end=last_comb_query_endpoint,
        required_target_start=first_comb_endpoint,
        required_target_end=last_comb_endpoint,
    )
    if not paf_records:
        raise AssertionError(
            "minimap2 returned no valid intended-name PAF alignment spanning the local comb branch "
            "from r_m_02 through r_m_39 on both query and target"
        )

    expected_coordinates = [
        (int(anchor["reference"]), "+", int(anchor["query"]), K) for anchor in anchors
    ]
    observed_coordinates = parse_sd(completed.stderr)
    if observed_coordinates != expected_coordinates:
        mismatch = next(
            (
                index,
                expected_coordinates[index] if index < len(expected_coordinates) else None,
                observed_coordinates[index] if index < len(observed_coordinates) else None,
            )
            for index in range(max(len(expected_coordinates), len(observed_coordinates)))
            if index >= len(expected_coordinates)
            or index >= len(observed_coordinates)
            or expected_coordinates[index] != observed_coordinates[index]
        )
        raise AssertionError(
            f"SD coordinate mismatch at {mismatch[0]}: expected {mismatch[1]}, observed {mismatch[2]} "
            f"(expected {len(expected_coordinates)} anchors, observed {len(observed_coordinates)})"
        )

    dump_candidates = sorted(dump_directory.glob("runl_call_*_dp.tsv"))
    if not dump_candidates:
        raise AssertionError("no DP dump was produced")
    dump_path = min(dump_candidates, key=lambda path: int(path.name.split("_")[2]))
    metadata, candidates, rows = parse_dump(dump_path)
    if int(metadata["anchor_count"]) != len(anchors) or len(rows) != len(anchors):
        raise AssertionError(
            f"DP dump anchor count differs: META={metadata['anchor_count']}, rows={len(rows)}, expected={len(anchors)}"
        )
    expected_candidates = sorted(
        (int(anchor["score"]), index)
        for index, anchor in enumerate(anchors)
        if int(anchor["score"]) >= 40
    )
    if metadata["stage"] != "dp" or int(metadata["min_score"]) != 40:
        raise AssertionError(f"unexpected first DP dump metadata: {metadata}")
    if int(metadata["candidate_end_count"]) != len(expected_candidates):
        raise AssertionError(
            f"candidate count differs: META={metadata['candidate_end_count']}, "
            f"expected={len(expected_candidates)}"
        )
    if candidates != expected_candidates:
        mismatch_index = next(
            index
            for index in range(max(len(candidates), len(expected_candidates)))
            if index >= len(candidates)
            or index >= len(expected_candidates)
            or candidates[index] != expected_candidates[index]
        )
        raise AssertionError(
            f"sorted candidate mismatch at {mismatch_index}: "
            f"expected={expected_candidates[mismatch_index] if mismatch_index < len(expected_candidates) else None}, "
            f"observed={candidates[mismatch_index] if mismatch_index < len(candidates) else None}"
        )
    top_score = expected_candidates[-1][0]
    top_indices = [index for score, index in expected_candidates if score == top_score]
    if top_score != 615 or len(top_indices) != m or top_indices[-1] != len(anchors) - 1:
        raise AssertionError(
            f"top-score tie mismatch: score={top_score}, multiplicity={len(top_indices)}, "
            f"indices={top_indices}, expected selected={len(anchors) - 1}"
        )
    expected_rows = [(int(anchor["score"]), int(anchor["predecessor"])) for anchor in anchors]
    if rows != expected_rows:
        mismatch_index = next(index for index, pair in enumerate(zip(rows, expected_rows)) if pair[0] != pair[1])
        raise AssertionError(
            f"f/p mismatch at {mismatch_index} ({anchors[mismatch_index]['label']}): "
            f"expected {expected_rows[mismatch_index]}, observed {rows[mismatch_index]}"
        )

    first_call_id = int(metadata["call_id"])
    runl_records = [
        record
        for record in parse_runl(completed.stderr)
        if record.get("stage") == "dp" and int(record.get("call_id", -1)) == first_call_id
    ]
    if sorted(int(record["lanes"]) for record in runl_records) != list(RUN_L_WIDTHS):
        raise AssertionError(f"missing first-call Run_L widths: {runl_records}")
    for record in runl_records:
        lanes = int(record["lanes"])
        expected = expected_runl(m, lanes)
        for field, expected_value in expected.items():
            observed_value = int(record[field])
            if observed_value != expected_value:
                raise AssertionError(
                    f"Run_L mismatch for m={m}, L={lanes}, {field}: expected {expected_value}, observed {observed_value}"
                )
        if int(record["start_index"]) != len(anchors) - 1:
            raise AssertionError(
                f"unexpected ranked end for m={m}, L={lanes}: {record['start_index']} != {len(anchors) - 1}"
            )

    return {
        "command": command,
        "timeout_seconds": timeout_seconds,
        "returncode": completed.returncode,
        "reference_fasta_sha256": reference_fasta_sha256,
        "query_fasta_sha256": query_fasta_sha256,
        "minimap2_binary_sha256": minimap2_description["sha256"],
        "minimap2_version": minimap2_description["version"],
        "paf_record_count": len(intended_paf_records),
        "intended_paf_records": intended_paf_records,
        "qualifying_paf_record_count": len(paf_records),
        "qualifying_paf_records": paf_records,
        "required_target_span": {
            "first_anchor_label": f"r{m:03d}_02",
            "first_anchor_endpoint": first_comb_endpoint,
            "last_anchor_label": f"r{m:03d}_39",
            "last_anchor_endpoint": last_comb_endpoint,
        },
        "required_query_span": {
            "first_anchor_label": f"r{m:03d}_02",
            "first_anchor_endpoint": first_comb_query_endpoint,
            "last_anchor_label": f"r{m:03d}_39",
            "last_anchor_endpoint": last_comb_query_endpoint,
        },
        "dp_dump": str(dump_path),
        "dp_metadata": metadata,
        "top_score_tie": {
            "score": top_score,
            "multiplicity": len(top_indices),
            "anchor_indices": top_indices,
            "candidate_sort_order": "score_ascending_then_anchor_index_ascending",
            "selection_rule": "consume_candidates_from_end",
            "selected_anchor_index": top_indices[-1],
            "selected_anchor_label": f"r{m:03d}_39",
        },
        "run_l": runl_records,
        "status": "passed",
    }


def generate_case(
    output_root: Path,
    m: int,
    seed: int,
    minimap2: Path | None,
    minimap2_description: dict[str, str] | None,
    timeout_seconds: float,
) -> dict[str, object]:
    """Generate one m case and optionally run the strict real-minimap2 validation."""

    case_directory = output_root / f"m_{m:04d}"
    if case_directory.exists():
        raise RuntimeError(
            f"refusing to reuse existing case directory (choose a fresh --output-root): {case_directory}"
        )
    case_directory.mkdir()
    anchors = build_anchor_geometry(m)
    rng = random.Random(seed + m * 1_000_003)
    used_kmer_hashes: set[int] = set()
    total_attempts = 0

    reference_length = max(int(anchor["reference"]) for anchor in anchors) + (ISLAND_LENGTH - DESIRED_END_OFFSET) + W + 1
    query_length = max(int(anchor["query"]) for anchor in anchors) + (ISLAND_LENGTH - DESIRED_END_OFFSET) + W + 1
    reference_sequence = ["N"] * reference_length
    query_sequence = ["N"] * query_length

    for anchor in anchors:
        island, minimizer_hash, minimizer_strand, attempts = find_island(rng, used_kmer_hashes)
        total_attempts += attempts
        reference_start = int(anchor["reference"]) - DESIRED_END_OFFSET
        query_start = int(anchor["query"]) - DESIRED_END_OFFSET
        place_island(reference_sequence, reference_start, island, str(anchor["label"]))
        place_island(query_sequence, query_start, island, str(anchor["label"]))
        anchor["island"] = island
        anchor["minimizer_hash"] = minimizer_hash
        anchor["minimizer_strand"] = minimizer_strand
        anchor["generation_attempts"] = attempts

    reference = "".join(reference_sequence)
    query = "".join(query_sequence)
    reference_minimizers = sketch_minimizers(reference)
    query_minimizers = sketch_minimizers(query)
    expected_reference = [
        {
            "hash": int(anchor["minimizer_hash"]),
            "span": K,
            "position": int(anchor["reference"]),
            "strand": int(anchor["minimizer_strand"]),
        }
        for anchor in anchors
    ]
    expected_query = [
        {
            "hash": int(anchor["minimizer_hash"]),
            "span": K,
            "position": int(anchor["query"]),
            "strand": int(anchor["minimizer_strand"]),
        }
        for anchor in anchors
    ]
    if reference_minimizers != expected_reference:
        raise AssertionError("internal mm_sketch port did not reproduce the intended reference minimizers")
    if query_minimizers != expected_query:
        raise AssertionError("internal mm_sketch port did not reproduce the intended query minimizers")

    reference_path = case_directory / "reference.fa"
    query_path = case_directory / "query.fa"
    write_fasta(reference_path, f"comb_reference_m{m}", reference)
    write_fasta(query_path, f"comb_query_m{m}", query)
    reference_fasta_sha256 = sha256_file(reference_path)
    query_fasta_sha256 = sha256_file(query_path)
    expected_path_length, expected_unit_runs = expected_path_profile(m)
    top_score = max(int(anchor["score"]) for anchor in anchors)
    top_score_indices = [
        index for index, anchor in enumerate(anchors) if int(anchor["score"]) == top_score
    ]
    if top_score != 615 or len(top_score_indices) != m or top_score_indices[-1] != len(anchors) - 1:
        raise AssertionError(
            f"unexpected top-score tie for m={m}: score={top_score}, indices={top_score_indices}"
        )
    manifest: dict[str, object] = {
        "schema_version": 1,
        "construction": "repaired_P_G_C_R_Run_L_witness",
        "control_role": "unique_minimizer_coordinate_island_adversarial_control",
        "interpretation": "exact anchor-geometry control; not repeat-frequency evidence",
        "case_reuse_policy": "refuse_existing_case_directory",
        "m": m,
        "seed": seed,
        "k": K,
        "w": W,
        "reference_length": len(reference),
        "query_length": len(query),
        "anchor_count": len(anchors),
        "total_island_search_attempts": total_attempts,
        "penalties": {str(width): ordinary_penalty(width) for width in (100, 275, 450, 500, 550)},
        "penalty_model": (
            "non-contracted binary32 prediction; the real minimap2 C f/p dump is ground truth"
        ),
        "reference_fasta_sha256": reference_fasta_sha256,
        "query_fasta_sha256": query_fasta_sha256,
        "fasta_artifacts": {
            "reference": {"path": "reference.fa", "sha256": reference_fasta_sha256},
            "query": {"path": "query.fa", "sha256": query_fasta_sha256},
        },
        "minimap2_binary": (
            minimap2_description
            if minimap2_description is not None
            else {"path": None, "sha256": None, "version": None, "status": "not_used"}
        ),
        "run_l_widths": list(RUN_L_WIDTHS),
        "expected_first_path": {
            "path_length": expected_path_length,
            "maximal_unit_predecessor_run_lengths": list(expected_unit_runs),
            "top_score": top_score,
            "top_score_multiplicity": len(top_score_indices),
            "top_score_is_unique": len(top_score_indices) == 1,
            "top_score_anchor_indices": top_score_indices,
            "top_score_anchor_labels": [str(anchors[index]["label"]) for index in top_score_indices],
            "candidate_sort_order": "score_ascending_then_anchor_index_ascending",
            "selection_rule": "consume_candidates_from_end",
            "selected_start_index": top_score_indices[-1],
            "selected_start_label": f"r{m:03d}_39",
        },
        "expected_run_l": {str(lanes): expected_runl(m, lanes) for lanes in RUN_L_WIDTHS},
        "anchors": anchors,
        "internal_sketch_validation": "passed",
    }
    if minimap2 is not None:
        if minimap2_description is None:
            raise AssertionError("mapping requested without minimap2 identity metadata")
        manifest["real_minimap2_validation"] = validate_real_run(
            case_directory,
            minimap2,
            anchors,
            m,
            reference_length=len(reference),
            query_length=len(query),
            reference_fasta_sha256=reference_fasta_sha256,
            query_fasta_sha256=query_fasta_sha256,
            minimap2_description=minimap2_description,
            timeout_seconds=timeout_seconds,
        )
    else:
        manifest["real_minimap2_validation"] = "not_requested"
    (case_directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def parse_m_values(values: Iterable[str] | None) -> list[int]:
    """Parse repeated/comma-separated --m values."""

    if values is None:
        values = ["4,20"]
    parsed: list[int] = []
    for value in values:
        for part in value.split(","):
            if part:
                parsed.append(int(part))
    if not parsed or any(value < 1 for value in parsed):
        raise argparse.ArgumentTypeError("--m values must be positive")
    return parsed


def resolve_output_root(requested: Path) -> Path:
    """Resolve an output root and require a strict child of this source tree."""

    resolved = requested.expanduser().resolve()
    try:
        relative = resolved.relative_to(SCRIPT_DIRECTORY)
    except ValueError as error:
        raise ValueError(f"--output-root must be under {SCRIPT_DIRECTORY}: {resolved}") from error
    if relative == Path("."):
        raise ValueError(f"--output-root must be a child directory under {SCRIPT_DIRECTORY}")
    return resolved


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--m", action="append", help="m values, repeated or comma-separated (default: 4,20)")
    parser.add_argument("--seed", type=int, default=2_026_090_2)
    parser.add_argument(
        "--minimap2",
        type=Path,
        default=Path(__file__).resolve().with_name("minimap2"),
        help="instrumented standalone minimap2; use --generate-only to skip execution",
    )
    parser.add_argument(
        "--mapping-timeout-seconds",
        type=float,
        default=DEFAULT_MAPPING_TIMEOUT_SECONDS,
        help=f"per-mapping timeout (default: {DEFAULT_MAPPING_TIMEOUT_SECONDS:g} seconds)",
    )
    parser.add_argument("--generate-only", action="store_true")
    arguments = parser.parse_args()
    run_expected_runl_regression_tests()
    m_values = parse_m_values(arguments.m)
    try:
        output_root = resolve_output_root(arguments.output_root)
    except ValueError as error:
        parser.error(str(error))
    if not math.isfinite(arguments.mapping_timeout_seconds) or arguments.mapping_timeout_seconds <= 0:
        parser.error("--mapping-timeout-seconds must be finite and positive")
    minimap2 = None if arguments.generate_only else arguments.minimap2.resolve()
    minimap2_description: dict[str, str] | None = None
    if minimap2 is not None:
        try:
            minimap2_description = describe_minimap2(minimap2, arguments.mapping_timeout_seconds)
        except (RuntimeError, ValueError) as error:
            parser.error(str(error))

    output_root.mkdir(parents=True, exist_ok=True)
    summaries: list[dict[str, object]] = []
    for m in m_values:
        manifest = generate_case(
            output_root,
            m,
            arguments.seed,
            minimap2,
            minimap2_description,
            arguments.mapping_timeout_seconds,
        )
        validation = manifest["real_minimap2_validation"]
        summaries.append(
            {
                "m": m,
                "anchor_count": manifest["anchor_count"],
                "reference_length": manifest["reference_length"],
                "query_length": manifest["query_length"],
                "validation": validation if isinstance(validation, str) else validation["status"],
            }
        )
    print(json.dumps({"output_root": str(output_root), "cases": summaries}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, RuntimeError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
