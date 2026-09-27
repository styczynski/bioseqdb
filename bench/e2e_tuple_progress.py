#!/usr/bin/env python3
"""Distinct approved q1024 tuples / remaining matrix cells, by machine family.

Run without arguments to print counters; --log appends a DOC0432 checkpoint.
Only logging advances the delta reference. Replicas and timing scopes do not
multiply tuple coverage. These counters do not assert comparable timing scopes.
"""
from __future__ import annotations

import argparse
import fcntl
import json
import time

import yaml
import monitor_e2e_fleet as monitor
from recover_e2e_measurements import MANIFEST, ROOT, atomic, authority_path

FAMILIES = ("local", "c7g", "m4", "m7a", "m7i")
OUTPUT = ROOT / "reporting/bioseq_benchmark_e2e/fleet_controller"
STATE = OUTPUT / "tuple_progress_last_logged.json"
# Resolved through bioseq-build doc_read(DOC0432).
LOG = ROOT / "DOCS/CURRENT/BENCHMARK_FRAMEWORK/0432_LOCAL_E2E_ALGORITHM_MATRIX_BENCHMARK_LOG.md"
DEFINITION = (
    "Distinct audited/approved q1024 tuples: original five-repeat results plus "
    "explicitly separate, user-authorized one-sample FAST results; replicas "
    "and historical/current timing scopes are deduplicated by stable key. "
    "Coverage does not imply that different timing scopes can be compared. "
    "Remaining includes running, unaudited, failed and currently blocked cells. "
    "Legacy ksw2_batched is excluded; remote Vulkan cells are hardware-unavailable "
    "and excluded. DNA5 and 4G software/capacity blockers remain in the totals. "
    "Deltas are relative to the previous tuple-counter log entry."
)


def tuple_counts() -> dict:
    children = json.loads(MANIFEST.read_bytes())["children"]
    eligible = {family: set() for family in FAMILIES}
    for child in children:
        identity = child["execution_cell"]["identity"]
        algorithms = [identity.get("requested_" + stage + "_algorithm")
                      for stage in ("minimizer", "chaining", "base_alignment")]
        if "ksw2_batched" in algorithms:
            continue
        gpu = any("vulkan" in str(value).lower() for value in algorithms)
        for family in FAMILIES:
            if family == "local" or not gpu:
                eligible[family].add(child["stable_key"])
    approved = {family: set() for family in FAMILIES}
    # Retiring a machine never removes its historical measured coverage.
    for host in ("local", *(row[0] for row in monitor.registered_hosts(include_retired=True))):
        family = host.rsplit("-", 1)[-1]
        # Missing/unreadable authority is an error, never a spurious zero/delta.
        document = yaml.load(authority_path(host).read_bytes(), Loader=yaml.CSafeLoader)
        approved[family].update(
            row["stable_key"] for row in document["approved_results"]
            if row.get("query_count") == 1024 and row.get("run_count", 0) >= 5
            and row.get("status") == "accepted" and row.get("prefilter") is False
            and row.get("exact_baseline_match_count_equal") is True
        )
    for family in FAMILIES:
        filename = ('ALL_RESULTS_TUPLES_FAST_LOCAL.yaml' if family == 'local'
                    else 'ALL_RESULTS_TUPLES_FAST_REMOTE_' + family.upper() + '.yaml')
        path = ROOT / filename
        if not path.exists():
            continue
        document = yaml.load(path.read_bytes(), Loader=yaml.CSafeLoader)
        expected_scope = ('query_only_framework_iteration_fast_local_single_sample_v1'
                          if family == 'local' else 'query_only_framework_iteration_fast_remote_single_sample_v1')
        approved[family].update(row['stable_key'] for row in document['approved_results']
            if row.get('query_count') == 1024 and row.get('run_count') == 1
            and row.get('timing_scope') == expected_scope and row.get('status') == 'accepted'
            and row.get('prefilter') is False and row.get('exact_baseline_match_count_equal') is True)
    return {family: {"completed": len(approved[family] & eligible[family]),
                     "remaining": len(eligible[family] - approved[family])}
            for family in FAMILIES}


def format_counters(counts: dict, previous: dict | None) -> str:
    lines = ["| Family | Approved tuples (Δ) | Remaining tuples (Δ) |",
             "| :-- | --: | --: |"]
    for family in FAMILIES:
        current = counts[family]
        old = previous[family] if previous is not None else current
        cells = [f"{current[key]} ({current[key] - old[key]:+d})"
                 for key in ("completed", "remaining")]
        lines.append(f"| {family.upper() if family != 'local' else 'Local'} | "
                     + " | ".join(cells) + " |")
    return "\n".join(lines)


def append_progress_log(details: str = "", *, title: str = "q1024 tuple coverage checkpoint") -> str:
    # Billing lookups must not prevent tuple logging or benchmark queue refill.
    try:
        from e2e_aws_credits import format_credit_balances
        credits = format_credit_balances()
    except Exception as exc:
        credits = "AWS credits unavailable: " + str(exc)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with (OUTPUT / "tuple_progress.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        counts = tuple_counts()
        previous = json.loads(STATE.read_bytes())["counts"] if STATE.exists() else None
        table = format_counters(counts, previous)
        stamp = time.strftime("%Y-%m-%d %H:%M %Z")
        first = " First counter entry establishes the reference (+0); earlier logs had no equivalent counter." if previous is None else ""
        with LOG.open("a") as output:
            output.write(f"\n### {stamp} — {title}\n\n{table}\n\n{DEFINITION}{first}\n")
            if details:
                output.write("\n" + details.rstrip() + "\n")
            output.write("\n" + credits + "\n")
        atomic(STATE, (json.dumps({"timestamp": stamp, "counts": counts}, indent=2) + "\n").encode())
        return table


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", action="store_true", help="Append counters and advance the logged delta reference")
    args = parser.parse_args()
    if args.log:
        print(append_progress_log())
    else:
        previous = json.loads(STATE.read_bytes())["counts"] if STATE.exists() else None
        print(format_counters(tuple_counts(), previous))


if __name__ == "__main__":
    main()
