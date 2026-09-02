#!/usr/bin/env python3
"""Adapt generated unique-minimizer comb controls to the repeat runner schema.

These cases are coordinate/island adversarial controls.  They validate exact
anchor, score, predecessor, and Run_L behavior.  They are not measurements of
repeat frequency in ordinary sequence data.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

from runl_repeat_common import (
    DEFAULT_LANE_WIDTHS,
    ROOT,
    atomic_write_json,
    canonical_json_sha256,
    sha256_file,
    utc_timestamp,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--comb-root", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def require_child(path: Path, label: str) -> Path:
    resolved = path.resolve()
    try:
        resolved.relative_to(ROOT.resolve())
    except ValueError as error:
        raise ValueError(f"{label} must resolve below {ROOT.resolve()}") from error
    if resolved == ROOT.resolve():
        raise ValueError(f"{label} must be a strict child of {ROOT.resolve()}")
    return resolved


def read_fasta(path: Path) -> tuple[str, bytes]:
    name: str | None = None
    sequence = bytearray()
    with path.open("rb") as source:
        for line_number, raw_line in enumerate(source, start=1):
            line = raw_line.rstrip(b"\r\n")
            if line.startswith(b">"):
                if name is not None:
                    raise ValueError(f"{path}:{line_number}: multiple FASTA records")
                name = line[1:].split(maxsplit=1)[0].decode("ascii")
            else:
                if name is None:
                    raise ValueError(f"{path}:{line_number}: sequence precedes header")
                compact = b"".join(line.split()).upper()
                if any(base not in b"ACGTN" for base in compact):
                    raise ValueError(f"{path}:{line_number}: invalid base")
                sequence.extend(compact)
    if not name or not sequence:
        raise ValueError(f"{path}: expected one nonempty FASTA record")
    return name, bytes(sequence)


def fasta_artifact(path: Path, manifest_root: Path) -> dict[str, Any]:
    name, sequence = read_fasta(path)
    return {
        "path": str(path.resolve().relative_to(manifest_root.resolve())),
        "file_sha256": sha256_file(path),
        "sequence_sha256": hashlib.sha256(sequence).hexdigest(),
        "name": name,
        "length": len(sequence),
        "alphabet": "ACGTN" if b"N" in sequence else "ACGT",
    }


def reconstruct_declared_path(anchors: list[dict[str, Any]]) -> list[int]:
    current = len(anchors) - 1
    path: list[int] = []
    while current >= 0:
        if current in path:
            raise ValueError("declared comb predecessor cycle")
        path.append(current)
        predecessor = int(anchors[current]["predecessor"])
        if predecessor < -1 or predecessor >= current:
            raise ValueError(f"invalid declared comb predecessor at {current}")
        current = predecessor
    return path


def main() -> int:
    arguments = parse_args()
    comb_root = require_child(arguments.comb_root, "--comb-root")
    output = require_child(
        arguments.output or (comb_root / "runner_manifest.json"), "--output"
    )
    if output.parent != comb_root:
        raise ValueError("--output must be an immediate child of --comb-root")
    case_manifests = sorted(comb_root.glob("m_*/manifest.json"))
    if not case_manifests:
        raise ValueError(f"no m_*/manifest.json files found below {comb_root}")
    cases: list[dict[str, Any]] = []
    seeds: list[int] = []
    adapter_root = comb_root / "runner_adapter"
    adapter_root.mkdir(parents=True, exist_ok=True)
    for order, source_manifest_path in enumerate(case_manifests):
        with source_manifest_path.open("r", encoding="utf-8") as source:
            source_manifest = json.load(source)
        if source_manifest.get("control_role") != "unique_minimizer_coordinate_island_adversarial_control":
            raise ValueError(f"{source_manifest_path}: missing adversarial-control label")
        if source_manifest.get("run_l_widths") != list(DEFAULT_LANE_WIDTHS):
            raise ValueError(f"{source_manifest_path}: lane widths differ from runner")
        anchors = source_manifest.get("anchors")
        if not isinstance(anchors, list) or not anchors:
            raise ValueError(f"{source_manifest_path}: missing exact anchor array")
        m = int(source_manifest["m"])
        seed = int(source_manifest["seed"])
        seeds.append(seed)
        case_id = f"comb_m{m:04d}_seed{seed}"
        case_directory = source_manifest_path.parent
        reference_path = case_directory / "reference.fa"
        query_path = case_directory / "query.fa"
        reference = fasta_artifact(reference_path, comb_root)
        query = fasta_artifact(query_path, comb_root)
        declared_path = reconstruct_declared_path(anchors)
        top_score = max(int(anchor["score"]) for anchor in anchors)
        top_score_indices = [
            index
            for index, anchor in enumerate(anchors)
            if int(anchor["score"]) == top_score
        ]
        expected_top_indices = [
            index
            for index, anchor in enumerate(anchors)
            if str(anchor["label"]).startswith("r")
            and str(anchor["label"]).endswith("_39")
        ]
        if (
            top_score != 615
            or top_score_indices != expected_top_indices
            or len(top_score_indices) != m
            or top_score_indices[-1] != len(anchors) - 1
        ):
            raise ValueError(
                f"{source_manifest_path}: unexpected top-score tie: "
                f"score={top_score}, indices={top_score_indices}, "
                f"expected={expected_top_indices}"
            )
        selected_start_index = top_score_indices[-1]
        selected_start_label = f"r{m:03d}_39"
        if str(anchors[selected_start_index]["label"]) != selected_start_label:
            raise ValueError(
                f"{source_manifest_path}: selected start label differs from "
                f"{selected_start_label}"
            )
        anchors_by_label = {str(anchor["label"]): anchor for anchor in anchors}
        if len(anchors_by_label) != len(anchors):
            raise ValueError(f"{source_manifest_path}: duplicate anchor label")
        try:
            first_comb_anchor = anchors_by_label[f"r{m:03d}_02"]
            last_comb_anchor = anchors_by_label[selected_start_label]
        except KeyError as error:
            raise ValueError(
                f"{source_manifest_path}: local comb PAF endpoint is absent"
            ) from error
        construction = {
            "schema_version": 1,
            "construction_type": "unique_minimizer_island_adversarial_control",
            "control_role": "unique_minimizer_coordinate_island_adversarial_control",
            "repeat_frequency_evidence": False,
            "case_id": case_id,
            "m": m,
            "seed": seed,
            "k": int(source_manifest["k"]),
            "w": int(source_manifest["w"]),
            "anchors": anchors,
            "expected_run_l": source_manifest["expected_run_l"],
            "source_manifest": {
                "path": str(source_manifest_path.resolve().relative_to(comb_root)),
                "file_sha256": sha256_file(source_manifest_path),
            },
        }
        construction_path = adapter_root / f"{case_id}.construction.json"
        construction_digest = atomic_write_json(construction_path, construction)
        cases.append(
            {
                "case_id": case_id,
                "replicate_id": f"comb_m{m:04d}",
                "seed": seed,
                "group": "unique_minimizer_island_adversarial_control",
                "parameters": {
                    "construction": "comb_unique_minimizer_islands",
                    "m": m,
                    "interpretation": "not repeat-frequency evidence",
                },
                "reference": reference,
                "query": query,
                "construction_artifact": {
                    "path": str(construction_path.resolve().relative_to(comb_root)),
                    "file_sha256": construction_digest,
                    "content_sha256": canonical_json_sha256(construction),
                },
                "expectation": {
                    "minimum_path_anchors": len(declared_path),
                    "require_unique_top_score": m == 1,
                    "expected_maximum_score_candidate_multiplicity": m,
                    "candidate_tie_policy": {
                        "candidate_sort_order": "score_ascending_then_anchor_index_ascending",
                        "selection_rule": "consume_candidates_from_end",
                        "equal_score_selection_order": "anchor_index_descending",
                        "expected_top_score": top_score,
                        "expected_top_score_anchor_indices": top_score_indices,
                        "expected_first_start_index": selected_start_index,
                        "expected_first_start_label": selected_start_label,
                    },
                    "require_no_query_minimizer_filter_loss": True,
                    "seed_frequency": {
                        "allowed_initial_reference_occurrences": [1],
                        "required_initial_filtered_count": 0,
                    },
                    "paf": {
                        "minimum_record_count": 1,
                        "required_target_name": reference["name"],
                        "required_query_name": query["name"],
                        "required_strand": "+",
                        "maximum_query_start": int(first_comb_anchor["query"]),
                        "minimum_query_end": int(last_comb_anchor["query"]) + 1,
                        "maximum_target_start": int(first_comb_anchor["reference"]),
                        "minimum_target_end": int(last_comb_anchor["reference"]) + 1,
                        "qualified_record_scope": "selected_private_branch_r_m_02_through_r_m_39",
                    },
                    "comb_exact_arrays": True,
                },
            }
        )
    seed_plan = {
        "adapter": Path(__file__).name,
        "comb_root": str(comb_root.relative_to(ROOT.resolve())),
        "source_seeds": seeds,
        "source_manifest_sha256": [sha256_file(path) for path in case_manifests],
    }
    manifest = {
        "schema_version": 1,
        "status": "complete",
        "experiment_id": "runl_repeat_disruption",
        "profile": "comb_adversarial_control",
        "created_at_utc": utc_timestamp(),
        "generation": {
            "adapter": Path(__file__).name,
            "adapter_sha256": sha256_file(Path(__file__)),
            "outcome_independent": True,
        },
        "lane_widths": list(DEFAULT_LANE_WIDTHS),
        "mapping": {
            "args": ["-k", "15", "-w", "10"],
            "threads": 1,
            "timeout_seconds": 300,
            "emit_seed_coordinates": True,
            "emit_seed_frequencies": True,
            "analysis_scope": "initial DP, with any RMQ retained separately",
        },
        "seed_plan": seed_plan,
        "seed_plan_sha256": canonical_json_sha256(seed_plan),
        "cases": cases,
    }
    digest = atomic_write_json(output, manifest)
    print(json.dumps({"manifest": str(output), "sha256": digest, "cases": len(cases)}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
