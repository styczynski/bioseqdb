#!/usr/bin/env python3
"""Generate/check the sealed hybrid packed q1024 execution partition.

The canonical 1,296-cell campaign and its 88 parent shards remain topology
authority.  This generator only replaces selected singleton execution owners
with exact multi-cell configs, maps four deadline-v2 q1024 anchors onto their
canonical stable keys, and leaves every other cell owned by its original
singleton.  It never builds or runs a benchmark.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import tempfile
from typing import Any

import generate_e2e_algorithm_matrix_shards as canonical


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
CANONICAL_CONTRACT_PATH = (
    REPOSITORY_ROOT / "bench/bench_e2e_algorithm_matrix_campaign_contract.json")
SINGLETON_MANIFEST_PATH = (
    REPOSITORY_ROOT
    / "bench/bench_e2e_algorithm_matrix_q1024_singleton_partition_manifest.json")
DEADLINE_V2_MANIFEST_PATH = (
    REPOSITORY_ROOT / "bench/bench_e2e_algorithm_matrix_deadline_v2_manifest.json")
OUTPUT_DIRECTORY = (
    REPOSITORY_ROOT / "bench/e2e_algorithm_matrix_q1024_packed_shards")
OUTPUT_MANIFEST_PATH = (
    REPOSITORY_ROOT
    / "bench/bench_e2e_algorithm_matrix_q1024_packed_partition_manifest.json")

CANONICAL_CONTRACT_SHA256 = (
    "e80407ca3a515ca04ac7ad616f0c7f5e0ef477f3e09884ec8ab93370e8c55933")
SINGLETON_MANIFEST_SHA256 = (
    "6bfe0681b46087860b7d873cd868da8e70be5ecab85b817e6979a29fce6f2c3a")
DEADLINE_V2_MANIFEST_SHA256 = (
    "bf2abc14dbda84079d6384e330b1fdc752e1754360cc61d9fec42af1e2d1b31c")
DEADLINE_V2_CAMPAIGN_ID = "bioseq-e2e-algorithm-matrix-deadline-v2"
DEADLINE_V2_CAMPAIGN_DIGEST = (
    "44a85360a5e975ec8a6d70ac0b06486c51d748770c2ada46106d2ccc5db4728a")

PACKED_PATH_PREFIX = "bench/e2e_algorithm_matrix_q1024_packed_shards/"
PACKED_FILENAME_PREFIX = "bench_e2e_algorithm_matrix_q1024_packed_"
RESIDUAL_CHAINS = [
    "simd_minimap2",
    "range_minimum_query",
    "vulkan_tiled_dynamic_programming",
]
SUPPORTED_MINIMIZERS = [
    "scalar",
    "bounded_nthash_simd",
    "simd_minimap2_reference",
    "two_stack_nthash",
]
MINIMAP_PAIR_DATASETS = [
    "M2_D4_K20_W15",
    "M2_D5_K15_W10",
    "M2_D5_K20_W15",
    "M4_D4_K15_W10",
    "M4_D4_K20_W15",
    "M4_D5_K15_W10",
    "M4_D5_K20_W15",
]
M4_PERSISTED_MPS_DATASETS = [
    "M4_D4_K15_W10",
    "M4_D4_K20_W15",
    "M4_D5_K15_W10",
    "M4_D5_K20_W15",
]

SEALED_ADOPTION_ARTIFACTS: dict[int, dict[str, Any]] = {
    56: {
        "report": {
            "path": "reporting/bioseq_benchmark_e2e/report_18.json",
            "sha256": "d0f29a65845ef9ffffb115fc2c1c55ef070cf7aabd65449c996e274f42c69dd7",
        },
        "raw_sidecar": {
            "path": "reporting/bioseq_benchmark_e2e/report_18.raw.jsonl",
            "sha256": "dedc00bf1765ab3a422746074e52266d73592e7fba5f8fbc1a9aa46b8c70d69e",
        },
    },
    57: {
        "report": {
            "path": "reporting/bioseq_benchmark_e2e/report_17.json",
            "sha256": "6fac80a86020ee43b2197404fd1520ad71c5220d0340843602e45b98d8f5429b",
        },
        "raw_sidecar": {
            "path": "reporting/bioseq_benchmark_e2e/report_17.raw.jsonl",
            "sha256": "836eca1bf19104002082313f357b46cb9e45cb5b98332e10aeab90137f4d8b5f",
        },
    },
    58: {
        "report": {
            "path": "reporting/bioseq_benchmark_e2e/report_94.json",
            "sha256": "f3a46b0c442b8fcc11661fe5d91efb31e2099dee3d819b8af6e4c1a48bb1facb",
        },
        "raw_sidecar": {
            "path": "reporting/bioseq_benchmark_e2e/report_94.raw.jsonl",
            "sha256": "b6f488e08f19e440d89e75c9ac0f8203bbffab21fc97168fe1497cd392662994",
        },
    },
}

# S056/S057 were executed under the immutable deadline-v1 parent paths before
# deadline-v2 copied those byte-identical members into its child directory.
# Their run receipts therefore bind the parent spelling while their adopted
# execution authority is the deadline-v2 schedule member.
ADOPTION_RUN_RECEIPT_CONFIG_ALIASES = {
    56: (
        "bench/e2e_algorithm_matrix_deadline_shards/"
        "bench_e2e_algorithm_matrix_deadline_s056_q1024_q1024_anchor_"
        "m2_d4_k15_w10_no_index_minimap2.json"),
    57: (
        "bench/e2e_algorithm_matrix_deadline_shards/"
        "bench_e2e_algorithm_matrix_deadline_s057_q1024_q1024_anchor_"
        "m2_d4_k15_w10_just_index_minimap2.json"),
}
SEALED_ADOPTION_RUN_RECEIPTS = {
    56: "rr_f6564879423bfcd46562dbb1920d6111",
    57: "rr_aa3f5446efbfe0d2de6fdae382532ca9",
    58: "rr_1e43d9e01a8a6eee985b4240ffe98d64",
}


class GenerationError(RuntimeError):
    """Raised when a packed member is not an exact canonical projection."""


def _sha256(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _stable_keys_sha256(keys: list[str] | set[str]) -> str:
    return _sha256("\n".join(sorted(keys)).encode("utf-8"))


def _canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=True,
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def _render(value: Any) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=True, allow_nan=False)
            + "\n").encode("utf-8")


def _relative(path: Path) -> str:
    return path.relative_to(REPOSITORY_ROOT).as_posix()


def _read_exact(path: Path, expected_sha256: str, label: str) -> tuple[dict, bytes]:
    value, raw = canonical._read_json(path)
    if _sha256(raw) != expected_sha256:
        raise GenerationError(f"{label} SHA-256 changed")
    return value, raw


def _canonical_parent(
        entries: list[dict[str, Any]],
        *,
        test_case: str,
        shard_kind: str,
        minimizer_index: str | None = None,
        minimizer_algorithm: str | None = None,
) -> dict[str, Any]:
    matches = []
    for entry in entries:
        if entry.get("role") != "shard" or entry.get("shard_kind") != shard_kind:
            continue
        cells = entry.get("cells")
        if not isinstance(cells, list) or not cells:
            continue
        first = cells[0]
        identity = first.get("identity", {})
        if first.get("test_case") != test_case:
            continue
        if (minimizer_index is not None
                and identity.get("bioseq_index_name") != minimizer_index):
            continue
        if (minimizer_algorithm is not None
                and identity.get("requested_minimizer_algorithm")
                    != minimizer_algorithm):
            continue
        matches.append(entry)
    if len(matches) != 1:
        raise GenerationError(
            f"expected one {test_case}/{shard_kind}/{minimizer_algorithm}/"
            f"{minimizer_index} parent, found {len(matches)}")
    return matches[0]


def _read_parent_config(parent: dict[str, Any]) -> tuple[dict, bytes]:
    path = REPOSITORY_ROOT / parent["path"]
    config, raw = canonical._read_json(path)
    if _sha256(raw) != parent["config_sha256"]:
        raise GenerationError(f"canonical parent bytes changed: {parent['path']}")
    if canonical._config_cells(config) != parent["cells"]:
        raise GenerationError(f"canonical parent descriptors changed: {parent['path']}")
    return config, raw


def _residual_config(parent: dict[str, Any]) -> dict[str, Any]:
    config, _ = _read_parent_config(parent)
    result = copy.deepcopy(config)
    algorithm = "bioseq_algorithm_matrix_just_index"
    axes = result["bench_e2e"]["matrix"]["over"][algorithm]
    if axes.get("chaining_algorithm") != [
            "scalar_minimap2", "simd_minimap2", "range_minimum_query",
            "vulkan_tiled_dynamic_programming"]:
        raise GenerationError("canonical persisted chaining axis changed")
    axes["chaining_algorithm"] = list(RESIDUAL_CHAINS)
    result["bench_e2e"]["tags"] = [
        "e2e", "algorithm-matrix", "campaign-packed-q1024"]
    identity = parent["cells"][0]["identity"]
    result["bench_e2e"]["description"] = (
        "Sealed q1024 packed residual for "
        f"{parent['cells'][0]['test_case']} / "
        f"{identity['requested_minimizer_algorithm']} / "
        f"{identity['bioseq_index_name']}; scalar_minimap2 is separately owned.")
    return result


def _block_aligner_residual_config(parent: dict[str, Any]) -> dict[str, Any]:
    """Retain every chainer while leaving the KSW2 half singleton-owned."""
    config, _ = _read_parent_config(parent)
    result = copy.deepcopy(config)
    algorithm = "bioseq_algorithm_matrix_just_index"
    axes = result["bench_e2e"]["matrix"]["over"][algorithm]
    if axes.get("chaining_algorithm") != [
            "scalar_minimap2", "simd_minimap2", "range_minimum_query",
            "vulkan_tiled_dynamic_programming"]:
        raise GenerationError("canonical persisted chaining axis changed")
    if axes.get("base_alignment_algorithm") != ["ksw2", "block_aligner"]:
        raise GenerationError("canonical persisted alignment axis changed")
    axes["base_alignment_algorithm"] = ["block_aligner"]
    result["bench_e2e"]["tags"] = [
        "e2e", "algorithm-matrix", "campaign-packed-q1024"]
    identity = parent["cells"][0]["identity"]
    result["bench_e2e"]["description"] = (
        "Sealed q1024 packed Block Aligner residual for "
        f"{parent['cells'][0]['test_case']} / "
        f"{identity['requested_minimizer_algorithm']} / "
        f"{identity['bioseq_index_name']}; the four KSW2 tuples remain "
        "owned by their completed singleton evidence.")
    return result


def _minimap_pair_config(parent: dict[str, Any]) -> dict[str, Any]:
    config, _ = _read_parent_config(parent)
    result = copy.deepcopy(config)
    algorithms = ("minimap2_no_index", "minimap2_just_index")
    result["params"] = {
        name: copy.deepcopy(config["params"][name]) for name in algorithms}
    source_over = config["bench_e2e"]["matrix"]["over"]
    result["bench_e2e"]["matrix"]["over"] = {
        name: copy.deepcopy(source_over[name]) for name in algorithms}
    source_metrics = config["bench_e2e"]["matrix"]["metrics"]
    result["bench_e2e"]["matrix"]["metrics"] = {
        name: copy.deepcopy(source_metrics[name])
        for name in canonical.MINIMAP2_BASELINE_METRIC_NAMES
    }
    result["bench_e2e"]["matrix"]["metrics"]["match_count"]["rules"] = []
    canonical._restrict_metric_applicability(result)
    result["bench_e2e"]["tags"] = [
        "e2e", "algorithm-matrix", "campaign-packed-q1024"]
    result["bench_e2e"]["description"] = (
        "Sealed q1024 paired minimap2 no-index/just-index baselines for "
        f"{parent['cells'][0]['test_case']}.")
    return result


def _member(
        *,
        ordinal: int,
        member_id: str,
        kind: str,
        path: Path,
        raw: bytes,
        config: dict[str, Any],
        parent: dict[str, Any],
        canonical_by_key: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    execution_cells = canonical._config_cells(config)
    keys = [cell["stable_key"] for cell in execution_cells]
    if len(keys) != len(set(keys)):
        raise GenerationError(f"packed member repeats a stable key: {path}")
    try:
        canonical_cells = [copy.deepcopy(canonical_by_key[key]) for key in keys]
    except KeyError as exc:
        raise GenerationError(f"packed member owns a noncanonical cell: {exc}") from exc
    metrics = list(config["bench_e2e"]["matrix"]["metrics"])
    rules = canonical._nonblocking_rule_names(config)
    expected_disposition = canonical._expected_all_records_disposition(config)
    supported = sum(
        cell["identity"]["engine"] == "minimap2"
        or (cell["identity"]["requested_minimizer_algorithm"]
                != "vulkan_minimap_integer"
            and cell["identity"]["requested_chaining_algorithm"]
                != "vulkan_tiled_dynamic_programming")
        for cell in canonical_cells)
    selector_rejections = len(canonical_cells) - supported
    return {
        "ordinal": ordinal,
        "id": member_id,
        "kind": kind,
        "path": _relative(path),
        "config_sha256": _sha256(raw),
        "source_parent_path": parent["path"],
        "source_parent_config_sha256": parent["config_sha256"],
        "expected_result_cells": len(execution_cells),
        "expected_raw_records": len(execution_cells) * 5,
        "repetitions_per_cell": 5,
        "stable_keys": keys,
        "expected_stable_keys_sha256": _stable_keys_sha256(keys),
        "allowed_nonblocking_rule_names": rules,
        "declared_metric_names": metrics,
        "expected_all_records_disposition": expected_disposition,
        "planned_supported_cells": supported,
        "planned_selector_rejection_cells": selector_rejections,
        "canonical_cells": canonical_cells,
        "execution_cells": execution_cells,
    }


def _canonical_match_for_deadline(
        deadline_identity: dict[str, Any],
        canonical_cells: list[dict[str, Any]],
) -> dict[str, Any]:
    if (deadline_identity.get("campaign_id") != DEADLINE_V2_CAMPAIGN_ID
            or deadline_identity.get("tier") != "q1024_anchor"
            or deadline_identity.get("query_count") != 1024):
        raise GenerationError("deadline adoption is not one q1024 anchor")
    fields = {
        "test_case": deadline_identity.get("dataset"),
        "index_mode": deadline_identity.get("index_mode"),
        "engine": deadline_identity.get("engine"),
        "bioseq_index_name": deadline_identity.get("bioseq_index_name"),
        "requested_minimizer_algorithm": deadline_identity.get(
            "requested_minimizer_algorithm"),
        "requested_chaining_algorithm": deadline_identity.get(
            "requested_chaining_algorithm"),
        "requested_base_alignment_algorithm": deadline_identity.get(
            "requested_base_alignment_algorithm"),
    }
    matches = []
    for cell in canonical_cells:
        identity = cell["identity"]
        if (cell["test_case"] == fields["test_case"]
                and all(identity[key] == value for key, value in fields.items()
                        if key != "test_case")):
            matches.append(cell)
    if len(matches) != 1:
        raise GenerationError(
            f"deadline adoption maps to {len(matches)} canonical cells")
    return copy.deepcopy(matches[0])


def _singleton_by_key(
        singleton_manifest: dict[str, Any], stable_key: str) -> dict[str, Any]:
    matches = [
        child for child in singleton_manifest["children"]
        if child.get("stable_key") == stable_key]
    if len(matches) != 1:
        raise GenerationError("canonical singleton lookup is not unique")
    return matches[0]


def _singleton_summary(child: dict[str, Any], role: str) -> dict[str, Any]:
    return {
        "role": role,
        "ordinal": child["ordinal"],
        "path": child["path"],
        "config_sha256": child["config_sha256"],
        "stable_key": child["stable_key"],
        "stable_key_sha256": child["stable_key_sha256"],
    }


def _find_canonical_cell(
        canonical_cells: list[dict[str, Any]], **identity_fields: Any
) -> dict[str, Any]:
    matches = [
        cell for cell in canonical_cells
        if all(cell["identity"].get(key) == value
               for key, value in identity_fields.items())]
    if len(matches) != 1:
        raise GenerationError(
            f"canonical identity lookup found {len(matches)} cells")
    return matches[0]


def _generate() -> tuple[dict[Path, bytes], bytes]:
    contract, contract_raw = _read_exact(
        CANONICAL_CONTRACT_PATH, CANONICAL_CONTRACT_SHA256,
        "canonical campaign contract")
    singleton_manifest, singleton_raw = _read_exact(
        SINGLETON_MANIFEST_PATH, SINGLETON_MANIFEST_SHA256,
        "q1024 singleton manifest")
    deadline_manifest, deadline_raw = _read_exact(
        DEADLINE_V2_MANIFEST_PATH, DEADLINE_V2_MANIFEST_SHA256,
        "deadline-v2 manifest")
    campaign = contract.get("campaign", {})
    ownership = contract.get("shard_ownership", {})
    entries = contract.get("configs")
    if (campaign.get("campaign_id") != canonical.CAMPAIGN_ID
            or campaign.get("campaign_digest")
                != canonical.EXPECTED_CAMPAIGN_DIGEST
            or campaign.get("expected_result_cells") != 1296
            or ownership.get("stable_keys_sha256") is None
            or not isinstance(entries, list)):
        raise GenerationError("canonical campaign identity/cardinality changed")
    monoliths = [entry for entry in entries if entry.get("role") == "monolithic"]
    if len(monoliths) != 1 or len(monoliths[0].get("cells", [])) != 1296:
        raise GenerationError("canonical monolith membership changed")
    canonical_cells = monoliths[0]["cells"]
    canonical_by_key = {cell["stable_key"]: cell for cell in canonical_cells}
    if (len(canonical_by_key) != 1296
            or _stable_keys_sha256(set(canonical_by_key))
                != ownership["stable_keys_sha256"]):
        raise GenerationError("canonical stable-key union changed")

    outputs: dict[Path, bytes] = {}
    members: list[dict[str, Any]] = []

    def add_generated(
            member_id: str,
            kind: str,
            filename: str,
            parent: dict[str, Any],
            config: dict[str, Any]) -> None:
        path = OUTPUT_DIRECTORY / filename
        raw = _render(config)
        outputs[path] = raw
        members.append(_member(
            ordinal=len(members), member_id=member_id, kind=kind,
            path=path, raw=raw, config=config, parent=parent,
            canonical_by_key=canonical_by_key))

    def add_parent(member_id: str, parent: dict[str, Any]) -> None:
        config, raw = _read_parent_config(parent)
        members.append(_member(
            ordinal=len(members), member_id=member_id,
            kind="canonical_parent", path=REPOSITORY_ROOT / parent["path"],
            raw=raw, config=config, parent=parent,
            canonical_by_key=canonical_by_key))

    for dataset in MINIMAP_PAIR_DATASETS:
        parent = _canonical_parent(
            entries, test_case=dataset, shard_kind="no_index_baselines")
        slug = dataset.lower()
        add_generated(
            f"minimap2_{slug}_both_modes",
            "generated_minimap2_pair",
            f"{PACKED_FILENAME_PREFIX}{slug}_minimap2_both_modes.json",
            parent,
            _minimap_pair_config(parent),
        )

    scalar_mpf = _canonical_parent(
        entries, test_case="M2_D4_K15_W10", shard_kind="persisted_index",
        minimizer_index="minimizers_per_fragment",
        minimizer_algorithm="scalar")
    add_generated(
        "m2_d4_k15_w10_scalar_mpfragment_residual_after_s058",
        "generated_persisted_residual",
        f"{PACKED_FILENAME_PREFIX}m2_d4_k15_w10_just_index_"
        "scalar_mpfragment_residual_after_s058.json",
        scalar_mpf,
        _residual_config(scalar_mpf),
    )
    for minimizer in SUPPORTED_MINIMIZERS[1:]:
        parent = _canonical_parent(
            entries, test_case="M2_D4_K15_W10",
            shard_kind="persisted_index",
            minimizer_index="minimizers_per_fragment",
            minimizer_algorithm=minimizer)
        add_parent(f"m2_d4_k15_w10_{minimizer}_mpfragment", parent)

    for minimizer in SUPPORTED_MINIMIZERS:
        parent = _canonical_parent(
            entries, test_case="M4_D4_K15_W10",
            shard_kind="persisted_index",
            minimizer_index="minimizers_per_fragment",
            minimizer_algorithm=minimizer)
        add_parent(f"m4_d4_k15_w10_{minimizer}_mpfragment", parent)

    scalar_mps = _canonical_parent(
        entries, test_case="M2_D4_K15_W10", shard_kind="persisted_index",
        minimizer_index="minimizers_per_sequence",
        minimizer_algorithm="scalar")
    add_generated(
        "m2_d4_k15_w10_scalar_mpsequence_residual_after_0664",
        "generated_persisted_residual",
        f"{PACKED_FILENAME_PREFIX}m2_d4_k15_w10_just_index_"
        "scalar_mpsequence_residual_after_0664.json",
        scalar_mps,
        _residual_config(scalar_mps),
    )
    for minimizer in SUPPORTED_MINIMIZERS[1:]:
        parent = _canonical_parent(
            entries, test_case="M2_D4_K15_W10",
            shard_kind="persisted_index",
            minimizer_index="minimizers_per_sequence",
            minimizer_algorithm=minimizer)
        add_parent(f"m2_d4_k15_w10_{minimizer}_mpsequence", parent)

    for dataset in M4_PERSISTED_MPS_DATASETS:
        parent = _canonical_parent(
            entries, test_case=dataset, shard_kind="persisted_index",
            minimizer_index="minimizers_per_sequence",
            minimizer_algorithm="scalar")
        add_parent(f"{dataset.lower()}_scalar_mpsequence", parent)

    scalar_k20_mpf = _canonical_parent(
        entries, test_case="M2_D4_K20_W15", shard_kind="persisted_index",
        minimizer_index="minimizers_per_fragment",
        minimizer_algorithm="scalar")
    add_generated(
        "m2_d4_k20_w15_scalar_mpfragment_block_residual_after_0739",
        "generated_persisted_residual",
        f"{PACKED_FILENAME_PREFIX}m2_d4_k20_w15_just_index_"
        "scalar_mpfragment_block_aligner_residual_after_0739.json",
        scalar_k20_mpf,
        _block_aligner_residual_config(scalar_k20_mpf),
    )
    scalar_k20_mps = _canonical_parent(
        entries, test_case="M2_D4_K20_W15", shard_kind="persisted_index",
        minimizer_index="minimizers_per_sequence",
        minimizer_algorithm="scalar")
    add_parent("m2_d4_k20_w15_scalar_mpsequence", scalar_k20_mps)

    if (len(members) != 25
            or sum(member["expected_result_cells"] for member in members) != 150
            or sum(member["planned_supported_cells"] for member in members) != 115
            or sum(member["planned_selector_rejection_cells"]
                   for member in members) != 35
            or sum(member["kind"] == "canonical_parent" for member in members) != 15
            or len(outputs) != 10):
        raise GenerationError("packed member count/coverage changed")
    packed_keys = [key for member in members for key in member["stable_keys"]]
    if len(packed_keys) != len(set(packed_keys)):
        raise GenerationError("packed configs overlap")
    member_by_id = {member["id"]: member for member in members}
    singleton_by_ordinal = {
        child["ordinal"]: child for child in singleton_manifest["children"]}
    expected_mpf_residual = [
        singleton_by_ordinal[ordinal]["stable_key"]
        for ordinal in range(740, 744)]
    expected_mps_parent = [
        singleton_by_ordinal[ordinal]["stable_key"]
        for ordinal in range(744, 752)]
    preserved_mpf_ksw2 = {
        singleton_by_ordinal[ordinal]["stable_key"]
        for ordinal in range(736, 740)}
    if (member_by_id[
            "m2_d4_k20_w15_scalar_mpfragment_block_residual_after_0739"
            ]["stable_keys"] != expected_mpf_residual
            or member_by_id[
                "m2_d4_k20_w15_scalar_mpsequence"
            ]["stable_keys"] != expected_mps_parent
            or preserved_mpf_ksw2 & set(packed_keys)
            or any(
                cell["identity"]["requested_base_alignment_algorithm"]
                    == "ksw2_batched"
                for member_id in (
                    "m2_d4_k20_w15_scalar_mpfragment_block_residual_after_0739",
                    "m2_d4_k20_w15_scalar_mpsequence",
                )
                for cell in member_by_id[member_id]["canonical_cells"])):
        raise GenerationError(
            "k20 scalar residual ownership/order/exclusion changed")

    deadline_campaign = deadline_manifest.get("campaign", {})
    deadline_schedule = deadline_manifest.get("schedule")
    if (deadline_campaign.get("campaign_id") != DEADLINE_V2_CAMPAIGN_ID
            or deadline_campaign.get("campaign_digest")
                != DEADLINE_V2_CAMPAIGN_DIGEST
            or not isinstance(deadline_schedule, list)
            or len(deadline_schedule) != 60):
        raise GenerationError("deadline-v2 campaign binding changed")
    adoptions: list[dict[str, Any]] = []
    for source_ordinal in range(56, 60):
        source = deadline_schedule[source_ordinal]
        if source.get("ordinal") != source_ordinal:
            raise GenerationError("deadline-v2 adoption ordinal changed")
        canonical_cell = _canonical_match_for_deadline(
            source["identity"], canonical_cells)
        singleton = _singleton_by_key(
            singleton_manifest, canonical_cell["stable_key"])
        execution_cell = copy.deepcopy(canonical_cell)
        execution_cell["test_case"] = (
            f"{source['identity']['dataset']}_Q1024_DEADLINE")
        execution_cell["test_case_index"] = 0
        execution_cell["tuple_index"] = 0
        expected_metrics = (
            list(canonical.MINIMAP2_BASELINE_METRIC_NAMES)
            if canonical_cell["identity"]["engine"] == "minimap2"
            else list(canonical.MIXED_PERSISTED_METRIC_NAMES))
        if source.get("metric_names") != expected_metrics:
            raise GenerationError(
                f"deadline-v2 s{source_ordinal:03d} metric set changed")
        receipt_config_path = ADOPTION_RUN_RECEIPT_CONFIG_ALIASES.get(
            source_ordinal, source["path"])
        receipt_config_sha256 = source["config_sha256"]
        if source_ordinal in ADOPTION_RUN_RECEIPT_CONFIG_ALIASES:
            _, receipt_config_raw = canonical._read_json(
                REPOSITORY_ROOT / receipt_config_path)
            if _sha256(receipt_config_raw) != receipt_config_sha256:
                raise GenerationError(
                    f"deadline-v1 receipt alias s{source_ordinal:03d} "
                    "is not byte-identical to deadline-v2")
        artifacts = copy.deepcopy(
            SEALED_ADOPTION_ARTIFACTS.get(source_ordinal))
        adoptions.append({
            "ordinal": len(adoptions),
            "source_ordinal": source_ordinal,
            "source_path": source["path"],
            "source_config_sha256": source["config_sha256"],
            "source_stable_key": source["stable_key"],
            "source_stable_key_sha256": source["stable_key_sha256"],
            "canonical_stable_key": canonical_cell["stable_key"],
            "canonical_stable_key_sha256": _sha256(
                canonical_cell["stable_key"].encode("utf-8")),
            "canonical_singleton_path": singleton["path"],
            "canonical_singleton_config_sha256": singleton["config_sha256"],
            "expected_result_cells": 1,
            "expected_raw_records": 5,
            "repetitions_per_cell": 5,
            "declared_metric_names": expected_metrics,
            "expected_all_records_disposition": None,
            "canonical_cell": canonical_cell,
            "execution_cell": execution_cell,
            "run_receipt_config": {
                "path": receipt_config_path,
                "sha256": receipt_config_sha256,
            },
            "run_receipt_id": SEALED_ADOPTION_RUN_RECEIPTS.get(
                source_ordinal),
            "sealed_artifacts": artifacts,
            "ingestion_policy": (
                "exact_sealed_artifacts_plus_run_receipt"
                if artifacts is not None
                else "terminal_validated_artifacts_plus_run_receipt"),
        })
    adopted_keys = [entry["canonical_stable_key"] for entry in adoptions]
    if (len(set(adopted_keys)) != 4
            or set(adopted_keys) & set(packed_keys)):
        raise GenerationError("deadline-v2 adoptions overlap packed ownership")

    mpf_block = _find_canonical_cell(
        canonical_cells, dataset="matrix_2gbp_dna4_k15_w10",
        index_mode="just_index", engine="bioseq",
        bioseq_index_name="minimizers_per_fragment",
        requested_minimizer_algorithm="scalar",
        requested_chaining_algorithm="scalar_minimap2",
        requested_base_alignment_algorithm="block_aligner")
    mps_probe = _find_canonical_cell(
        canonical_cells, dataset="matrix_2gbp_dna4_k15_w10",
        index_mode="just_index", engine="bioseq",
        bioseq_index_name="minimizers_per_sequence",
        requested_minimizer_algorithm="scalar",
        requested_chaining_algorithm="scalar_minimap2",
        requested_base_alignment_algorithm="ksw2")
    mps_block = _find_canonical_cell(
        canonical_cells, dataset="matrix_2gbp_dna4_k15_w10",
        index_mode="just_index", engine="bioseq",
        bioseq_index_name="minimizers_per_sequence",
        requested_minimizer_algorithm="scalar",
        requested_chaining_algorithm="scalar_minimap2",
        requested_base_alignment_algorithm="block_aligner")
    retained_singletons = [
        _singleton_summary(
            _singleton_by_key(singleton_manifest, mps_probe["stable_key"]),
            "first_mps_probe"),
        _singleton_summary(
            _singleton_by_key(singleton_manifest, mpf_block["stable_key"]),
            "mpfragment_scalar_block_residual"),
        _singleton_summary(
            _singleton_by_key(singleton_manifest, mps_block["stable_key"]),
            "mpsequence_scalar_block_residual"),
    ]
    retained_keys = [entry["stable_key"] for entry in retained_singletons]
    if (len(set(retained_keys)) != 3
            or set(retained_keys) & (set(packed_keys) | set(adopted_keys))):
        raise GenerationError("retained singleton schedule overlaps replacement owners")

    optional_paths = {
        "seven_day_no_index_k20": (
            "bench/e2e_algorithm_matrix_shards/"
            "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
            "0150_c9d4c0fd1968374d.json"),
        "stretch_no_index_k15": (
            "bench/e2e_algorithm_matrix_shards/"
            "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
            "0068_2496323c88e8add2.json"),
    }
    children_by_path = {
        child["path"]: child for child in singleton_manifest["children"]}
    optional_singletons = []
    for role, path in optional_paths.items():
        child = children_by_path.get(path)
        if child is None:
            raise GenerationError(f"optional singleton is no longer sealed: {path}")
        optional_singletons.append(_singleton_summary(child, role))

    superseded_keys = set(packed_keys) | set(adopted_keys)
    fallback_keys = set(canonical_by_key) - superseded_keys
    scheduled_keys = superseded_keys | set(retained_keys)
    if (len(superseded_keys) != 154
            or len(fallback_keys) != 1142
            or len(scheduled_keys) != 157
            or superseded_keys & fallback_keys
            or superseded_keys | fallback_keys != set(canonical_by_key)
            or not set(retained_keys) <= fallback_keys):
        raise GenerationError("hybrid singleton replacement union changed")

    run_order = [
        {"step": 1, "action": "ingest_adopted_deadline_v2",
         "source_ordinals": [56, 57, 58, 59]},
        {"step": 2, "action": "run_singleton",
         "path": retained_singletons[0]["path"]},
    ]
    step = 3
    for dataset in MINIMAP_PAIR_DATASETS:
        member = member_by_id[f"minimap2_{dataset.lower()}_both_modes"]
        run_order.append({"step": step, "action": "run_packed",
                          "path": member["path"]})
        step += 1
    mpf_order = [
        "m2_d4_k15_w10_scalar_mpfragment_residual_after_s058",
        "m2_d4_k15_w10_bounded_nthash_simd_mpfragment",
        "m2_d4_k15_w10_simd_minimap2_reference_mpfragment",
        "m2_d4_k15_w10_two_stack_nthash_mpfragment",
    ]
    run_order.append({"step": step, "action": "run_packed",
                      "path": member_by_id[mpf_order[0]]["path"]})
    step += 1
    run_order.append({"step": step, "action": "run_singleton",
                      "path": retained_singletons[1]["path"]})
    step += 1
    for member_id in mpf_order[1:]:
        run_order.append({"step": step, "action": "run_packed",
                          "path": member_by_id[member_id]["path"]})
        step += 1
    for minimizer in SUPPORTED_MINIMIZERS:
        member = member_by_id[
            f"m4_d4_k15_w10_{minimizer}_mpfragment"]
        run_order.append({"step": step, "action": "run_packed",
                          "path": member["path"]})
        step += 1
    mps_order = [
        "m2_d4_k15_w10_scalar_mpsequence_residual_after_0664",
        "m2_d4_k15_w10_bounded_nthash_simd_mpsequence",
        "m2_d4_k15_w10_simd_minimap2_reference_mpsequence",
        "m2_d4_k15_w10_two_stack_nthash_mpsequence",
    ]
    run_order.append({"step": step, "action": "run_packed",
                      "path": member_by_id[mps_order[0]]["path"]})
    step += 1
    run_order.append({"step": step, "action": "run_singleton",
                      "path": retained_singletons[2]["path"]})
    step += 1
    for member_id in mps_order[1:]:
        run_order.append({"step": step, "action": "run_packed",
                          "path": member_by_id[member_id]["path"]})
        step += 1
    m4_mps_order = [
        f"{dataset.lower()}_scalar_mpsequence"
        for dataset in M4_PERSISTED_MPS_DATASETS
    ]
    for member_id in m4_mps_order:
        run_order.append({
            "step": step,
            "action": "run_packed",
            "gate": "disk_preflight",
            "path": member_by_id[member_id]["path"],
        })
        step += 1
    for member_id in (
            "m2_d4_k20_w15_scalar_mpfragment_block_residual_after_0739",
            "m2_d4_k20_w15_scalar_mpsequence"):
        run_order.append({
            "step": step,
            "action": "run_packed",
            "path": member_by_id[member_id]["path"],
        })
        step += 1
    run_order.append({
        "step": step,
        "action": "optional_seven_day_singleton",
        "path": optional_singletons[0]["path"],
    })

    execution_contract = {
        "authority_mode": "hybrid_exact_disjoint",
        "packed_config_count": 25,
        "existing_canonical_parent_config_count": 15,
        "generated_packed_config_count": 10,
        "packed_result_cells": 150,
        "packed_raw_records": 750,
        "adopted_config_count": 4,
        "adopted_result_cells": 4,
        "adopted_raw_records": 20,
        "superseded_singleton_cells": 154,
        "singleton_fallback_cells": 1142,
        "hybrid_execution_config_count": 1171,
        "canonical_result_cells": 1296,
        "canonical_raw_records": 6480,
        "repetitions_per_cell": 5,
        "repeat_indices": list(range(5)),
        "retained_scheduled_singletons": 3,
        "scheduled_result_cells": 157,
        "scheduled_supported_cells": 122,
        "scheduled_selector_rejection_cells": 35,
        "exact_nonoverlapping": True,
        "canonical_union_exact": True,
        "duplicate_stable_key_repeat_permitted": False,
        "canonical_stable_keys_sha256": ownership["stable_keys_sha256"],
        "packed_stable_keys_sha256": _stable_keys_sha256(packed_keys),
        "adopted_stable_keys_sha256": _stable_keys_sha256(adopted_keys),
        "superseded_stable_keys_sha256": _stable_keys_sha256(superseded_keys),
        "singleton_fallback_stable_keys_sha256": _stable_keys_sha256(fallback_keys),
        "scheduled_stable_keys_sha256": _stable_keys_sha256(scheduled_keys),
        "prefilter_enabled": False,
        "pipeline_mode": "task_parallel",
        "cold_no_index_persistence": "off",
        "cigar_projection": True,
        "mapq_projection": True,
        "base_alignment_algorithms": ["ksw2", "block_aligner"],
        "excluded_base_alignment_algorithms": ["ksw2_batched"],
        "q1_or_q8_membership_permitted": False,
        "generated_path_prefix": PACKED_PATH_PREFIX,
        "generated_filename_prefix": PACKED_FILENAME_PREFIX,
        "deterministic_first_ingestion": adoptions[0]["source_path"],
        "partition_digest_algorithm": (
            "sha256-canonical-json-without-partition_digest"),
    }
    manifest: dict[str, Any] = {
        "schema": "bioseq.e2e.algorithm-matrix-q1024-packed-partition",
        "version": 1,
        "campaign_binding": {
            "campaign_id": campaign["campaign_id"],
            "campaign_digest": campaign["campaign_digest"],
            "canonical_manifest": _relative(CANONICAL_CONTRACT_PATH),
            "canonical_manifest_sha256": _sha256(contract_raw),
            "singleton_manifest": _relative(SINGLETON_MANIFEST_PATH),
            "singleton_manifest_sha256": _sha256(singleton_raw),
            "deadline_v2_manifest": _relative(DEADLINE_V2_MANIFEST_PATH),
            "deadline_v2_manifest_sha256": _sha256(deadline_raw),
            "deadline_v2_campaign_id": DEADLINE_V2_CAMPAIGN_ID,
            "deadline_v2_campaign_digest": DEADLINE_V2_CAMPAIGN_DIGEST,
        },
        "execution_contract": execution_contract,
        "packed_members": members,
        "adoptions": adoptions,
        "retained_singletons": retained_singletons,
        "optional_singletons": optional_singletons,
        "execution_plan": {
            "status": "planned_not_executed",
            "adoption_ingestion_order": [56, 57, 58, 59],
            "run_order": run_order,
            "six_day": {
                "base_wall_hours": 100,
                "guarded_wall_hours": 126,
                "new_supported_cells": 85,
                "supported_cells_including_adoptions": 89,
                "remaining_supported_cells": 215,
            },
            "seven_day": {
                "base_wall_hours": 131,
                "guarded_wall_hours": 164,
                "new_supported_cells": 86,
                "supported_cells_including_adoptions": 90,
                "remaining_supported_cells": 214,
                "extension_singleton": optional_singletons[0]["path"],
            },
            "stretch": {
                "base_wall_hours": 161,
                "minimum_guarded_hours_before_launch": 39,
                "supported_cells_including_adoptions": 91,
                "extension_singleton": optional_singletons[1]["path"],
            },
            "runtime_risks": {
                "two_stack_scheduled_supported_cells": 18,
                "two_stack_failure_mode": "prior q16 PostgreSQL connection loss",
                "mps_first_probe_required": retained_singletons[0]["path"],
            },
            "disk_preflight": {
                "observed_free_kib_checkpoint": 17028108,
                "required_free_gib_for_target_ge_4gbp_mps": 50,
                "required_filesystems": ["/tmp", "postgresql_data"],
                "m4_just_index_mps_supported_cells_blocked": 24,
                "gated_packed_configs": [
                    member_by_id[member_id]["path"]
                    for member_id in m4_mps_order
                ],
                "projected_seven_day_evidence_gib": 5.3,
                "cleanup_authorized": False,
            },
            "remaining_scope_after_six_day": {
                "m4_just_index_mps": 24,
                "no_index": 191,
                "total_supported_cells": 215,
            },
            "remaining_scope_after_seven_day": {
                "m4_just_index_mps": 24,
                "no_index": 190,
                "total_supported_cells": 214,
            },
        },
    }
    digest_payload = copy.deepcopy(manifest)
    manifest["execution_contract"]["partition_digest"] = _sha256(
        _canonical_bytes(digest_payload))
    return outputs, _render(manifest)


def _atomic_write(path: Path, raw: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(raw)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--write", action="store_true",
        help="atomically write the exact generated configs and manifest")
    return parser.parse_args()


def main() -> int:
    args = _arguments()
    try:
        outputs, manifest_raw = _generate()
        expected = {**outputs, OUTPUT_MANIFEST_PATH: manifest_raw}
        if args.write:
            for path, raw in expected.items():
                _atomic_write(path, raw)
        else:
            errors = []
            for path, raw in expected.items():
                try:
                    actual = path.read_bytes()
                except OSError as exc:
                    errors.append(f"missing generated output {path}: {exc}")
                    continue
                if actual != raw:
                    errors.append(f"generated output differs: {path}")
            if errors:
                raise GenerationError("; ".join(errors))
    except (GenerationError, canonical.GenerationError, OSError, ValueError) as exc:
        print(f"packed q1024 generation failed: {exc}", file=os.sys.stderr)
        return 2
    print(
        "q1024 packed partition current: "
        f"{len(outputs)} generated configs; "
        f"manifest_sha256={_sha256(manifest_raw)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
