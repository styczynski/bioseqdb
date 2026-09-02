#!/usr/bin/env python3
"""Run and summarize the standalone minimap2 Run_L experiment."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
import platform
import shlex
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import yaml


ROOT = Path(__file__).resolve().parent
OCCURRENCE_COUNTS = [
    1, 2, 3, 4, 5, 10, 20, 30, 40, 50, 100, 200, 300, 400, 500,
    1000, 2000, 3000, 4000, 5000,
]
K_VALUES = list(range(20, 7, -1))
CALIBRATION_COUNTS = [1, 2, 5, 10]
LANE_WIDTHS = [4, 8, 16]
PILOT_RUN_IDS = {
    "default_N1_k15",
    "default_N10_k15",
    "default_N20_k15",
    "default_N100_k15",
    "default_N5000_k15",
    "default_N100_k8",
    "unfiltered_N100_k15",
    "unfiltered_N5000_k15",
    "unfiltered_N100_k8",
    "fast_long_default_N1",
    "hifi_calibration_N1",
}
DUMP_RUN_IDS = {
    "default_N10_k15",
    "unfiltered_N100_k15",
    "hifi_calibration_N1",
}
RAW_COUNTERS = [
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
]


@dataclass(frozen=True)
class RunSpec:
    run_id: str
    arm: str
    dataset_arm: str
    occurrence_count: int
    k: int
    extra_args: tuple[str, ...]
    series_memberships: tuple[tuple[str, int], ...]


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def command_first_line(argv: list[str]) -> str:
    result = subprocess.run(argv, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return result.stdout.splitlines()[0]


def cpu_model() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return "unknown"


def memory_snapshot() -> dict[str, int]:
    values: dict[str, int] = {}
    for line in Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
        key, raw_value = line.split(":", 1)
        if key in {"MemTotal", "MemAvailable", "SwapTotal", "SwapFree"}:
            values[f"{key.lower()}_kib"] = int(raw_value.strip().split()[0])
    values["swap_used_kib"] = values["swaptotal_kib"] - values["swapfree_kib"]
    return values


def monitor_resources(stop: threading.Event, samples: list[dict[str, int]]) -> None:
    while not stop.is_set():
        samples.append(memory_snapshot())
        stop.wait(0.5)


def summarize_resource_samples(
    samples: list[dict[str, int]], *, jobs: int, started_at: str, completed_at: str
) -> dict[str, Any]:
    return {
        "started_at_utc": started_at,
        "completed_at_utc": completed_at,
        "parallel_processes": jobs,
        "sample_interval_seconds": 0.5,
        "sample_count": len(samples),
        "mem_total_kib": samples[0]["memtotal_kib"],
        "minimum_mem_available_kib": min(sample["memavailable_kib"] for sample in samples),
        "swap_total_kib": samples[0]["swaptotal_kib"],
        "starting_swap_used_kib": samples[0]["swap_used_kib"],
        "maximum_swap_used_kib": max(sample["swap_used_kib"] for sample in samples),
        "ending_swap_used_kib": samples[-1]["swap_used_kib"],
    }


def rate(numerator: int, denominator: int) -> dict[str, int | float | None]:
    if denominator == 0:
        return {"numerator": numerator, "denominator": denominator, "fraction": None, "percent": None}
    fraction = numerator / denominator
    return {
        "numerator": numerator,
        "denominator": denominator,
        "fraction": fraction,
        "percent": 100.0 * fraction,
    }


def validate_record(record: dict[str, Any]) -> list[str]:
    lanes = int(record["lanes"])
    errors: list[str] = []
    if record["loop_evaluations"] != record["fast_batches"] + record["scalar_fallbacks"]:
        errors.append("loop_evaluations != fast_batches + scalar_fallbacks")
    if record["bounds_pass"] != record["fast_batches"] + record["contiguous_mismatch"]:
        errors.append("bounds_pass != fast_batches + contiguous_mismatch")
    if record["loop_evaluations"] != (
        record["bounds_pass"]
        + record["bounds_fail_current"]
        + record["bounds_fail_capacity"]
        + record["bounds_fail_both"]
    ):
        errors.append("loop_evaluations != bounds outcomes")
    if record["primary_path_anchors"] != lanes * record["fast_batches"] + record["scalar_fallbacks"]:
        errors.append("primary_path_anchors != lanes*fast_batches + scalar_fallbacks")
    if record["fast_covered_anchors"] != lanes * record["fast_batches"]:
        errors.append("fast_covered_anchors != lanes*fast_batches")
    if record["primary_path_anchors"] != record["scalar_path_length"]:
        errors.append("primary_path_anchors != independent scalar_path_length")
    if record["scalar_validation_error"] != 0:
        errors.append("independent scalar predecessor validation failed")
    if record["emulation_error"] != 0:
        errors.append("Run_L emulation encountered an invalid predecessor")
    if record["bounds_fail_capacity"] != 0:
        errors.append("capacity bound failed while current-index bound passed")
    return errors


def decorate_record(record: dict[str, Any]) -> dict[str, Any]:
    output = dict(record)
    output["rates"] = {
        "predicate_true_per_loop_evaluation": rate(record["fast_batches"], record["loop_evaluations"]),
        "predicate_true_per_bounds_passing_evaluation": rate(record["fast_batches"], record["bounds_pass"]),
        "fast_path_anchor_coverage": rate(record["fast_covered_anchors"], record["primary_path_anchors"]),
    }
    output["scalar_iterations_avoided"] = (record["lanes"] - 1) * record["fast_batches"]
    output["validation_errors"] = validate_record(record)
    return output


def aggregate_records(records: list[dict[str, Any]]) -> dict[str, Any]:
    by_stage: dict[str, dict[str, Any]] = {}
    for stage in sorted({str(record["stage"]) for record in records}):
        stage_records = [record for record in records if record["stage"] == stage]
        lane_summaries: dict[str, Any] = {}
        for lanes in LANE_WIDTHS:
            selected = [record for record in stage_records if record["lanes"] == lanes]
            totals = {counter: sum(int(record[counter]) for record in selected) for counter in RAW_COUNTERS}
            totals["backtrack_calls"] = len(selected)
            totals["candidate_end_count_total"] = sum(int(record["candidate_end_count"]) for record in selected)
            totals["anchor_count_total"] = sum(int(record["anchor_count"]) for record in selected)
            validation_errors = [
                f"call {record['call_id']}: {error}"
                for record in selected
                for error in validate_record(record)
            ]
            lane_summaries[str(lanes)] = {
                "raw": totals,
                "rates": {
                    "predicate_true_per_loop_evaluation": rate(totals["fast_batches"], totals["loop_evaluations"]),
                    "predicate_true_per_bounds_passing_evaluation": rate(totals["fast_batches"], totals["bounds_pass"]),
                    "fast_path_anchor_coverage": rate(totals["fast_covered_anchors"], totals["primary_path_anchors"]),
                    "calls_with_any_fast_batch": rate(totals["has_fast_batch"], totals["has_ranked_end"]),
                },
                "scalar_iterations_avoided": (lanes - 1) * totals["fast_batches"],
                "validation_errors": validation_errors,
            }
        by_stage[stage] = {
            "call_ids": sorted({int(record["call_id"]) for record in stage_records}),
            "lanes": lane_summaries,
        }

    initial_dp: dict[str, Any] | None = None
    dp_records = [record for record in records if record["stage"] == "dp"]
    if dp_records:
        first_call = min(int(record["call_id"]) for record in dp_records)
        initial_dp = {
            str(lanes): decorate_record(
                next(record for record in dp_records if record["call_id"] == first_call and record["lanes"] == lanes)
            )
            for lanes in LANE_WIDTHS
        }

    all_errors = [
        f"{record['stage']} call {record['call_id']} L={record['lanes']}: {error}"
        for record in records
        for error in validate_record(record)
    ]
    return {
        "initial_dp_by_lane": initial_dp,
        "by_stage": by_stage,
        "all_validation_errors": all_errors,
    }


def parse_stderr(stderr: str) -> tuple[list[dict[str, Any]], list[dict[str, Any]], dict[str, Any] | None]:
    map_records: list[dict[str, Any]] = []
    runl_records: list[dict[str, Any]] = []
    timing: dict[str, Any] | None = None
    for line in stderr.splitlines():
        if line.startswith("MM_RUNL_MAP\t"):
            map_records.append(json.loads(line.split("\t", 1)[1]))
        elif line.startswith("MM_RUNL\t"):
            runl_records.append(json.loads(line.split("\t", 1)[1]))
        elif line.startswith("MM_TIME\t"):
            timing = {}
            for field in line.split("\t")[1:]:
                key, value = field.split("=", 1)
                timing[key] = float(value) if "." in value else int(value)
    return map_records, runl_records, timing


def build_specs() -> list[RunSpec]:
    specs: dict[str, RunSpec] = {}
    for arm, prefix, extra_args in [
        ("requested_default", "default", ()),
        ("repeat_filter_disabled_sensitivity", "unfiltered", ("-f", "1000000", "--q-occ-frac", "0")),
    ]:
        for count in OCCURRENCE_COUNTS:
            run_id = f"{prefix}_N{count}_k15"
            memberships: list[tuple[str, int]] = [("occurrence_count", count)]
            if count == 100:
                memberships.append(("k_at_N100", 15))
            specs[run_id] = RunSpec(
                run_id, arm, "primary", count, 15, extra_args, tuple(memberships)
            )
        for k in K_VALUES:
            run_id = f"{prefix}_N100_k{k}"
            if run_id in specs:
                continue
            specs[run_id] = RunSpec(
                run_id,
                arm,
                "primary",
                100,
                k,
                extra_args + ("-k", str(k)),
                (("k_at_N100", k),),
            )
    for arm, prefix, k, extra_args in [
        ("default_fast_long_calibration", "fast_long_default", 15, ()),
        ("map_hifi_fast_long_calibration", "hifi_calibration", 19, ("-x", "map-hifi")),
    ]:
        for count in CALIBRATION_COUNTS:
            run_id = f"{prefix}_N{count}"
            specs[run_id] = RunSpec(
                run_id,
                arm,
                "hifi_calibration",
                count,
                k,
                extra_args,
                ((f"{prefix}_occurrence_count", count),),
            )
    return list(specs.values())


def initialize_results(manifest: dict[str, Any], minimap2: Path) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "experiment": {
            "title": "Minimap2 predecessor arrays evaluated with the BioSeqDB first-path Run_L predicate",
            "status": "running",
            "started_at_utc": utc_now(),
            "completed_at_utc": None,
            "predicate_latex": (
                r"\operatorname{Run}_L(j,c)\iff j\geq L-1\land c\leq n-L\land "
                r"\bigwedge_{u=0}^{L-1}p_{j-u}=j-u-1"
            ),
            "primary_endpoint": "argmax over eligible anchors of (score, anchor_array_index)",
            "candidate_order": "ascending (score, anchor_array_index), consumed from the end",
            "primary_lane_width": 4,
            "measured_lane_widths": LANE_WIDTHS,
            "requested_grids": {
                "occurrence_count_at_default_k": OCCURRENCE_COUNTS,
                "k_at_N_100": K_VALUES,
            },
            "unique_requested_runs_per_arm": 32,
            "scope_adjustments": [],
            "interpretation_limit": (
                "This is a shadow evaluation of BioSeqDB Run_L on minimap2-produced score and predecessor arrays. "
                "It does not measure net SIMD speedup or reproduce every BioSeqDB anchor-ordering detail."
            ),
        },
        "provenance": {
            "minimap2_version": command_first_line([str(minimap2), "--version"]),
            "minimap2_binary_sha256": sha256(minimap2),
            "instrumented_lchain_sha256": sha256(ROOT / "lchain.c"),
            "instrumented_map_sha256": sha256(ROOT / "map.c"),
            "generator_sha256": sha256(ROOT / "runl_generate_data.py"),
            "runner_sha256": sha256(Path(__file__)),
            "build_argv": ["make", "-j4"],
            "compiler": command_first_line(["cc", "--version"]),
            "host": {
                "platform": platform.platform(),
                "machine": platform.machine(),
                "cpu_model": cpu_model(),
                "logical_cpu_count": os.cpu_count(),
            },
            "mapping_threads": 1,
            "instrumentation_environment": "MM_RUNL_EXPERIMENT=1",
        },
        "workloads": manifest,
        "arms": {
            "requested_default": {
                "description": "Literal minimap2 defaults, except one mapping thread and requested k overrides",
                "extra_arguments": [],
            },
            "repeat_filter_disabled_sensitivity": {
                "description": "Structural sensitivity control; not part of the requested default result",
                "extra_arguments": ["-f", "1000000", "--q-occ-frac", "0"],
            },
            "default_fast_long_calibration": {
                "description": "Separate default-parameter calibration using the synthetic 500 kbp FAST_LONG shape",
                "extra_arguments": [],
            },
            "map_hifi_fast_long_calibration": {
                "description": "Separate calibration for the HiFi-specific source comment using a synthetic 500 kbp FAST_LONG-compatible shape",
                "extra_arguments": ["-x", "map-hifi"],
            },
        },
        "runs": [],
        "validations": {
            "all_instrumentation_invariants_pass": None,
            "independent_dump_reconstruction": [],
            "review_findings": [],
        },
        "resource_monitoring": [],
    }


def save_results(path: Path, results: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        yaml.safe_dump(results, sort_keys=False, width=120, allow_unicode=False),
        encoding="utf-8",
    )
    temporary.replace(path)


def run_one(spec: RunSpec, manifest: dict[str, Any], raw_dir: Path, minimap2: Path) -> dict[str, Any]:
    workload = manifest[spec.dataset_arm]
    reference_entry = workload["references"][str(spec.occurrence_count)]
    reference = ROOT / reference_entry["path"]
    query = ROOT / workload["query_path"]
    argv = [str(minimap2), "-t", "1", *spec.extra_args, str(reference), str(query)]
    time_format = "MM_TIME\twall_seconds=%e\tuser_seconds=%U\tsystem_seconds=%S\tmax_rss_kib=%M"
    timed_argv = ["/usr/bin/time", "-f", time_format, *argv]
    environment = os.environ.copy()
    environment["MM_RUNL_EXPERIMENT"] = "1"
    dump_dir: Path | None = None
    if spec.run_id in DUMP_RUN_IDS:
        dump_dir = raw_dir / "dumps" / spec.run_id
        dump_dir.mkdir(parents=True, exist_ok=True)
        environment["MM_RUNL_DUMP_DIR"] = str(dump_dir)

    started = utc_now()
    monotonic_start = time.monotonic()
    process = subprocess.run(
        timed_argv,
        cwd=ROOT,
        env=environment,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        timeout=3600,
        check=False,
    )
    observed_wall = time.monotonic() - monotonic_start
    completed = utc_now()
    stderr_path = raw_dir / f"{spec.run_id}.stderr.log"
    stderr_path.write_text(process.stderr, encoding="utf-8")
    map_records, runl_records, timing = parse_stderr(process.stderr)
    summaries = aggregate_records(runl_records)
    status = "completed" if process.returncode == 0 else "failed"
    if process.returncode == 0 and not runl_records:
        status = "completed_without_backtrack_call"

    return {
        "run_id": spec.run_id,
        "status": status,
        "arm": spec.arm,
        "dataset_arm": spec.dataset_arm,
        "series_memberships": [
            {"series": series, "value": value} for series, value in spec.series_memberships
        ],
        "parameters": {
            "occurrence_count": spec.occurrence_count,
            "k": spec.k,
            "w": 19 if spec.arm == "map_hifi_fast_long_calibration" else 10,
            "mapping_threads": 1,
        },
        "dataset": {
            "reference_path": str(reference.relative_to(ROOT)),
            "reference_sha256": reference_entry["sha256"],
            "reference_length": workload["reference_length"],
            "intended_occurrences": reference_entry["intended_occurrences"],
            "verified_exact_source_occurrences": reference_entry["verified_exact_source_occurrences"],
            "query_path": str(query.relative_to(ROOT)),
            "query_sha256": workload["query_sha256"],
            "source_length": workload["source_length"],
            "query_length_after_edits": workload["query_length_after_edits"],
        },
        "argv": argv,
        "argv_shell_quoted_for_display": shlex.join(argv),
        "started_at_utc": started,
        "completed_at_utc": completed,
        "exit_code": process.returncode,
        "timing": timing or {"wrapper_observed_wall_seconds": observed_wall},
        "wrapper_observed_wall_seconds": observed_wall,
        "stderr_path": str(stderr_path.relative_to(ROOT)),
        "dump_directory": str(dump_dir.relative_to(ROOT)) if dump_dir else None,
        "map_diagnostics": map_records,
        "backtrack_records": [decorate_record(record) for record in runl_records],
        "summary": summaries,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=ROOT / "experiment_data" / "manifest.json")
    parser.add_argument("--output", type=Path, default=ROOT / "experiment_results.yaml")
    parser.add_argument("--raw-dir", type=Path, default=ROOT / "experiment_raw")
    parser.add_argument("--pilot", action="store_true")
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    args = parser.parse_args()

    minimap2 = ROOT / "minimap2"
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    args.raw_dir.mkdir(parents=True, exist_ok=True)
    if args.output.exists():
        results = yaml.safe_load(args.output.read_text(encoding="utf-8"))
    else:
        results = initialize_results(manifest, minimap2)
    completed_ids = {
        run["run_id"] for run in results["runs"] if run["status"].startswith("completed")
    }
    specs = build_specs()
    if args.pilot:
        specs = [spec for spec in specs if spec.run_id in PILOT_RUN_IDS]
    pending = [spec for spec in specs if spec.run_id not in completed_ids]
    jobs = max(1, min(args.jobs, len(pending) or 1))
    invocation_started = utc_now()
    print(f"{invocation_started} pending_runs={len(pending)} pilot={args.pilot} jobs={jobs}", flush=True)
    resource_samples: list[dict[str, int]] = []
    stop_monitor = threading.Event()
    monitor = threading.Thread(
        target=monitor_resources, args=(stop_monitor, resource_samples), daemon=True
    )
    monitor.start()
    failed_exit_code = 0
    try:
        with ThreadPoolExecutor(max_workers=jobs) as executor:
            futures = {
                executor.submit(run_one, spec, manifest, args.raw_dir, minimap2): spec
                for spec in pending
            }
            for index, future in enumerate(as_completed(futures), 1):
                spec = futures[future]
                run = future.result()
                results["runs"].append(run)
                all_errors = [
                    error
                    for existing in results["runs"]
                    for error in existing["summary"]["all_validation_errors"]
                ]
                results["validations"]["all_instrumentation_invariants_pass"] = not all_errors
                save_results(args.output, results)
                initial_l4 = (run["summary"]["initial_dp_by_lane"] or {}).get("4")
                coverage = None if not initial_l4 else initial_l4["rates"]["fast_path_anchor_coverage"]["percent"]
                current_memory = memory_snapshot()
                print(
                    f"{utc_now()} [{index}/{len(pending)}] {spec.run_id} status={run['status']} "
                    f"wall={run['wrapper_observed_wall_seconds']:.3f}s L4_path_coverage_percent={coverage} "
                    f"mem_available_mib={current_memory['memavailable_kib'] / 1024:.1f} "
                    f"swap_used_mib={current_memory['swap_used_kib'] / 1024:.1f}",
                    flush=True,
                )
                if run["exit_code"] != 0 and failed_exit_code == 0:
                    failed_exit_code = int(run["exit_code"])
    finally:
        stop_monitor.set()
        monitor.join()
    invocation_completed = utc_now()
    if not resource_samples:
        resource_samples.append(memory_snapshot())
    results.setdefault("resource_monitoring", []).append(
        summarize_resource_samples(
            resource_samples,
            jobs=jobs,
            started_at=invocation_started,
            completed_at=invocation_completed,
        )
    )
    save_results(args.output, results)
    if failed_exit_code:
        print("one or more runs failed", file=sys.stderr, flush=True)
        sys.exit(failed_exit_code)

    if not args.pilot:
        results["experiment"]["status"] = "measurement_complete_pending_independent_review"
        results["experiment"]["completed_at_utc"] = utc_now()
        order = {spec.run_id: index for index, spec in enumerate(build_specs())}
        results["runs"].sort(key=lambda run: order[run["run_id"]])
        save_results(args.output, results)
    print(f"{utc_now()} done output={args.output}", flush=True)


if __name__ == "__main__":
    main()
