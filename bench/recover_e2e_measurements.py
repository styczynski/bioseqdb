#!/usr/bin/env python3
"""Recover complete q1024 measurements without requiring ephemeral MCP receipts.

User-authorized policy, 2026-09-21: missing build/run receipts are a disclosed
provenance limitation, not evidence of invalid numerical measurements. Reuse
the existing raw-sidecar, correctness, physical-stage, interval and baseline
validators. Never infer missing samples, manufacture receipts, overwrite an
approved observation, or mix timing scopes. Archives remain audit evidence;
ALL_RESULTS_TUPLES*.yaml remain the self-contained numerical authority.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import tarfile
import tempfile

import yaml
import aggregate_e2e_algorithm_matrix as audit

ROOT = Path(__file__).resolve().parent.parent
COLLECTED = ROOT / "reporting/bioseq_benchmark_e2e/fleet_collected_20260921"
MANIFEST = ROOT / "bench/bench_e2e_algorithm_matrix_q1024_observed_singleton_partition_manifest.json"
POLICY = (
    " Recovery policy authorized 2026-09-21: complete saved raw measurements may "
    "be accepted without ephemeral build/run receipts or a sealed append manifest. "
    "Recovered rows are explicitly annotated; this does not attest their exact "
    "source/binary provenance. Existing checks for five repeats, correctness, "
    "observed stages, disabled prefilter, timing intervals and minimap2 match "
    "counts remain required. No minimizer bit-parity is required."
)
COMMENT = (
    "Recovered from complete checksummed raw measurements; build/run receipt "
    "provenance was not required or re-established. Exact source/binary provenance "
    "is therefore unverified. Existing correctness, stage and interval validators "
    "passed for all five repeats. Process priority and OS cache state are not attested."
)


def atomic(path: Path, contents: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(contents)
    os.replace(temporary, path)


def authority_path(host: str) -> Path:
    if host == "local":
        return ROOT / "ALL_RESULTS_TUPLES.yaml"
    suffix = host.upper().replace("-", "_").removeprefix("DEFAULT_")
    return ROOT / f"ALL_RESULTS_TUPLES_REMOTE_{suffix}.yaml"


def parse_report(report_bytes: bytes, raw_bytes: bytes, name: str, children: dict) -> list[dict]:
    report = json.loads(report_bytes)
    lines = raw_bytes.splitlines()
    if len(lines) != 7:
        raise ValueError("recovery currently supports complete five-repeat singleton reports only")
    first = audit._validate_record(json.loads(lines[1]), {}, audit.Q1024_CAMPAIGN_ID, 1024, True)
    child = children[first["stable_key"]]
    config = json.loads((ROOT / child["path"]).read_bytes())
    entry = {"expected_raw_records": 5, "cells": [child["execution_cell"]]}
    # The existing validator checks the footer, embedded report values, complete
    # membership, schedule, checksums and summaries, not just the five medians.
    with tempfile.TemporaryDirectory(prefix=".bioseq-numeric-audit-", dir=ROOT) as temporary:
        report_path = Path(temporary) / name
        if report_path.is_absolute() and not report_path.is_relative_to(Path(temporary)):
            raise ValueError("archive report escapes audit directory")
        if ".." in Path(name).parts:
            raise ValueError("archive report contains parent traversal")
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_bytes(report_bytes)
        report_path.with_suffix(".raw.jsonl").write_bytes(raw_bytes)
        records, manifest, _, _ = audit._validate_raw_sidecar(report_path, report, config, entry)
    source = {
        "report": name, "raw_sidecar": name.replace(".json", ".raw.jsonl"),
        "run_id": manifest["run_id"],
        "schedule_sorted_checksum": manifest["schedule_sorted_checksum"],
        "run_receipt_id": None, "run_receipt_sha256": None,
    }
    return [audit._validate_record(r, source, audit.Q1024_CAMPAIGN_ID, 1024, True) for r in records]


def approved_row(row: dict) -> dict:
    engine = row["engine"]
    requested = {k: row[f"requested_{k}_algorithm"] for k in ("minimizer", "chaining", "base_alignment")}
    observed = {k: row[f"observed_{k}_algorithm"] for k in requested}
    exact = requested == observed
    if engine == "minimap2":
        conformance, scope, path = "external_minimap2_baseline", "external_minimap2_baseline", "minimap2"
    elif exact:
        conformance, scope = "exact_requested_path", "requested_and_effective_path"
        path = " -> ".join(observed.values())
    elif (row["alphabet"] == "dna5" and requested["minimizer"] == "simd_minimap2_reference"
          and observed["minimizer"] == "scalar"
          and all(requested[k] == observed[k] for k in ("chaining", "base_alignment"))):
        conformance, scope = "documented_scalar_minimizer_fallback", "planned_dna5_reference_scalar_only"
        path = " -> ".join(observed.values())
    else:
        raise ValueError("unclassified observed path; needs review, not automatic approval")
    result = {
        "stable_key_schema": "bioseq-e2e-cell-v2", "stable_key": row["stable_key"],
        "timing_scope": audit.APPROVED_TUPLES_QUERY_TIMING_SCOPE,
        **{k: row[k] for k in ("campaign_id", "dataset", "target_size_gbp", "alphabet", "kmer_size", "window_size")},
        "query_count": 1024, "mode": row["index_mode"], "pipeline": engine,
        "pipeline_mode": "task_parallel" if engine == "bioseq" else None,
        "index_type": row["bioseq_index_name"] if engine == "bioseq" else "minimap2_internal",
        "requested_algorithms": requested, "observed_algorithms": observed,
        "effective_path": path, "execution_conformance": conformance,
        "comparison_eligibility": {"requested_path": exact, "effective_path": True, "scope": scope},
        "prefilter": False, "exact_baseline_match_count_equal": True, "status": "accepted", "run_count": 5,
        "metrics": {"unit": "ns", "p50_a": row["outer_e2e_ns_p50"], "p50_b": row["outer_e2e_minus_cigar_or_mapq_union_ns_p50"]},
        "comments": ((row["COMMENTS"] + "; ") if row["COMMENTS"] else "") + COMMENT,
    }
    return audit._approved_row_in_fixed_order(result)


def family_baselines(host_dir: Path) -> tuple[dict, list[dict]]:
    """Reuse checked correctness evidence, never another host's timings.

    All records were validated against the same canonical q1024 singleton
    fixture/configuration. Require every available baseline for an identity
    to agree on the ordered per-query counts in each repeat; disagreements
    are not resolved by selecting whichever baseline makes Bioseq pass.
    """
    family = host_dir.name.rsplit("-", 1)[-1]
    grouped = {}
    for peer in sorted(COLLECTED.iterdir()):
        if not peer.is_dir() or peer.name.rsplit("-", 1)[-1] != family:
            continue
        path = peer / "numeric_recovery_cache.json"
        if not path.exists():
            continue
        cache = json.loads(path.read_bytes())
        if cache.get("validator_version") != 2:
            continue
        for item in cache.get("reports", {}).values():
            records = item.get("records", [])
            if (len(records) != 5 or records[0]["identity"]["engine"] != "minimap2"
                    or not all(r["accepted"] and r["correctness_evaluated"]
                               and r["match_count"] == r["correct_match_count"] == r["expected_match_count"]
                               and r["ordered_oracle"] is not None for r in records)):
                continue
            records = sorted(records, key=lambda r: r["repeat"])
            if [r["repeat"] for r in records] != list(range(5)):
                continue
            signature = tuple(tuple(r["ordered_oracle"]["counts"]) for r in records)
            grouped.setdefault(records[0]["stable_key"], []).append((peer.name, records, signature))
    baselines, conflicts = {}, []
    for key, options in grouped.items():
        if len({option[2] for option in options}) != 1:
            conflicts.append({"stable_key": key, "error": "same-family saved minimap2 correctness baselines disagree; no peer baseline reused"})
        else:
            baselines[key] = options[0][:2]
    return baselines, conflicts


def recover(host_dir: Path, children: dict, publish: bool) -> dict:
    cache_path = host_dir / "numeric_recovery_cache.json"
    cache = json.loads(cache_path.read_bytes()) if cache_path.exists() else {}
    if cache.get("validator_version") != 2:
        cache = {"validator_version": 2, "reports": {}}
    previous_inventory = dict(cache.get("archive_inventory", {}))
    # Content-addressing avoids repeating a large audit every scheduler tick.
    for archive in sorted(host_dir.glob("*.tar.gz")):
        stamp = [archive.stat().st_size, archive.stat().st_mtime_ns]
        previous = cache.setdefault("archive_inventory", {}).get(archive.name)
        if previous and previous["stamp"] == stamp and previous["sha256"] in cache.get("archives", []):
            continue
        archive_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
        cache["archive_inventory"][archive.name] = {"stamp": stamp, "sha256": archive_hash}
        if archive_hash in cache.get("archives", []):
            continue
        with tarfile.open(archive) as tar:
            names = {m.name for m in tar.getmembers() if m.isfile()}
            for name in sorted(names):
                raw_name = name.replace(".json", ".raw.jsonl")
                if not name.endswith(".json") or raw_name not in names:
                    continue
                report_bytes = tar.extractfile(name).read()
                raw_bytes = tar.extractfile(raw_name).read()
                digest = hashlib.sha256(report_bytes + raw_bytes).hexdigest()
                if digest in cache["reports"]:
                    continue
                item = {"name": name, "archive": archive.name}
                try:
                    item["records"] = parse_report(report_bytes, raw_bytes, name, children)
                except Exception as exc:
                    item["error"] = str(exc)
                cache["reports"][digest] = item
        cache.setdefault("archives", []).append(archive_hash)
        atomic(cache_path, (json.dumps(cache) + "\n").encode())
    if previous_inventory != cache.get("archive_inventory", {}):
        atomic(cache_path, (json.dumps(cache) + "\n").encode())
    # Select an entire independently accepted report, never cherry-pick repeats
    # across reports. Preserve the first valid numerical observation per tuple.
    cells = {}
    errors = []
    for item in cache["reports"].values():
        if "error" in item:
            errors.append({"report": item["name"], "error": item["error"]})
            continue
        records = item["records"]
        if not records:
            continue
        key = records[0]["stable_key"]
        if key not in cells or not all(r["accepted"] for r in cells[key]):
            cells[key] = records
    owned_keys = set(cells)
    shared, baseline_conflicts = family_baselines(host_dir)
    errors.extend(baseline_conflicts)
    reused = {}
    for key, records in list(cells.items()):
        identity = records[0]["identity"]
        if identity["engine"] != "bioseq":
            continue
        baseline_identity = dict(identity, engine="minimap2", bioseq_index_name=None,
                                 requested_minimizer_algorithm=None,
                                 requested_chaining_algorithm=None,
                                 requested_base_alignment_algorithm=None)
        baseline_key = audit._stable_key_for_identity(baseline_identity)
        if baseline_key not in cells and baseline_key in shared:
            donor, baseline_records = shared[baseline_key]
            cells[baseline_key] = baseline_records
            reused[baseline_key] = donor
    aggregated = [row for row in audit._aggregate([r for records in cells.values() for r in records])
                  if row["stable_key"] in owned_keys]
    for row in aggregated:
        identity = dict(children[row["stable_key"]]["execution_cell"]["identity"],
                        engine="minimap2", bioseq_index_name=None,
                        requested_minimizer_algorithm=None, requested_chaining_algorithm=None,
                        requested_base_alignment_algorithm=None)
        donor = reused.get(audit._stable_key_for_identity(identity))
        if donor:
            note = (f"Correctness-only minimap2 baseline reused from {donor} in the same machine family, "
                    "with the same canonical q1024 dataset/configuration and repeat ordinals. "
                    "Saved baseline ordered counts agree; no donor timings were used or published for this host.")
            row["COMMENTS"] = ((row["COMMENTS"] + "; ") if row["COMMENTS"] else "") + note
    candidates = []
    for row in aggregated:
        if row["status"] == "accepted":
            try:
                candidates.append(approved_row(row))
            except ValueError as exc:
                errors.append({"stable_key": row["stable_key"], "error": str(exc)})
    added = 0
    path = authority_path(host_dir.name)
    if publish and candidates and path.exists():
        with path.with_name("." + path.name + ".lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            document = yaml.load(path.read_bytes(), Loader=yaml.CSafeLoader)
            existing = document["approved_results"]
            seen = {(r["stable_key"], r["timing_scope"]) for r in existing}
            for row in candidates:
                identity = (row["stable_key"], row["timing_scope"])
                if identity in seen:
                    continue
                checked = audit._validate_approved_tuple_row(
                    row, audit.Q1024_CAMPAIGN_ID, document["matrix_scope"], "recovered row",
                    allow_legacy_preservation=False, require_field_order=True)
                existing.append(checked)
                seen.add(identity)
                added += 1
            if added or document["authority"].get("sealed_verification_manifest_required_for_append") is False:
                document["authority"]["sealed_verification_manifest_required_for_append"] = False
                if POLICY not in document["purpose"]:
                    document["purpose"] += POLICY
                if host_dir.name == "local":
                    audit._regenerate_approved_tuple_projections(document, existing)
                else:
                    # All campaign CPU cells are now eligible, independent of
                    # whether their counterpart has completed on the local host.
                    keys = sorted(children)
                    document["eligible_stable_keys"] = keys
                    document["hardware_unavailable_cells"] = [
                        audit._remote_hardware_disposition_from_identity(
                            children[key]["canonical_cell"]["identity"], query_count=1024)
                        for key in keys
                        if audit._remote_identity_requires_vulkan(children[key]["canonical_cell"]["identity"])]
                    document["projection_provenance"].update({
                        "eligible_stable_keys_projection_sha256": audit._sha256(audit._canonical_bytes(keys)),
                        "hardware_unavailable_cells_projection_sha256": audit._sha256(
                            audit._canonical_bytes(document["hardware_unavailable_cells"])),
                    })
                    audit._regenerate_remote_approved_tuple_projections(document, existing)
                atomic(path, audit._canonical_approved_tuples_yaml_v3(document))
    result = {"host": host_dir.name, "approved_candidates": len(candidates), "added": added,
              "authority_missing": not path.exists(), "errors": errors,
              "dispositions": [{"stable_key": r["stable_key"], "status": r["status"], "comments": r["COMMENTS"]} for r in aggregated]}
    atomic(host_dir / "numeric_recovery_summary.json", (json.dumps(result, indent=2) + "\n").encode())
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--publish", action="store_true")
    parser.add_argument("--host")
    args = parser.parse_args()
    # One auditor owns the caches/publication pass, even after controller restart.
    lock = (COLLECTED / ".recovery.lock").open("a")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print("Recovery already active; leaving existing audit running.", flush=True)
        return
    children = {c["stable_key"]: c for c in json.loads(MANIFEST.read_bytes())["children"]}
    for host in sorted(COLLECTED.iterdir()):
        if host.is_dir() and (not args.host or args.host == host.name):
            try:
                result = recover(host, children, args.publish)
            except Exception as exc:
                print(json.dumps({"host": host.name, "publication_error": str(exc)}), flush=True)
                continue
            print(json.dumps({k: v for k, v in result.items() if k not in ("dispositions", "errors")}
                             | {"errors": len(result["errors"])}), flush=True)


if __name__ == "__main__":
    main()
