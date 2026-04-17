#!/usr/bin/env python3
"""build_with_profiling.py — run ./build.sh N times and aggregate profiles.

Used to answer "did my last change actually speed up the pipeline, or did I
catch an outlier?" Each run rebuilds (touching the relevant .cppm is cheap —
most work is ninja no-op) and re-executes the aligner, dumping
`build/bioseq.prof`. The script converts to JSON via the vendored
profiler_converter, aggregates per-block timings across runs, and prints
tab-separated tables with mean / median / P99 and per-run values so the
owning AI agent (or a human) can eyeball regressions.

Usage (from repo root):
    python3 build_with_profiling.py              # 5 runs, default
    python3 build_with_profiling.py -n 10        # 10 runs
    python3 build_with_profiling.py --no-build   # skip build; re-run binary only
    python3 build_with_profiling.py --out stats.tsv

Runs are serial on purpose: parallel rebuilds fight over the OS scheduler
and bias timings. See RESEARCH/CODE_GUIDELINES.md §11 for the block-name
contract — every block you read here is declared by name in the source.
"""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
BUILD_DIR = REPO_ROOT / "build"
PROF_BIN = BUILD_DIR / "bioseq.prof"
PROF_JSON = BUILD_DIR / "prof.json"
PROF_CONVERTER = BUILD_DIR / "vcpkg_installed" / "arm64-osx" / "tools" / "easy-profiler" / "profiler_converter"
BUILD_SCRIPT = REPO_ROOT / "build.sh"
ALIGNER_BIN = BUILD_DIR / "bioseq" / "bioseq_aligner"


@dataclass
class BlockStats:
    """Accumulates per-name totals across runs.

    Each entry in `total_ms_per_run[i]` is the sum of every instance of
    the named block during run i (self_ms is the same idea for self-time).
    """
    name: str
    total_ms_per_run: list[float] = field(default_factory=list)
    self_ms_per_run: list[float] = field(default_factory=list)


def run_cmd(argv: list[str], *, cwd: Path | None = None) -> None:
    """Run a command, streaming output; abort on non-zero exit."""
    result = subprocess.run(argv, cwd=cwd)
    if result.returncode != 0:
        sys.exit(f"[build_with_profiling] command failed: {' '.join(argv)}")


def do_build() -> None:
    """Run ./build.sh. The script already runs the aligner, so the
    .prof file lands in build/ by the time we return."""
    if not BUILD_SCRIPT.exists():
        sys.exit(f"[build_with_profiling] missing {BUILD_SCRIPT}")
    print("[build_with_profiling] running build.sh ...")
    run_cmd(["bash", str(BUILD_SCRIPT)], cwd=REPO_ROOT)


def do_binary_only() -> None:
    """Re-run the aligner without rebuilding. Lower-overhead mode — used
    when --no-build is set or when the build has already produced the
    aligner for this invocation."""
    if not ALIGNER_BIN.exists():
        sys.exit(f"[build_with_profiling] missing aligner {ALIGNER_BIN}")
    target = REPO_ROOT / "data" / "target.fasta"
    query = REPO_ROOT / "data" / "query.fasta"
    if not target.exists() or not query.exists():
        sys.exit("[build_with_profiling] missing data/target.fasta or data/query.fasta")
    print("[build_with_profiling] running aligner only ...")
    run_cmd([str(ALIGNER_BIN), "-c", str(target), str(query)], cwd=BUILD_DIR)


def convert_prof() -> dict:
    """Convert bioseq.prof -> prof.json, return parsed JSON."""
    if not PROF_BIN.exists():
        sys.exit(f"[build_with_profiling] missing {PROF_BIN} — build produced no profile")
    if not PROF_CONVERTER.exists():
        sys.exit(f"[build_with_profiling] missing {PROF_CONVERTER}")
    run_cmd([str(PROF_CONVERTER), str(PROF_BIN), str(PROF_JSON)])
    with PROF_JSON.open("r") as handle:
        return json.load(handle)


def accumulate_run(data: dict, store: dict[str, BlockStats], run_index: int) -> None:
    """Walk all blocks across all threads, summing per-name totals for
    this run. Every block instance with the same name gets its duration
    added to the same bucket — so blocks that fire multiple times per
    pipeline (e.g. mm_sketch once per sequence) are aggregated.

    Invariant: at entry, every bucket in `store` has exactly `run_index`
    entries (0 before the first run). At exit, every bucket (including
    new ones discovered this run and old ones not seen this run) has
    exactly `run_index + 1` entries.
    """

    def ensure_bucket(name: str) -> BlockStats:
        bucket = store.get(name)
        if bucket is None:
            bucket = BlockStats(name=name)
            # Back-fill with zeros so index math stays rectangular
            bucket.total_ms_per_run = [0.0] * run_index
            bucket.self_ms_per_run  = [0.0] * run_index
            store[name] = bucket
        # Append this run's slot (starts at zero; we add into it).
        if len(bucket.total_ms_per_run) == run_index:
            bucket.total_ms_per_run.append(0.0)
            bucket.self_ms_per_run.append(0.0)
        return bucket

    def walk(node: dict) -> None:
        name = node.get("name", "?")
        start_ns = node.get("start", 0)
        stop_ns  = node.get("stop", start_ns)
        # Clamp negatives: clock skew or nested-time artifacts could
        # theoretically produce negative deltas. We'd rather report zero
        # than contaminate the average.
        duration_ns = max(0, stop_ns - start_ns)
        kids = node.get("children", [])
        kids_sum = sum(max(0, c.get("stop", 0) - c.get("start", 0)) for c in kids)
        self_ns = max(0, duration_ns - kids_sum)

        bucket = ensure_bucket(name)
        bucket.total_ms_per_run[-1] += duration_ns / 1e6
        bucket.self_ms_per_run[-1]  += self_ns / 1e6

        for child in kids:
            walk(child)

    for thread in data.get("threads", []):
        for child in thread.get("children", []):
            walk(child)

    # Back-fill any bucket that didn't see this run with a zero entry.
    for bucket in store.values():
        if len(bucket.total_ms_per_run) == run_index:
            bucket.total_ms_per_run.append(0.0)
            bucket.self_ms_per_run.append(0.0)


def percentile(sorted_values: list[float], q: float) -> float:
    """Linear-interpolated percentile q in [0, 100] from a sorted list.

    Note: with small N (say <10 runs) "P99" is essentially the max —
    the label is still useful because regressions typically show up in
    the tails before they show up in the mean.
    """
    if not sorted_values:
        return 0.0
    if len(sorted_values) == 1:
        return sorted_values[0]
    fractional_index = (q / 100.0) * (len(sorted_values) - 1)
    lower_index = int(fractional_index)
    upper_index = min(lower_index + 1, len(sorted_values) - 1)
    weight = fractional_index - lower_index
    return sorted_values[lower_index] * (1 - weight) + sorted_values[upper_index] * weight


def compute_summary(values: list[float]) -> dict[str, float]:
    """Return mean, median, stdev, P50, P99, min, max of a list of ms values."""
    if not values:
        return {"mean": 0.0, "median": 0.0, "stdev": 0.0,
                "p50": 0.0, "p99": 0.0, "min": 0.0, "max": 0.0}
    sorted_values = sorted(values)
    return {
        "mean":   statistics.mean(values),
        "median": statistics.median(values),
        "stdev":  statistics.pstdev(values) if len(values) > 1 else 0.0,
        "p50":    percentile(sorted_values, 50),
        "p99":    percentile(sorted_values, 99),
        "min":    min(values),
        "max":    max(values),
    }


# Blocks that matter most to the memory-allocator work: surface these at
# the top of the summary so the AI agent reading this output doesn't need
# to scroll through 40 lines to find the headline number.
PRIORITY_BLOCKS = [
    "minimap_port",
    "minimap2_port",
    "mm_map_frag_core",
    "mm_align_skeleton",
    "mg_lchain_dp",
    "mg_lchain_rmq",
    "mg_lchain_simd",
    "backtrack_chains",
    "collect_seed_hits",
    "target_index_build",
    "target_index_lookup",
    "mm_sketch",
    "base_collect_minimizers",
    "mm_gen_regs",
    "mm_set_parent",
    "mm_select_sub",
    "mm_est_err",
]


def write_tsv(handle, store: dict[str, BlockStats], num_runs: int) -> None:
    """Emit several tab-separated tables.

    1) Priority summary: the blocks that the allocator work targets, in a
       fixed order. Fastest table for an AI agent to diff against a prior
       run.
    2) Full summary: same stats but for every block name we saw.
    3) Per-run raw totals: one row per block, one column per run.
    """
    def emit_table(title: str, headers: list[str], rows: list[list[str]]):
        print(title, file=handle)
        print("\t".join(headers), file=handle)
        for row in rows:
            print("\t".join(row), file=handle)
        print(file=handle)

    def summary_row(name: str, kind: str, values: list[float]) -> list[str]:
        stats = compute_summary(values)
        return [
            name,
            kind,
            f"{stats['mean']:.3f}",
            f"{stats['p50']:.3f}",
            f"{stats['p99']:.3f}",
            f"{stats['min']:.3f}",
            f"{stats['max']:.3f}",
            f"{stats['stdev']:.3f}",
        ]

    header_summary = ["block", "metric", "mean_ms", "p50_ms", "p99_ms",
                      "min_ms", "max_ms", "stdev_ms"]

    # --- Priority summary -------------------------------------------------
    priority_rows: list[list[str]] = []
    for name in PRIORITY_BLOCKS:
        bucket = store.get(name)
        if bucket is None:
            continue
        priority_rows.append(summary_row(name, "total_ms", bucket.total_ms_per_run))
        priority_rows.append(summary_row(name, "self_ms",  bucket.self_ms_per_run))
    emit_table("# Priority summary (allocator-relevant blocks)",
               header_summary, priority_rows)

    # --- Full summary -----------------------------------------------------
    full_rows: list[list[str]] = []
    for name, bucket in sorted(store.items(),
                               key=lambda kv: -statistics.mean(kv[1].total_ms_per_run)
                                              if kv[1].total_ms_per_run else 0):
        full_rows.append(summary_row(name, "total_ms", bucket.total_ms_per_run))
        full_rows.append(summary_row(name, "self_ms",  bucket.self_ms_per_run))
    emit_table("# Full per-block summary (sorted by mean total_ms)",
               header_summary, full_rows)

    # --- Per-run matrix ---------------------------------------------------
    run_headers = ["block", "metric"] + [f"run_{i+1}_ms" for i in range(num_runs)]
    matrix_rows: list[list[str]] = []
    for name, bucket in sorted(store.items()):
        matrix_rows.append([name, "total_ms",
                           *(f"{v:.3f}" for v in bucket.total_ms_per_run)])
        matrix_rows.append([name, "self_ms",
                           *(f"{v:.3f}" for v in bucket.self_ms_per_run)])
    emit_table("# Per-run matrix (totals across runs)", run_headers, matrix_rows)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=("Run the bioseq aligner N times, aggregate easy_profiler "
                     "timings into TSV tables."),
        epilog=("Runs are serial so measurements don't bias each other. "
                "Minimize contention: close other apps, prefer AC power."))
    parser.add_argument("-n", "--runs", type=int, default=5,
                        help="number of runs (default: 5)")
    parser.add_argument("--no-build", action="store_true",
                        help="skip ./build.sh; rerun the aligner binary only")
    parser.add_argument("--out", type=str, default=None,
                        help="write TSV tables to this file (default: stdout)")
    args = parser.parse_args()

    if args.runs < 1:
        sys.exit("[build_with_profiling] --runs must be >= 1")

    # On the first run, always do a full build (ensures the binary is
    # up-to-date with the current source). On later runs we can skip
    # rebuild — ninja would no-op anyway, but skipping it trims a few
    # hundred milliseconds of CMake setup per iteration.
    need_build_this_time = not args.no_build
    store: dict[str, BlockStats] = {}

    for run_index in range(args.runs):
        print(f"\n[build_with_profiling] === run {run_index + 1}/{args.runs} ===")
        t0 = time.monotonic()
        if need_build_this_time:
            do_build()
        else:
            do_binary_only()
        elapsed = time.monotonic() - t0
        print(f"[build_with_profiling] run {run_index + 1} wall time: {elapsed:.2f}s")

        data = convert_prof()
        accumulate_run(data, store, run_index)

        # After the first build, later runs can skip the CMake step.
        need_build_this_time = False

    # Determine output handle
    out = open(args.out, "w") if args.out else sys.stdout
    try:
        write_tsv(out, store, args.runs)
    finally:
        if args.out:
            out.close()
            print(f"[build_with_profiling] wrote TSV to {args.out}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
