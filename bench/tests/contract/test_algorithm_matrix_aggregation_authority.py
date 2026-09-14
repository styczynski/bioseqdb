#!/usr/bin/env python3
"""Contract checks for algorithm-matrix partitions and synthetic append files.

The current authority is validated read-only before deriving synthetic migration
inputs from its sealed historical rows. Tests write only temporary copies.
"""

from __future__ import annotations

import base64
import contextlib
import copy
import importlib.util
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest import mock


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
REVISION4_EMPTY_LEDGER_FIXTURE = (
    Path(__file__).resolve().parent
    / "fixtures/q1024_revision4_empty_ledger.json"
)
AGGREGATOR_PATH = REPOSITORY_ROOT / "bench/aggregate_e2e_algorithm_matrix.py"
SPEC = importlib.util.spec_from_file_location("algorithm_matrix_aggregator", AGGREGATOR_PATH)
assert SPEC is not None and SPEC.loader is not None
aggregator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(aggregator)
GENERATOR_PATH = REPOSITORY_ROOT / "bench/generate_e2e_algorithm_matrix_shards.py"
GENERATOR_SPEC = importlib.util.spec_from_file_location(
    "algorithm_matrix_shard_generator", GENERATOR_PATH)
assert GENERATOR_SPEC is not None and GENERATOR_SPEC.loader is not None
generator = importlib.util.module_from_spec(GENERATOR_SPEC)
GENERATOR_SPEC.loader.exec_module(generator)


def _revision4_empty_ledger():
    return json.loads(
        REVISION4_EMPTY_LEDGER_FIXTURE.read_text(encoding="utf-8"))


class AlgorithmMatrixAggregationAuthorityTests(unittest.TestCase):
    _synthetic_observed_manifest_cache = None
    _synthetic_q8_evidence_cache = None
    _synthetic_approved_authority_cache = None
    _legacy_approved_authority_cache = None
    _current_approved_authority_cache = None

    @classmethod
    def setUpClass(cls):
        """Keep migration fixtures independent of later approved appends.

        The real publication is validated with the unmodified production seals.
        Its 66 historical rows remain pinned to the original projection digest.
        A v3 publication no longer contains the original v2 wrapper bytes, so
        only this isolated test module binds a reconstructed wrapper's own hash.
        This is synthetic migration input, never historical byte-recovery proof.
        """
        raw = (REPOSITORY_ROOT / "ALL_RESULTS_TUPLES.yaml").read_bytes()
        document = aggregator._parse_approved_tuples_yaml(raw, "current authority")
        aggregator._validate_approved_tuple_authority_document(document, raw)
        cls._current_approved_authority_cache = (copy.deepcopy(document), raw)
        if document["version"] == aggregator.APPROVED_TUPLES_LEGACY_VERSION:
            if hashlib.sha256(raw).hexdigest() != aggregator.APPROVED_TUPLES_LEGACY_SHA256:
                raise AssertionError("frozen v2 authority bytes changed")
            cls._legacy_approved_authority_cache = (document, raw)
            return

        rows = aggregator._approved_historical_v2_rows(document["approved_results"])
        if (len(rows) != 66 or aggregator._approved_projection_sha256(rows)
                != aggregator.APPROVED_TUPLES_LEGACY_PROJECTION_SHA256):
            raise AssertionError("frozen historical approved-row projection changed")
        legacy = copy.deepcopy(document)
        legacy["version"] = aggregator.APPROVED_TUPLES_LEGACY_VERSION
        legacy["purpose"] = "Synthetic v2 migration fixture over sealed historical rows."
        provenance = legacy["projection_provenance"]
        provenance["generator_version"] = aggregator.APPROVED_TUPLES_LEGACY_VERSION
        del provenance["historical_v2_preservation"]
        for field in ("median_partition_rule", "by_timing_scope"):
            del provenance["current_p50_audit"][field]
        del legacy["stable_key_contract"]["observation_identity_rule"]
        legacy["metric_definitions"].pop("timing_scope", None)
        legacy["terminology"].get("approved_row_field_guide", {}).pop("timing_scope", None)
        for field in ("current_logical_tuple_count", "current_timing_observation_count",
                      "current_query_only_observation_count", "current_query_only_run_total"):
            del legacy["run_count_expectations"][field]
        for view_name in ("corrected_user_intended", "historical_rmq_mislabeled"):
            view = legacy["high_priority_families"][view_name]
            del view["valid_run_vectors_by_timing_scope"]
            for family in view["families"]:
                del family["approved_observations_by_timing_scope"]
        protocol = legacy["append_update_protocol"]
        protocol["writer"]["version"] = 1
        protocol["canonical_serializer"]["version"] = aggregator.APPROVED_TUPLES_LEGACY_VERSION
        protocol["sealed_append_candidate"]["version"] = 1
        protocol["sealed_append_candidate"]["verification_manifest"]["version"] = 1
        aggregator._regenerate_approved_tuple_projections(legacy, rows)
        # The pinned-byte migration path intentionally retains its legacy
        # summary layout; exercise that branch using this fixture's own bytes.
        legacy["corrected_summaries"] = aggregator._approved_corrected_summaries(
            legacy, rows, legacy_layout=True)
        overall = legacy["corrected_summaries"]["overall"]
        legacy["excluded_and_nonvalid_summary"]["missing"].update({
            "cells": overall["missing_cells"],
            "planned_runs_not_recorded": overall["unrecorded_planned_runs"],
        })
        fixture_raw = aggregator._canonical_approved_tuples_yaml_v2(legacy)
        fixture_seal = mock.patch.object(
            aggregator, "APPROVED_TUPLES_LEGACY_SHA256",
            hashlib.sha256(fixture_raw).hexdigest())
        fixture_seal.start()
        cls.addClassCleanup(fixture_seal.stop)
        aggregator._validate_approved_tuple_authority_document(legacy, fixture_raw)
        cls._legacy_approved_authority_cache = (legacy, fixture_raw)

    @classmethod
    @contextlib.contextmanager
    def _load_q8_contract_with_synthetic_source_evidence(cls):
        """Load q8 against one hermetic byte snapshot and synthetic worker logs."""
        if cls._synthetic_q8_evidence_cache is None:
            proof, _ = aggregator._read_json_file(
                aggregator.Q8_QUERY_PREFIX_PROOF_PATH,
                "q8 query-prefix proof test fixture")
            manifest, _ = aggregator._read_json_file(
                aggregator.Q8_RESCUE_MANIFEST_PATH,
                "q8 rescue manifest test fixture")
            ledger_path = REPOSITORY_ROOT / "Q8_RESULTS.yaml"
            ledger, ledger_raw = aggregator._read_json_file(
                ledger_path, "q8 results test fixture")
            proof = copy.deepcopy(proof)
            manifest = copy.deepcopy(manifest)

            source_path = REVISION4_EMPTY_LEDGER_FIXTURE
            source_raw = source_path.read_bytes()
            source_lines = source_raw.decode("utf-8").splitlines()
            synthetic_source_evidence = {
                "path": source_path.relative_to(REPOSITORY_ROOT).as_posix(),
                "sha256": hashlib.sha256(source_raw).hexdigest(),
                "evidence": [{
                    "line": 2,
                    "statement": source_lines[1],
                }],
            }
            query_contract = proof["query_generation_contract"]
            query_contract["benchmark_loader"] = copy.deepcopy(
                synthetic_source_evidence)
            query_contract["sequence_generator"] = copy.deepcopy(
                synthetic_source_evidence)
            proof["matrix_and_repeat_contract"]["sealed_matrix_evidence"] = (
                copy.deepcopy(synthetic_source_evidence))

            external_bytes = {}
            manifest["purpose"] = (
                "Synthetic in-memory q8 rescue authority for contract tests; "
                "repository report and raw-sample bytes are unchanged.")
            for ordinal, child in enumerate(manifest["children"]):
                worker_log = child["worker_log"]
                worker_log_path = Path(worker_log["path"])
                worker_log_raw = (
                    "synthetic q8 rescue worker-log fixture\n"
                    f"ordinal={ordinal}\n"
                ).encode("utf-8")
                external_bytes[worker_log_path] = worker_log_raw
                worker_log["sha256"] = hashlib.sha256(
                    worker_log_raw).hexdigest()
                worker_log["size"] = len(worker_log_raw)

            manifest_raw = (json.dumps(
                manifest, indent=2, ensure_ascii=True, allow_nan=False) + "\n").encode(
                    "utf-8")
            proof["rescue_execution_authority"]["manifest_sha256"] = (
                hashlib.sha256(manifest_raw).hexdigest())
            proof_raw = (json.dumps(
                proof, indent=2, ensure_ascii=True, allow_nan=False) + "\n").encode(
                    "utf-8")

            repository_paths = {
                aggregator.CAMPAIGN_CONTRACT_PATH,
                aggregator.Q8_CONFIG_PATH,
                aggregator.Q1024_CONTRACT_PATH,
                aggregator.Q1024_CONFIG_PATH,
                aggregator.Q8_QUERY_PREFIX_PROOF_PATH,
                aggregator.Q8_RESCUE_MANIFEST_PATH,
                ledger_path,
                source_path,
                REPOSITORY_ROOT / manifest["parent"]["path"],
            }
            for child in manifest["children"]:
                repository_paths.update({
                    REPOSITORY_ROOT / child["path"],
                    REPOSITORY_ROOT / child["report"]["path"],
                    REPOSITORY_ROOT / child["report"]["raw_sidecar"]["path"],
                })
            for entry in ledger["aggregation_state"]["processed_reports"]:
                repository_paths.update({
                    REPOSITORY_ROOT / entry["campaign_contract"],
                    REPOSITORY_ROOT / entry["config"],
                    REPOSITORY_ROOT / entry["report"],
                    REPOSITORY_ROOT / entry["raw_sidecar"],
                })
            repository_bytes = {
                path: aggregator._read_regular(
                    path, f"synthetic q8 dependency {path.name}")
                for path in repository_paths
            }
            repository_bytes.update({
                aggregator.Q8_QUERY_PREFIX_PROOF_PATH: proof_raw,
                aggregator.Q8_RESCUE_MANIFEST_PATH: manifest_raw,
                ledger_path: ledger_raw,
                source_path: source_raw,
            })
            cls._synthetic_q8_evidence_cache = (
                repository_bytes, external_bytes)

        repository_bytes, external_bytes = cls._synthetic_q8_evidence_cache

        def read_repository_evidence(path, label):
            exact_path = Path(path)
            if exact_path not in repository_bytes:
                raise AssertionError(
                    f"synthetic q8 fixture omitted repository dependency: "
                    f"{label} {exact_path}")
            return repository_bytes[exact_path]

        def read_external_evidence(path, label):
            exact_path = Path(path)
            if exact_path not in external_bytes:
                raise AssertionError(
                    f"synthetic q8 fixture omitted external dependency: "
                    f"{label} {exact_path}")
            return external_bytes[exact_path]

        with mock.patch.object(
                aggregator, "_read_regular",
                side_effect=read_repository_evidence), mock.patch.object(
                    aggregator, "_read_external_regular",
                    side_effect=read_external_evidence):
            yield aggregator._load_campaign_contract(), repository_bytes

    @staticmethod
    def _approved_tuple_row(
            *, target=2, alphabet="dna4", kmer=15, window=10,
            mode="no_index", p50_a=50, p50_b=40,
            timing_scope=aggregator.APPROVED_TUPLES_QUERY_TIMING_SCOPE):
        dataset = f"matrix_{target}gbp_{alphabet}_k{kmer}_w{window}"
        requested = {
            "minimizer": "two_stack_nthash",
            "chaining": "scalar_minimap2",
            "base_alignment": "ksw2",
        }
        identity = {
            "campaign_id": aggregator.Q1024_CAMPAIGN_ID,
            "dataset": dataset,
            "alphabet": alphabet,
            "target_size_gbp": target,
            "kmer_size": kmer,
            "window_size": window,
            "index_mode": mode,
            "engine": "bioseq",
            "bioseq_index_name": "minimizers_per_sequence",
            "requested_minimizer_algorithm": requested["minimizer"],
            "requested_chaining_algorithm": requested["chaining"],
            "requested_base_alignment_algorithm": requested["base_alignment"],
        }
        return {
            "stable_key_schema": "bioseq-e2e-cell-v2",
            "stable_key": aggregator._stable_key_for_identity(identity),
            "timing_scope": timing_scope,
            "campaign_id": aggregator.Q1024_CAMPAIGN_ID,
            "dataset": dataset,
            "target_size_gbp": target,
            "alphabet": alphabet,
            "kmer_size": kmer,
            "window_size": window,
            "query_count": 1024,
            "mode": mode,
            "pipeline": "bioseq",
            "pipeline_mode": "task_parallel",
            "index_type": "minimizers_per_sequence",
            "requested_algorithms": requested,
            "observed_algorithms": copy.deepcopy(requested),
            "effective_path": "two_stack_nthash -> scalar_minimap2 -> ksw2",
            "execution_conformance": "exact_requested_path",
            "comparison_eligibility": {
                "requested_path": True,
                "effective_path": True,
                "scope": "requested_and_effective_path",
            },
            "prefilter": False,
            "exact_baseline_match_count_equal": True,
            "status": "accepted",
            "run_count": 5,
            "metrics": {"unit": "ns", "p50_a": p50_a, "p50_b": p50_b},
            "comments": None,
        }

    @staticmethod
    def _approved_family(identifier, mode, minimizer, chaining, base_alignment):
        pipeline = "minimap2" if identifier.startswith("minimap2_") else "bioseq"
        return {
            "id": identifier,
            "label": f"Synthetic {identifier}",
            "mode": mode,
            "pipeline": pipeline,
            "index_type": ("minimap2_internal" if pipeline == "minimap2"
                           else "minimizers_per_fragment"),
            "requested_algorithms": {
                "minimizer": minimizer,
                "chaining": chaining,
                "base_alignment": base_alignment,
            },
            "materialized_cells": 0,
            "recorded_runs": 0,
            "approved_cells": 0,
            "valid_runs": 0,
        }

    @classmethod
    def _approved_priority_families(cls):
        minimap = (None, None, None)
        corrected = [
            cls._approved_family("bounded_simd_dp_block_mpf_just_index",
                                 "just_index", "bounded_nthash_simd",
                                 "simd_minimap2", "block_aligner"),
            cls._approved_family("vulkan_seed_vulkan_chain_block_mpf_just_index",
                                 "just_index", "vulkan_minimap_integer",
                                 "vulkan_tiled_dynamic_programming", "block_aligner"),
            cls._approved_family("bounded_simd_dp_block_mpf_no_index",
                                 "no_index", "bounded_nthash_simd",
                                 "simd_minimap2", "block_aligner"),
            cls._approved_family("vulkan_seed_vulkan_chain_block_mpf_no_index",
                                 "no_index", "vulkan_minimap_integer",
                                 "vulkan_tiled_dynamic_programming", "block_aligner"),
            cls._approved_family("minimap2_just_index", "just_index", *minimap),
            cls._approved_family("minimap2_no_index", "no_index", *minimap),
        ]
        historical = [
            cls._approved_family("bounded_rmq_block_mpf_just_index",
                                 "just_index", "bounded_nthash_simd",
                                 "range_minimum_query", "block_aligner"),
            cls._approved_family("vulkan_seed_vulkan_chain_block_mpf_just_index",
                                 "just_index", "vulkan_minimap_integer",
                                 "vulkan_tiled_dynamic_programming", "block_aligner"),
            cls._approved_family("bounded_rmq_block_mpf_no_index",
                                 "no_index", "bounded_nthash_simd",
                                 "range_minimum_query", "block_aligner"),
            cls._approved_family("vulkan_seed_vulkan_chain_block_mpf_no_index",
                                 "no_index", "vulkan_minimap_integer",
                                 "vulkan_tiled_dynamic_programming", "block_aligner"),
            cls._approved_family("minimap2_just_index", "just_index", *minimap),
            cls._approved_family("minimap2_no_index", "no_index", *minimap),
        ]
        return {
            "classification": "derived_non_authoritative",
            "projection_description": "Synthetic generated priority projection.",
            "vector_metric": "valid_runs",
            "family_order_is_normative": False,
            "corrected_user_intended": {
                "simd_dp_key": "simd_minimap2",
                "valid_run_vector": [1],
                "recorded_run_vector_for_exact_family_definitions": [1],
                "families": corrected,
                "reconciliation": "synthetic",
            },
            "historical_rmq_mislabeled": {
                "historical_shorthand_that_was_wrong": "SIMD-DP",
                "actual_chaining_key": "range_minimum_query",
                "actual_human_label": "Minimap2 RMQ",
                "valid_run_vector": [1],
                "recorded_run_vector_for_exact_family_definitions": [1],
                "families": historical,
                "reconciliation": "synthetic",
                "invariant": "RMQ is never relabeled.",
            },
        }

    @classmethod
    def _legacy_approved_authority(cls):
        """Return isolated migration input, never reopen the mutable publication."""
        if cls._legacy_approved_authority_cache is None:
            raise AssertionError("migration fixture was not initialized")
        document, raw = cls._legacy_approved_authority_cache
        return copy.deepcopy(document), raw

    @classmethod
    def _synthetic_approved_authority(cls):
        if cls._synthetic_approved_authority_cache is None:
            legacy, raw = cls._legacy_approved_authority()
            cls._synthetic_approved_authority_cache = (
                aggregator._migrate_approved_tuple_authority_v3(
                    legacy, hashlib.sha256(raw).hexdigest()))
        return copy.deepcopy(cls._synthetic_approved_authority_cache)

    @classmethod
    def _current_approved_authority(cls):
        if cls._current_approved_authority_cache is None:
            raise AssertionError("current authority fixture was not initialized")
        document, raw = cls._current_approved_authority_cache
        return copy.deepcopy(document), raw

    @classmethod
    def _synthetic_observed_manifest(cls, _authority):
        """Return one cached complete manifest over the sealed q1024 source."""
        if cls._synthetic_observed_manifest_cache is not None:
            return cls._synthetic_observed_manifest_cache
        canonical_manifest_raw = aggregator.Q1024_CONTRACT_PATH.read_bytes()
        canonical_config_raw = aggregator.Q1024_CONFIG_PATH.read_bytes()
        canonical_config = json.loads(canonical_config_raw.decode("utf-8"))
        projected_config = aggregator._expected_observed_rung_config(
            canonical_config, 1024)
        test_cases = projected_config["test_cases"]
        all_cells = aggregator._execution_descriptors_from_config(
            projected_config, aggregator.Q1024_CAMPAIGN_ID,
            aggregator.OBSERVED_RESULT_STATUS_CONTRACT, query_count=1024)
        no_index_groups = []
        for test_case in test_cases:
            no_index_groups.append([
                cell for cell in all_cells
                if cell["test_case"] == test_case
                and (cell["identity"]["engine"] == "minimap2"
                     or cell["identity"]["index_mode"] == "no_index")])
        persisted_cells = [
            cell for cell in all_cells
            if cell["identity"]["engine"] == "bioseq"
            and cell["identity"]["index_mode"] == "just_index"]
        if (len(all_cells) != 1296
                or any(len(group) != 82 for group in no_index_groups)
                or len(persisted_cells) != 640):
            raise AssertionError("synthetic observed manifest matrix is incomplete")

        config_raw = (json.dumps(
            projected_config, indent=2, ensure_ascii=True, allow_nan=False)
            + "\n").encode("utf-8")
        config_base64 = base64.urlsafe_b64encode(config_raw).decode("ascii")
        full_config = "bench/synthetic_q1024_observed_campaign.json"
        full_config_sha256 = hashlib.sha256(config_raw).hexdigest()

        def manifest_entry(path, role, shard_kind, cells, metrics, config_sha256):
            keys = [cell["stable_key"] for cell in cells]
            return {
                "path": path,
                "config_sha256": config_sha256,
                "role": role,
                "shard_kind": shard_kind,
                "expected_result_cells": len(cells),
                "expected_raw_records": len(cells) * 5,
                "expected_stable_keys_sha256": hashlib.sha256(
                    "\n".join(sorted(keys)).encode("utf-8")).hexdigest(),
                "allowed_nonblocking_rule_names": [],
                "declared_metric_names": list(metrics),
                "expected_all_records_disposition": None,
                "cells": cells,
            }

        monolithic = manifest_entry(
            full_config, "monolithic", None, all_cells,
            aggregator.FULL_DECLARED_METRIC_NAMES, full_config_sha256)
        monolithic["allowed_nonblocking_rule_names"] = (
            aggregator._config_nonblocking_rule_names(projected_config))
        shards = []
        for ordinal, cells in enumerate(no_index_groups):
            path = (
                "bench/synthetic_q1024_observed_shards/"
                f"no_index_{ordinal:02d}.json")
            shards.append(manifest_entry(
                path, "shard", "no_index_baselines", cells,
                aggregator.FULL_DECLARED_METRIC_NAMES,
                hashlib.sha256(path.encode("ascii")).hexdigest()))
        persisted_cells.sort(key=lambda cell: cell["stable_key"])
        for ordinal in range(80):
            path = (
                "bench/synthetic_q1024_observed_shards/"
                f"persisted_{ordinal:02d}.json")
            cells = persisted_cells[ordinal * 8:(ordinal + 1) * 8]
            shards.append(manifest_entry(
                path, "shard", "persisted_index", cells,
                aggregator.MIXED_PERSISTED_METRIC_NAMES,
                hashlib.sha256(path.encode("ascii")).hexdigest()))

        campaign = {
            "campaign_id": aggregator.Q1024_CAMPAIGN_ID,
            "campaign_digest_algorithm": (
                "sha256-canonical-json-without-campaign_digest"),
            "stable_key_schema": "bioseq-e2e-cell-v2",
            "stable_key_fields": list(aggregator.STABLE_KEY_FIELDS),
            "repetitions_per_cell": 5,
            "expected_result_cells": 1296,
            "expected_raw_records": 6480,
            "required_shard_configs": 88,
            "publication_status_contract": {
                "status_source": "observed",
                "minimap2": ["accepted"],
                "bioseq": ["accepted"],
                "rejected_dispositions": ["rejected_setup", "rejected_sql"],
                "nonpublishable_statuses": ["rejected", "invalid", "pending"],
            },
            "query_count": 1024,
            "result_status_contract": aggregator.OBSERVED_RESULT_STATUS_CONTRACT,
            "execution_contract_id": (
                aggregator._observed_rung_execution_contract_id(1024)),
        }
        manifest = {
            "schema": aggregator.CAMPAIGN_CONTRACT_SCHEMA,
            "version": aggregator.OBSERVED_RUNG_CAMPAIGN_CONTRACT_VERSION,
            "campaign": campaign,
            "source": {
                "full_config": full_config,
                "full_config_sha256": full_config_sha256,
                "generator": "bench/generate_e2e_algorithm_matrix_shards.py",
            },
            "configs": [monolithic, *shards],
            "shard_ownership": {
                "exact_nonoverlapping": True,
                "shard_paths": [shard["path"] for shard in shards],
                "stable_keys_sha256": hashlib.sha256(
                    "\n".join(sorted(cell["stable_key"] for cell in all_cells)).encode(
                        "utf-8")).hexdigest(),
                "resume_semantics": "Synthetic exact observed q1024 ownership.",
            },
            "rung": {
                "schema": "bioseq.e2e.algorithm-matrix-observed-rung",
                "version": 1,
                "query_count": 1024,
                "ladder_query_counts": list(aggregator.RUNG_QUERY_COUNTS),
                "predecessor_query_count": 512,
                "predecessor_execution_contract_id": (
                    aggregator._observed_rung_execution_contract_id(512)),
                "query_ordinal_interval": {"begin": 0, "end_exclusive": 1024},
                "projection": "exact_fixed_seed_ordered_prefix_of_q1024",
                "fixture_projection_sha256": (
                    aggregator._rung_fixture_projection_sha256(
                        projected_config, 1024)),
                "matrix_contract": {
                    "all_axes_preserved": True,
                    "repetitions_per_cell": 5,
                    "prefilter_enabled": False,
                    "base_alignment_algorithms": ["ksw2", "block_aligner"],
                    "excluded_base_alignment_algorithms": ["ksw2_batched"],
                },
                "canonical_campaign_binding": {
                    "campaign_id": aggregator.Q1024_CAMPAIGN_ID,
                    "campaign_digest": aggregator.Q1024_CAMPAIGN_DIGEST,
                    "manifest": "bench/bench_e2e_algorithm_matrix_campaign_contract.json",
                    "manifest_sha256": hashlib.sha256(
                        canonical_manifest_raw).hexdigest(),
                    "config": "bench/bench_e2e_algorithm_matrix_campaign.json",
                    "config_sha256": hashlib.sha256(
                        canonical_config_raw).hexdigest(),
                },
            },
        }
        campaign["campaign_digest"] = hashlib.sha256(
            aggregator._canonical_bytes(manifest)).hexdigest()
        raw = (json.dumps(
            manifest, indent=2, ensure_ascii=True, allow_nan=False) + "\n").encode("utf-8")
        cls._synthetic_observed_manifest_cache = (
            base64.urlsafe_b64encode(raw).decode("ascii"),
            hashlib.sha256(raw).hexdigest(),
            config_base64,
            full_config_sha256,
            base64.urlsafe_b64encode(canonical_manifest_raw).decode("ascii"),
            base64.urlsafe_b64encode(canonical_config_raw).decode("ascii"),
        )
        return cls._synthetic_observed_manifest_cache

    @staticmethod
    def _candidate_execution_manifest(candidate):
        encoded = candidate["verification_manifest"]["proofs"]["config"][
            "payload"]["execution_contract"]["manifest_base64"]
        return json.loads(base64.urlsafe_b64decode(encoded).decode("utf-8"))

    @staticmethod
    def _replace_candidate_execution_manifest(candidate, manifest):
        digest_payload = copy.deepcopy(manifest)
        del digest_payload["campaign"]["campaign_digest"]
        manifest["campaign"]["campaign_digest"] = hashlib.sha256(
            aggregator._canonical_bytes(digest_payload)).hexdigest()
        raw = (json.dumps(
            manifest, indent=2, ensure_ascii=True, allow_nan=False) + "\n").encode("utf-8")
        execution = candidate["verification_manifest"]["proofs"]["config"][
            "payload"]["execution_contract"]
        execution["manifest_base64"] = base64.urlsafe_b64encode(raw).decode("ascii")
        execution["sha256"] = hashlib.sha256(raw).hexdigest()

    @staticmethod
    def _candidate_observed_config(candidate):
        encoded = candidate["verification_manifest"]["proofs"]["config"][
            "payload"]["config_base64"]
        return json.loads(base64.urlsafe_b64decode(encoded).decode("utf-8"))

    @classmethod
    def _replace_candidate_observed_config(cls, candidate, config):
        raw = (json.dumps(
            config, indent=2, ensure_ascii=True, allow_nan=False) + "\n").encode(
                "utf-8")
        digest = hashlib.sha256(raw).hexdigest()
        payload = candidate["verification_manifest"]["proofs"]["config"]["payload"]
        payload["config_base64"] = base64.urlsafe_b64encode(raw).decode("ascii")
        payload["config_sha256"] = digest
        manifest = cls._candidate_execution_manifest(candidate)
        manifest["source"]["full_config_sha256"] = digest
        monolithic = next(
            entry for entry in manifest["configs"]
            if entry["role"] == "monolithic")
        monolithic["config_sha256"] = digest
        manifest["rung"]["fixture_projection_sha256"] = (
            aggregator._rung_fixture_projection_sha256(config, 1024))
        cls._replace_candidate_execution_manifest(candidate, manifest)

    @staticmethod
    def _synthetic_approved_gate8_sql(row):
        if row["pipeline"] != "bioseq":
            return None
        applicable_cache = (
            row["mode"] == "just_index"
            and row["requested_algorithms"]["minimizer"]
                == "vulkan_minimap_integer"
        )
        repeats = []
        for repeat in range(5):
            digest = hashlib.sha256(
                f"gate8:{row['stable_key']}:{repeat}".encode("utf-8")).hexdigest()
            repeats.append({
                "repeat": repeat,
                "repeat_identity": (
                    f"{row['stable_key']}:timing_scope:{row['timing_scope']}:"
                    f"repeat:{repeat}"),
                "measurement_sql_sha256": digest,
                "pre_statement_gucs_sha256": digest,
                "explain_json_sha256": digest,
                "literal_single_statement": True,
                "complete_guc_count": 22,
                "explain_analyze_json": True,
                "custom_scan_node_count": 1,
                "custom_plan_provider": "BioseqBatchJoin",
                "chaining_algorithm": row["requested_algorithms"]["chaining"],
                "base_alignment_algorithm": row["requested_algorithms"]["base_alignment"],
                "pipeline_concurrency_mode": row["pipeline_mode"],
                "query_fixture_physical_no_index": True,
                "prefilter_activity_count": 0,
                "route_and_ordered_counts_unchanged": True,
                "cache_control_disposition":
                    "nonpublishable_control_config_changed",
                "vectorization_control_disposition":
                    "nonpublishable_control_config_changed",
                "cache_evidence": {
                    "applicability": (
                        "persisted_vulkan_seed_statement_resident"
                        if applicable_cache else
                        "not_applicable_to_selected_route"),
                    "gpu_persisted_resident_uploads":
                        1 if applicable_cache else 0,
                    "gpu_persisted_resident_cache_hits": 0,
                },
            })
        return {
            "schema": "bioseq.e2e.gate8-approved-repeats",
            "version": 1,
            "raw_payload_schema_version":
                aggregator.RAW_PAYLOAD_SCHEMA_VERSION,
            "repeat_count": 5,
            "repeats": repeats,
        }

    @classmethod
    def _synthetic_candidate(cls, authority, rows=None, authority_raw=None):
        candidate_rows = rows or [cls._approved_tuple_row(
            target=4, alphabet="dna5", kmer=20, window=15,
            mode="just_index", p50_a=110, p50_b=100)]
        timing_scope = aggregator.APPROVED_TUPLES_QUERY_TIMING_SCOPE
        if any(row["timing_scope"] != timing_scope for row in candidate_rows):
            raise AssertionError("synthetic candidates are query-only observations")
        if authority_raw is None:
            serializer = (
                aggregator._canonical_approved_tuples_yaml_v2
                if authority["version"] == aggregator.APPROVED_TUPLES_LEGACY_VERSION
                else aggregator._canonical_approved_tuples_yaml_v3)
            authority_raw = serializer(authority)
        authority_sha256 = hashlib.sha256(authority_raw).hexdigest()
        merge_base = aggregator._migrate_approved_tuple_authority_v3(
            authority, authority_sha256)
        (execution_manifest, execution_manifest_sha256,
         projected_config_base64, projected_config_sha256,
         canonical_manifest_base64, canonical_config_base64) = (
            cls._synthetic_observed_manifest(authority))
        fixture_rows = []
        for row in candidate_rows:
            if row["dataset"] not in {item["dataset"] for item in fixture_rows}:
                fixture_rows.append({
                    "fixture_id": f"synthetic-{row['dataset']}",
                    "fixture_sha256": hashlib.sha256(
                        row["dataset"].encode("ascii")).hexdigest(),
                    "dataset": row["dataset"],
                    "target_size_gbp": row["target_size_gbp"],
                    "alphabet": row["alphabet"],
                    "kmer_size": row["kmer_size"],
                    "window_size": row["window_size"],
                    "query_count": row["query_count"],
                })
        fixture_rows.sort(key=lambda item: item["dataset"])
        a_values = [100, 120, 110, 130, 90]
        unions = [10, 20, 10, 30, 10]
        chunk_rows = []
        for row in candidate_rows:
            repeats = []
            for repeat, (a_ns, union_ns) in enumerate(zip(a_values, unions)):
                vector_digest = hashlib.sha256(
                    f"{row['stable_key']}:{repeat}".encode("utf-8")).hexdigest()
                repeats.append({
                    "repeat": repeat,
                    "repeat_identity": (
                        f"{row['stable_key']}:timing_scope:{row['timing_scope']}:"
                        f"repeat:{repeat}"),
                    "query_partition": {
                        "identity": f"synthetic-q1024-{row['dataset']}",
                        "sha256": hashlib.sha256(
                            f"partition:{row['dataset']}".encode("ascii")).hexdigest(),
                        "query_count": 1024,
                    },
                    "A_ns": a_ns,
                    "union_ns": union_ns,
                    "B_ns": a_ns - union_ns,
                    "candidate_match_vector_sha256": vector_digest,
                    "baseline_match_vector_sha256": vector_digest,
                    "candidate_match_vector_count": 1024,
                    "baseline_match_vector_count": 1024,
                    "candidate_match_total": 17,
                    "baseline_match_total": 17,
                    "valid": True,
                })
            chunk_rows.append({
                "stable_key": row["stable_key"],
                "timing_scope": row["timing_scope"],
                "prior_disposition": "missing",
                "repeats": repeats,
            })
        payloads = {
            "source": {
                "timing_scope": timing_scope,
                "source_commit": "1" * 40,
                "source_graph_fingerprint": "2" * 64,
                "authorized_artifacts_sha256": "3" * 64,
            },
            "build": {
                "timing_scope": timing_scope,
                "build_receipt_id": "br_" + "4" * 32,
                "build_receipt_sha256": "5" * 64,
                "mode": "benchmark-e2e",
                "preset": "bench-e2e-opt",
                "debug": False,
                "status": "success",
                "source_commit": "1" * 40,
                "source_graph_fingerprint": "2" * 64,
                "authorized_artifacts_sha256": "3" * 64,
            },
            "fixture": {"timing_scope": timing_scope, "fixtures": fixture_rows},
            "config": {
                "timing_scope": timing_scope,
                "campaign_id": authority["campaign_identity"]["campaign_id"],
                "campaign_digest": authority["campaign_identity"]["campaign_digest"],
                "execution_contract": {
                    "identity": aggregator._observed_rung_execution_contract_id(1024),
                    "version": aggregator.OBSERVED_RUNG_CAMPAIGN_CONTRACT_VERSION,
                    "sha256": execution_manifest_sha256,
                    "result_status_contract": aggregator.OBSERVED_RESULT_STATUS_CONTRACT,
                    "manifest_base64": execution_manifest,
                    "canonical_manifest_base64": canonical_manifest_base64,
                },
                "config_sha256": projected_config_sha256,
                "config_base64": projected_config_base64,
                "canonical_config_base64": canonical_config_base64,
                "rows": [aggregator._approved_config_projection(row)
                         for row in candidate_rows],
            },
            "measurement_contract": {
                "timing_scope": timing_scope,
                "clock": "monotonic",
                "unit": "ns",
                "cigar_projection": True,
                "mapq_projection": True,
                "prefilter": False,
                "interval_receipt_complete": True,
                "equal_nanosecond_rate": True,
                "device": {"identity": "synthetic-device", "version": "1"},
                "driver": {"identity": "synthetic-driver", "version": "1"},
                "rows": [{
                    **aggregator._approved_measurement_projection(row),
                    "gate8_sql": cls._synthetic_approved_gate8_sql(row),
                } for row in candidate_rows],
            },
            "chunk_plan": {"timing_scope": timing_scope, "rows": chunk_rows},
        }
        proofs = {}
        for index, category in enumerate(aggregator.APPROVED_TUPLES_PROOF_CATEGORIES):
            payload = payloads[category]
            proofs[category] = {
                "identity": f"synthetic-{category}",
                "version": index + 1,
                "sha256": hashlib.sha256(
                    aggregator._canonical_bytes(payload)).hexdigest(),
                "payload": payload,
            }
        bindings = []
        for row in candidate_rows:
            bindings.append({
                "stable_key": row["stable_key"],
                "timing_scope": row["timing_scope"],
                "proofs": {
                    category: {
                        "identity": proofs[category]["identity"],
                        "version": proofs[category]["version"],
                    }
                    for category in aggregator.APPROVED_TUPLES_PROOF_CATEGORIES
                },
            })
        merged_rows = sorted(
            copy.deepcopy(merge_base["approved_results"] + candidate_rows),
            key=aggregator._approved_observation_sort_key)
        candidate = {
            "schema": aggregator.APPROVED_TUPLES_CANDIDATE_SCHEMA,
            "version": aggregator.APPROVED_TUPLES_CANDIDATE_VERSION,
            "authority_before_sha256": authority_sha256,
            "stable_key_schema": "bioseq-e2e-cell-v2",
            "timing_scope": timing_scope,
            "candidate_rows": copy.deepcopy(candidate_rows),
            "verification_manifest": {
                "schema": aggregator.APPROVED_TUPLES_MANIFEST_SCHEMA,
                "version": aggregator.APPROVED_TUPLES_MANIFEST_VERSION,
                "timing_scope": timing_scope,
                "proofs": proofs,
                "row_bindings": bindings,
            },
            "candidate_projection_sha256": aggregator._approved_projection_sha256(
                merged_rows),
            "candidate_envelope_sha256": "0" * 64,
        }
        cls._reseal_synthetic_candidate(candidate)
        return candidate

    @staticmethod
    def _synthetic_validated_gate8_v2_records(
            row, cell_index, payloads, fixture_projection_sha256):
        """Make validated-record-shaped evidence for private projection tests."""
        applicable_cache = (
            row["mode"] == "just_index"
            and row["requested_algorithms"]["minimizer"]
                == "vulkan_minimap_integer")
        gpu_selected = (
            row["requested_algorithms"]["minimizer"]
                == "vulkan_minimap_integer"
            or row["requested_algorithms"]["chaining"]
                == "vulkan_tiled_dynamic_programming")
        runtime_device = {
            "execution_device": "cpu_and_gpu" if gpu_selected else "cpu",
            "gpu_device_uuid": "1" * 32 if gpu_selected else "",
            "gpu_driver": "synthetic-gpu-driver" if gpu_selected else "",
        }
        target_table = f"synthetic_target_{cell_index}"
        query_table = f"synthetic_query_{cell_index}"
        absence = {
            "target_rows": row["query_count"],
            "target_cardinality_metadata_only": 1,
            "source_parent_rows_read": 0,
            "source_fragment_rows_read": 0,
            "source_decoded_bp": 0,
            "physical_index_relations": 0,
            "persisted_index_rows": 0,
            "generation_objects": 0,
        }
        query_fixture = {
            "table": query_table,
            "access_method": "bioseq",
            "row_count": row["query_count"],
            "distinct_query_id_count": row["query_count"],
            "minimum_query_id": 0,
            "maximum_query_id": row["query_count"] - 1,
            "policies": {
                "minimizers_per_fragment": "UPDATE_MANUALLY",
                "minimizers_per_sequence": "UPDATE_MANUALLY",
            },
            "physical_absence": {
                "minimizers_per_fragment": copy.deepcopy(absence),
                "minimizers_per_sequence": copy.deepcopy(absence),
            },
        }
        route = {"prefilter_activity_count": 0}
        counts = [1] * row["query_count"]
        gucs = {
            "sql": "SELECT synthetic_22_gucs;",
            "json_text": "synthetic-22-gucs",
            "values": {f"synthetic.guc.{index}": "on" for index in range(22)},
        }
        cache_evidence = {
            "applicability": (
                "persisted_vulkan_seed_statement_resident"
                if applicable_cache else "not_applicable_to_selected_route"),
            "not_applicable_reason": (
                None if applicable_cache else
                "no_index_uses_transient_target_construction"
                if row["mode"] == "no_index" else
                "selected_minimizer_is_not_vulkan_seed"),
            "gpu_persisted_resident_uploads": 1 if applicable_cache else 0,
            "gpu_persisted_resident_cache_hits": 0,
        }
        measurement = {
            "boundary": "framework_iteration_measurement_window",
            "operation": "single_libpq_PQexec_mmsearch_statement",
            "statement_count": 1,
            "sql": f"SELECT synthetic_mmsearch('{row['stable_key']}')",
            "target_table": target_table,
            "query_table": query_table,
            "index_read_requirement": 2 if row["mode"] == "just_index" else 1,
            "pre_statement_gucs": gucs,
            "ordered_primary_match_counts": counts,
            "route": route,
            "cache_evidence": cache_evidence,
        }
        source_postgres = {
            "postmaster_start_time": f"synthetic-epoch-{cell_index}-0",
            "backend_pid": 100 + cell_index * 5,
            "database_oid": 200,
        }
        control = {
            "disposition": "nonpublishable_control_config_changed",
            "ordered_primary_match_counts": counts,
            "route": route,
        }
        target_state = {
            "kind": (
                "persisted_generation" if row["mode"] == "just_index"
                else "transient_recomputed"),
            "layout": "synthetic-layout" if row["mode"] == "just_index" else None,
            "generation_id": 1 if row["mode"] == "just_index" else None,
            "source_identity": (
                "synthetic-source" if row["mode"] == "just_index" else None),
        }
        qualification = {
            "runtime_cell": {
                "dataset_key": row["dataset"],
                "query_count": row["query_count"],
                "target_table": target_table,
                "target_state": target_state,
                "device": runtime_device,
            },
            "source_repeat": 0,
            "source_postgres": source_postgres,
            "source_measurement": measurement,
            "query_fixture": query_fixture,
            "detached": {
                "accepted_gucs": gucs,
                "explain_json_text": '[{"Plan":{"Node Type":"Custom Scan"}}]',
                "explain_contract": {
                    "custom_scan_node_count": 1,
                    "custom_plan_provider": "BioseqBatchJoin",
                    "pipeline": {
                        "chaining_algorithm": row["requested_algorithms"][
                            "chaining"],
                        "base_alignment_algorithm": row[
                            "requested_algorithms"]["base_alignment"],
                        "pipeline_concurrency_mode": row["pipeline_mode"],
                    },
                },
                "explain_route": route,
                "controls": {
                    "cache_off": copy.deepcopy(control),
                    "vectorization_off": copy.deepcopy(control),
                },
            },
        }
        records = []
        for repeat in range(aggregator.MIN_ACCEPTED_REPEATS):
            current_postgres = {
                "postmaster_start_time": (
                    f"synthetic-epoch-{cell_index}-{repeat}"),
                "backend_pid": 100 + cell_index * 5 + repeat,
                "database_oid": 200,
            }
            records.append({
                "stable_key": row["stable_key"],
                "test_case": "synthetic_gate8_cell",
                "group_index": cell_index,
                "repeat": repeat,
                "accepted": True,
                "payload_schema_version": aggregator.RAW_PAYLOAD_SCHEMA_VERSION,
                "record_checksum": f"{cell_index * 5 + repeat + 1:016x}",
                "gate8_sql": {
                    "schema": "bioseq.e2e.gate8-sql-proof",
                    "version": 2,
                    "scope": "accepted_nonpaired_bioseq_algorithm_matrix",
                    "qualification_order": "after_source_measurement",
                    "measurement": copy.deepcopy(measurement),
                    "query_fixture": copy.deepcopy(query_fixture),
                    "binding": {
                        "repeat": repeat,
                        "source_repeat": 0,
                        "role": "source" if repeat == 0 else "reuse",
                        "source_epoch_is_current": repeat == 0,
                        "current_postgres": current_postgres,
                        "source_postgres": source_postgres,
                    },
                    "qualification": copy.deepcopy(qualification),
                },
            })
        source = {
            "campaign_id": row["campaign_id"],
            "campaign_digest": payloads["config"]["campaign_digest"],
            "config_sha256": payloads["config"]["config_sha256"],
            "report_sha256": "6" * 64,
            "raw_sidecar_sha256": "7" * 64,
            "run_id": "synthetic-gate8-run",
            "run_receipt_id": "rr_" + "8" * 32,
            "run_receipt_sha256": "9" * 64,
            "fixture_projection_sha256": fixture_projection_sha256,
        }
        build_receipt = {
            "build_receipt_id": payloads["build"]["build_receipt_id"],
            "content_sha256": payloads["build"]["build_receipt_sha256"],
        }
        return records, source, build_receipt

    @classmethod
    def _upgrade_candidate_gate8_v2(cls, candidate):
        """Replace synthetic v1 summaries through the canonical v2 projector."""
        manifest = candidate["verification_manifest"]
        payloads = {
            name: proof["payload"]
            for name, proof in manifest["proofs"].items()
        }
        execution_manifest = cls._candidate_execution_manifest(candidate)
        fixture_projection_sha256 = execution_manifest["rung"][
            "fixture_projection_sha256"]
        measurement_rows = payloads["measurement_contract"]["rows"]
        for cell_index, (row, measurement_row) in enumerate(zip(
                candidate["candidate_rows"], measurement_rows)):
            if row["pipeline"] != "bioseq":
                continue
            records, source, build_receipt = (
                cls._synthetic_validated_gate8_v2_records(
                    row, cell_index, payloads, fixture_projection_sha256))
            aggregator._validate_gate8_cell_qualifications(
                records, source, build_receipt)
            measurement_row["gate8_sql"] = (
                aggregator._project_approved_gate8_sql_v2(
                    records, row["timing_scope"]))
        cls._reseal_synthetic_candidate(candidate)
        return candidate

    @staticmethod
    def _reseal_synthetic_candidate(candidate):
        for proof in candidate["verification_manifest"]["proofs"].values():
            proof["sha256"] = hashlib.sha256(
                aggregator._canonical_bytes(proof["payload"])).hexdigest()
        envelope = copy.deepcopy(candidate)
        del envelope["candidate_envelope_sha256"]
        candidate["candidate_envelope_sha256"] = hashlib.sha256(
            aggregator._canonical_bytes(envelope)).hexdigest()

    @staticmethod
    def _synthetic_remote_host_descriptor(machine_id="M7A"):
        policy = aggregator.APPROVED_TUPLES_REMOTE_MACHINE_POLICIES[machine_id]
        receipt_host = {
            "node": policy["hostname"],
            "system": "Linux",
            "machine": policy["architecture"],
            "release": "7.0.0-1012-aws",
        }
        descriptor = {
            "schema": aggregator.APPROVED_TUPLES_REMOTE_HOST_SCHEMA,
            "version": aggregator.APPROVED_TUPLES_REMOTE_HOST_VERSION,
            "machine_id": machine_id,
            "cloud_provider": "aws",
            "aws_identity": {
                "account_id": policy["account_id"],
                "region": policy["region"],
                "availability_zone": policy["availability_zone"],
                "instance_id": policy["instance_id"],
                "ec2_architecture": policy["ec2_architecture"],
            },
            "instance_type": policy["instance_type"],
            "architecture": policy["architecture"],
            "cpu": {
                "architecture": policy["architecture"],
                "model": {
                    "C7G": "AWS Graviton3 Processor",
                    "M4": "Intel Xeon processor",
                    "M7A": "AMD EPYC 9R14",
                    "M7I": "Intel(R) Xeon(R) Platinum 8488C",
                }[machine_id],
                "isa": policy["architecture"],
                "vcpu_count": policy["vcpu_count"],
                "core_count": policy["core_count"],
                "threads_per_core": policy["threads_per_core"],
            },
            "memory": {"physical_mib": policy["physical_memory_mib"]},
            "storage": {
                "kind": "ebs",
                "root_device": policy["root_device"],
                "volume_id": policy["volume_id"],
                "capacity_gib": policy["capacity_gib"],
                "volume_type": policy["volume_type"],
                "provisioned_iops": policy["provisioned_iops"],
                "provisioned_throughput_mib_per_s": policy[
                    "provisioned_throughput_mib_per_s"],
                "encrypted": True,
                "instance_store_present": False,
            },
            "receipt_host": receipt_host,
            "measurement_device": {
                "identity": (
                    f"cpu:{policy['architecture']}:{receipt_host['node']}"),
                "version": f"Linux:{receipt_host['release']}",
            },
            "build_preset": policy["build_preset"],
            "preset_family": "e2e",
            "gpu": {
                "available": False,
                "device_count": 0,
                "identity": None,
                "driver": None,
                "vulkan_available": False,
                "aws_gpu_info": None,
            },
            "descriptor_sha256": "0" * 64,
        }
        descriptor["descriptor_sha256"] = (
            aggregator._remote_host_descriptor_sha256(descriptor))
        return descriptor

    @classmethod
    def _remoteize_candidate(cls, candidate, remote_authority):
        candidate = copy.deepcopy(candidate)
        host = remote_authority["remote_host_binding"]
        authority_sha256 = (
            aggregator._remote_bootstrap_seed_sha256(remote_authority)
            if not remote_authority["approved_results"] else
            hashlib.sha256(
                aggregator._canonical_approved_tuples_yaml_v3(
                    remote_authority)).hexdigest())
        candidate["version"] = aggregator.APPROVED_TUPLES_REMOTE_CANDIDATE_VERSION
        candidate["authority_before_sha256"] = authority_sha256
        candidate["remote_host_binding"] = {
            "machine_id": host["machine_id"],
            "host_descriptor_sha256": host["descriptor_sha256"],
            "receipt_host_sha256": hashlib.sha256(
                aggregator._canonical_bytes(host["receipt_host"])).hexdigest(),
        }
        candidate["same_host_baselines"] = [{
            "candidate_stable_key": row["stable_key"],
            "baseline_stable_key": aggregator._remote_baseline_stable_key(row),
            "host_descriptor_sha256": host["descriptor_sha256"],
        } for row in candidate["candidate_rows"]]
        baseline_by_candidate = {
            binding["candidate_stable_key"]: binding["baseline_stable_key"]
            for binding in candidate["same_host_baselines"]
        }
        for row in candidate["verification_manifest"]["proofs"][
                "chunk_plan"]["payload"]["rows"]:
            row["baseline_stable_key"] = baseline_by_candidate[
                row["stable_key"]]
        build = candidate["verification_manifest"]["proofs"]["build"]["payload"]
        build.update({
            "preset": host["build_preset"],
            "remote_host_descriptor_sha256": host["descriptor_sha256"],
            "receipt_host_sha256": candidate[
                "remote_host_binding"]["receipt_host_sha256"],
        })
        measurement = candidate["verification_manifest"]["proofs"][
            "measurement_contract"]["payload"]
        measurement.update({
            "device": copy.deepcopy(host["measurement_device"]),
            "remote_host_descriptor_sha256": host["descriptor_sha256"],
        })
        merged = sorted(
            copy.deepcopy(remote_authority["approved_results"]
                          + candidate["candidate_rows"]),
            key=aggregator._approved_observation_sort_key)
        candidate["candidate_projection_sha256"] = (
            aggregator._approved_projection_sha256(merged))
        cls._reseal_synthetic_candidate(candidate)
        return candidate

    @classmethod
    def _write_synthetic_approved_inputs(cls, directory, *, legacy=False):
        if legacy:
            authority, authority_raw = cls._legacy_approved_authority()
        else:
            authority = cls._synthetic_approved_authority()
            authority_raw = aggregator._canonical_approved_tuples_yaml_v3(authority)
        authority_path = Path(directory) / "ALL_RESULTS_TUPLES.yaml"
        authority_path.write_bytes(authority_raw)
        candidate_rows = None
        if legacy:
            candidate_row = copy.deepcopy(next(
                row for row in authority["approved_results"]
                if row["execution_conformance"] == "exact_requested_path"))
            candidate_row["timing_scope"] = (
                aggregator.APPROVED_TUPLES_QUERY_TIMING_SCOPE)
            candidate_row["metrics"] = {"unit": "ns", "p50_a": 110, "p50_b": 100}
            candidate_rows = [aggregator._approved_row_in_fixed_order(candidate_row)]
        candidate = cls._synthetic_candidate(
            authority, candidate_rows, authority_raw=authority_raw)
        candidate_path = Path(directory) / "approved_append_candidate.json"
        candidate_path.write_text(
            json.dumps(candidate, ensure_ascii=True, sort_keys=True,
                       separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
        return authority_path, authority_raw, authority, candidate_path, candidate

    @staticmethod
    def _ordered_oracle_record(counts=(1, 1), mapq=(60, 60), repeat=0):
        """Embedded synthetic evidence only; never a measured/approved result."""
        def vector_blob(field, values, schema, version, domain, vector_key, embedded):
            payload = domain.encode("ascii") + len(values).to_bytes(8, "little")
            vector = []
            for ordinal, value in enumerate(values):
                vector.append({"query_id": ordinal, field: value})
                payload += ordinal.to_bytes(8, "little")
                payload += (value % (1 << 64)).to_bytes(8, "little")
            digest = hashlib.sha256(payload).hexdigest()
            return {
                "schema": schema, "version": version, "ordered_query_count": len(values),
                "fields": ["query_id", field], "digest_domain": domain,
                "digest_algorithm": "sha256", "digest": digest,
                "digest_words": {f"word_{word}": int(digest[word * 8:word * 8 + 8], 16)
                                 for word in range(8)},
                vector_key: vector, embedded: True,
            }
        count = vector_blob(
            "primary_match_count", counts, "bioseq.e2e.ordered-query-primary-count-oracle", 2,
            "bioseq.e2e.ordered-query-primary-count.v2", "ordered_observations",
            "raw_observations_embedded")
        count.update({
            "repeat_ordinal": repeat, "direct_comparison_available": True,
            "direct_comparison_exact": True, "pair_verdict_sealed": True,
            "validity_scope": "exact ordered query-id and primary-match-count equality "
                              "plus exact total primary-match-count equality",
            "mapq_participates_in_validity": False, "diagnostic": None,
        })
        diagnostic = vector_blob(
            "mapq", mapq, "bioseq.e2e.ordered-query-mapq-diagnostic", 1,
            "bioseq.e2e.ordered-query-mapq-diagnostic.v1", "ordered_vector", "raw_vector_embedded")
        diagnostic.update({
            "numeric_vector_equal": True, "numeric_equality_is_validity_gate": False,
            "minimap2_mapq_semantics_claimed": False,
            "diagnostic": f"diagnostic MAPQ vectors happen to match at repeat={repeat}; this does "
                          "not claim minimap2 MAPQ semantics",
        })
        return {"repeat": repeat, "ordered_query_oracle": count, "mapq_diagnostic": diagnostic,
                "tuple_status": {"accepted": 1}, "correctness": {"match_count": sum(counts)}}

    @staticmethod
    def _ordered_pair_member(record, engine):
        return {
            "identity": {"engine": engine, "campaign_id": "fixture", "dataset": "fixture",
                         "index_mode": "just_index"},
            "test_case": "fixture", "group_index": 0, "repeat": record["repeat"],
            "accepted": True,
            "payload_schema_version": aggregator.RAW_PAYLOAD_SCHEMA_VERSION,
            "match_count": record["correctness"]["match_count"],
            "ordered_oracle": aggregator._validate_ordered_query_oracles(record, 2),
        }

    @classmethod
    def _ordinary_ordered_oracle_record(cls, counts=(1, 1), repeat=0):
        record = cls._ordered_oracle_record(
            counts=counts, mapq=(-1,) * len(counts), repeat=repeat)
        record["ordered_query_oracle"].update({
            "repeat_ordinal": None,
            "direct_comparison_available": False,
            "direct_comparison_exact": None,
            "pair_verdict_sealed": False,
            "diagnostic": None,
        })
        record["mapq_diagnostic"].update({
            "numeric_vector_equal": None,
            "diagnostic": None,
        })
        return record

    def test_ordered_oracle_recomputes_embedded_vectors_and_physical_digest_words(self):
        record = self._ordered_oracle_record()
        validated = aggregator._validate_ordered_query_oracles(record, 2)
        self.assertEqual(validated["counts"], (1, 1))
        self.assertEqual(validated["mapq"], (60, 60))
        self.assertIsNone(aggregator._validate_ordered_query_oracles({}, 2))
        mutations = (
            ("ordered_query_oracle", "digest", "0" * 64),
            ("ordered_query_oracle", "digest_domain", "wrong-domain"),
            ("ordered_query_oracle", "digest_words", {f"word_{index}": 0 for index in range(8)}),
            ("ordered_query_oracle", "ordered_observations", [
                {"query_id": 1, "primary_match_count": 1},
                {"query_id": 0, "primary_match_count": 1}]),
            ("ordered_query_oracle", "repeat_ordinal", 1),
            ("ordered_query_oracle", "pair_verdict_sealed", False),
            ("ordered_query_oracle", "mapq_participates_in_validity", True),
            ("mapq_diagnostic", "numeric_equality_is_validity_gate", True),
            ("mapq_diagnostic", "ordered_vector", [
                {"query_id": 0, "mapq": True}, {"query_id": 1, "mapq": 60}]),
        )
        for blob, field, value in mutations:
            with self.subTest(blob=blob, field=field):
                bad = copy.deepcopy(record)
                bad[blob][field] = value
                with self.assertRaises(aggregator.ContractError):
                    aggregator._validate_ordered_query_oracles(bad, 2)
        for field in ("ordered_query_oracle", "mapq_diagnostic"):
            bad = copy.deepcopy(record)
            bad[field] = None
            with self.assertRaises(aggregator.ContractError):
                aggregator._validate_ordered_query_oracles(bad, 2)

    def test_ordered_pair_requires_actual_same_repeat_baseline_and_equal_count_vector(self):
        bioseq = self._ordered_pair_member(self._ordered_oracle_record(), "bioseq")
        baseline = self._ordered_pair_member(self._ordered_oracle_record(), "minimap2")
        aggregator._validate_paired_repeat_oracles([bioseq, baseline], {"fixture": True})
        unequal = self._ordered_pair_member(self._ordered_oracle_record(counts=(2, 0)), "minimap2")
        wrong_repeat = self._ordered_pair_member(self._ordered_oracle_record(repeat=1), "minimap2")
        for records in ([bioseq], [bioseq, bioseq], [bioseq, unequal], [bioseq, wrong_repeat]):
            with self.assertRaises(aggregator.ContractError):
                aggregator._validate_paired_repeat_oracles(records, {"fixture": True})
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_paired_repeat_oracles([bioseq, baseline], {"fixture": False})
        unproven = copy.deepcopy(baseline)
        unproven["ordered_oracle"] = None
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_paired_repeat_oracles([bioseq, unproven], {"fixture": True})

    def test_ordinary_ordered_vectors_are_authenticated_but_never_presealed(self):
        bioseq_record = self._ordinary_ordered_oracle_record()
        baseline_record = self._ordinary_ordered_oracle_record()
        bioseq = self._ordered_pair_member(bioseq_record, "bioseq")
        baseline = self._ordered_pair_member(baseline_record, "minimap2")
        self.assertFalse(bioseq["ordered_oracle"]["sealed"])
        self.assertIsNone(bioseq["ordered_oracle"]["exact"])
        aggregator._validate_paired_repeat_oracles(
            [bioseq, baseline], {"fixture": False})

        falsely_sealed = copy.deepcopy(bioseq_record)
        falsely_sealed["ordered_query_oracle"]["direct_comparison_available"] = True
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_ordered_query_oracles(falsely_sealed, 2)

    @staticmethod
    def _ordinary_aggregate_item(engine, repeat, counts):
        identity = {
            "campaign_id": "fixture", "dataset": "fixture",
            "alphabet": "dna4", "target_size_gbp": 2,
            "kmer_size": 15, "window_size": 10,
            "index_mode": "just_index", "engine": engine,
            "bioseq_index_name": (
                "minimizers_per_fragment" if engine == "bioseq" else None),
            "requested_minimizer_algorithm": (
                "bounded_nthash_simd" if engine == "bioseq" else None),
            "requested_chaining_algorithm": (
                "simd_minimap2" if engine == "bioseq" else None),
            "requested_base_alignment_algorithm": (
                "block_aligner" if engine == "bioseq" else None),
        }
        observed = {
            "observed_minimizer_algorithm": (
                "bounded_nthash_simd" if engine == "bioseq" else None),
            "observed_chaining_algorithm": (
                "simd_minimap2" if engine == "bioseq" else None),
            "observed_base_alignment_algorithm": (
                "block_aligner" if engine == "bioseq" else None),
        }
        return {
            "identity": identity,
            "stable_key": f"{engine}-fixture",
            "repeat": repeat,
            "accepted": True,
            "record_status": "accepted",
            "disposition": "accepted",
            "rejection_reason": "",
            "behavior_comment": "",
            "correctness_evaluated": True,
            "match_count": sum(counts),
            "expected_match_count": sum(counts),
            "correct_match_count": sum(counts),
            "outer_e2e_ns": 100 + repeat,
            "adjusted_ns": 90 + repeat,
            "observed": observed,
            "transition_classifications": {
                "minimizer": "exact" if engine == "bioseq" else "not_applicable",
                "chaining": "exact" if engine == "bioseq" else "not_applicable",
                "base_alignment": "exact" if engine == "bioseq" else "not_applicable",
            },
            "ordered_oracle": {"counts": tuple(counts), "sealed": False},
            "payload_schema_version": aggregator.RAW_PAYLOAD_SCHEMA_VERSION,
            "source": {
                "report": f"synthetic/{engine}.json",
                "raw_sidecar": f"synthetic/{engine}.raw.jsonl",
                "run_id": f"synthetic-{engine}",
                "schedule_sorted_checksum": "0" * 16,
            },
            "record_checksum": f"{repeat:064x}",
        }

    def test_ordinary_aggregation_compares_same_repeat_ordered_vector_and_total(self):
        aggregator._activate_campaign_profile("Q8_RESULTS.yaml")
        records = []
        for repeat in range(5):
            records.extend([
                self._ordinary_aggregate_item("bioseq", repeat, (1, 1)),
                self._ordinary_aggregate_item("minimap2", repeat, (1, 1)),
            ])
        rows = aggregator._aggregate(records)
        bioseq = next(row for row in rows if row["engine"] == "bioseq")
        self.assertEqual(bioseq["status"], "accepted")

        records[-1]["ordered_oracle"]["counts"] = (0, 2)
        rows = aggregator._aggregate(records)
        bioseq = next(row for row in rows if row["engine"] == "bioseq")
        self.assertEqual(bioseq["status"], "invalid")
        self.assertIn("ordered primary-count vector differs", bioseq["COMMENTS"])

    def test_gate8_cache_evidence_uses_existing_resident_counters_only(self):
        identity = {
            "index_mode": "just_index",
            "requested_minimizer_algorithm": "vulkan_minimap_integer",
        }
        applicable = {
            "applicability": "persisted_vulkan_seed_statement_resident",
            "not_applicable_reason": None,
            "gpu_persisted_resident_uploads": 1,
            "gpu_persisted_resident_cache_hits": 0,
        }
        self.assertEqual(
            aggregator._validate_gate8_cache_evidence(
                applicable, identity, "fixture"),
            applicable,
        )
        non_applicable = {
            "applicability": "not_applicable_to_selected_route",
            "not_applicable_reason": "no_index_uses_transient_target_construction",
            "gpu_persisted_resident_uploads": 0,
            "gpu_persisted_resident_cache_hits": 0,
        }
        identity["index_mode"] = "no_index"
        self.assertEqual(
            aggregator._validate_gate8_cache_evidence(
                non_applicable, identity, "fixture"),
            non_applicable,
        )
        bad = copy.deepcopy(applicable)
        bad["gpu_persisted_resident_cache_hits"] = 1
        identity["index_mode"] = "just_index"
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_gate8_cache_evidence(
                bad, identity, "fixture")

    def test_gate8_explain_summary_must_exist_in_retained_plan_bytes(self):
        reader_identity = {
            "bioseq_index_name": "minimizers_per_fragment",
            "index_mode": "no_index",
            "requested_minimizer_algorithm": "bounded_nthash_simd",
        }
        self.assertIn("transient target", aggregator._gate8_mpf_reader_comment(
            {}, reader_identity))
        reader_identity["index_mode"] = "just_index"
        reader_record = {"attestation": {"minimizer_fragment_reader_stats": {
            "fragment_native_control_count_bytes": 64,
            "fragment_native_compact_direct_selection_shards": 0,
        }}}
        self.assertIn("persisted control reader", aggregator._gate8_mpf_reader_comment(
            reader_record, reader_identity))
        reader_record["attestation"]["minimizer_fragment_reader_stats"].update({
            "fragment_native_control_count_bytes": 0,
            "fragment_native_compact_direct_selection_shards": 32,
        })
        self.assertEqual(aggregator._gate8_mpf_reader_comment(
            reader_record, reader_identity), "")
        reader_identity["requested_minimizer_algorithm"] = "vulkan_minimap_integer"
        self.assertIn("device resident", aggregator._gate8_mpf_reader_comment(
            {}, reader_identity))
        pipeline = {
            "chaining_algorithm": "simd_minimap2",
            "base_alignment_algorithm": "block_aligner",
            "pipeline_concurrency_mode": "automatic",
            "max_concurrency": 3,
        }
        explain = [{"Plan": {
            "Node Type": "Custom Scan",
            "Custom Plan Provider": "BioseqBatchJoin",
            "Bioseq Pipeline": {
                "Chaining Algorithm": "Minimap2 SIMD",
                "Base Alignment Algorithm": "Block Aligner",
                "Pipeline Concurrency Mode":
                    pipeline["pipeline_concurrency_mode"],
                "Max Concurrency": pipeline["max_concurrency"],
            },
        }}]
        aggregator._validate_gate8_explain_bytes(explain, pipeline)
        explain[0]["Plan"]["Custom Plan Provider"] = "forged-summary-only"
        with self.assertRaisesRegex(
                aggregator.ContractError, "not proven by.*plan bytes"):
            aggregator._validate_gate8_explain_bytes(explain, pipeline)

    def test_raw_schema_contract_retains_exact_v11_then_admits_current(self):
        current = {
            "raw_payload_schema_version":
                aggregator.RAW_PAYLOAD_SCHEMA_VERSION,
            "old_or_mixed_schema_policy":
                aggregator.RAW_PAYLOAD_SCHEMA_POLICY,
            "unchanged_field": "sealed",
        }
        legacy = aggregator._legacy_all_results_contract(current)
        self.assertEqual(legacy, {
            "raw_payload_schema_version": 11,
            "old_or_mixed_schema_policy": "reject",
            "unchanged_field": "sealed",
        })
        self.assertEqual(
            current["raw_payload_schema_version"],
            aggregator._framework_e2e_raw_payload_schema_version(),
        )
        self.assertEqual(current["unchanged_field"], "sealed")

    def test_mapq_difference_is_diagnostic_and_cannot_hide_selected_scalar_fallback(self):
        left = self._ordered_oracle_record(mapq=(42, 60))
        right = self._ordered_oracle_record()
        comment = (
            "diagnostic MAPQ vector differs at repeat=0 ordered_index=0 "
            "bioseq=(query_id=0,mapq=42) minimap2=(query_id=0,mapq=60) "
            f"bioseq_mapq_sha256={left['mapq_diagnostic']['digest']} "
            f"minimap2_mapq_sha256={right['mapq_diagnostic']['digest']}; numeric "
            "minimap2 MAPQ equivalence is not a validity requirement")
        for record in (left, right):
            record["mapq_diagnostic"].update({"numeric_vector_equal": False, "diagnostic": comment})
        members = [self._ordered_pair_member(left, "bioseq"),
                   self._ordered_pair_member(right, "minimap2")]
        aggregator._validate_paired_repeat_oracles(members, {"fixture": True})
        aggregator._validate_accepted_behavior(left, "accepted_behavior_difference", comment, False)
        fallback_comment = (
            "simd_minimap2_reference completed through the operation-observed "
            "scalar minimizer route; " + comment)
        aggregator._validate_accepted_behavior(left, "accepted_fallback", fallback_comment, True)
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_accepted_behavior(
                left, "accepted_behavior_difference", fallback_comment, True)
        members[0]["ordered_oracle"]["mapq_equal"] = True
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_paired_repeat_oracles(members, {"fixture": True})

    def test_failed_pair_can_retain_diagnostic_vector_but_never_an_accepted_member(self):
        record = self._ordered_oracle_record(mapq=(-1, -1))
        reason = "**INVALID**: paired minimap2 oracle repeat=0 has a failed logical row"
        record["ordered_query_oracle"].update({"direct_comparison_exact": False, "diagnostic": reason})
        record["tuple_status"] = {
            "accepted": 0, "disposition": "invalid", "phase": "paired_oracle", "rejection_reason": reason,
        }
        validated = aggregator._validate_ordered_query_oracles(record, 2)
        self.assertFalse(validated["exact"])
        record["tuple_status"]["accepted"] = 1
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_ordered_query_oracles(record, 2)

    def test_full_record_paired_failure_is_invalid_and_has_no_timing_authority(self):
        # This derives a synthetic validation fixture, never a benchmark report
        # or an input eligible for the result ledger's append operation.
        record = self._raw_record(9)
        oracle = self._ordered_oracle_record(
            counts=(1,) * 8, mapq=(60,) * 8, repeat=record["repeat"])
        record["ordered_query_oracle"] = oracle["ordered_query_oracle"]
        record["mapq_diagnostic"] = oracle["mapq_diagnostic"]
        self.assertTrue(self._validate_q8_record(record)["accepted"])

        reason = (
            f"**INVALID**: paired minimap2 oracle repeat={record['repeat']} has a failed "
            "logical row bioseq_status=accepted minimap2_status=invalid")
        record["ordered_query_oracle"].update({
            "direct_comparison_exact": False, "diagnostic": reason,
        })
        record["tuple_status"].update({
            "accepted": 0, "disposition": "invalid", "phase": "paired_oracle",
            "rejection_reason": reason, "behavior_difference_comment": "",
        })
        record["alignment_snapshot"] = None
        record["persisted_gpu_operation_stats"] = None
        record["interval_receipt"] = None
        for field in (
                "downstream_active_wall_available", "downstream_active_wall_invocation_count",
                "downstream_active_wall_unavailable_invocation_count", "downstream_work_saturated"):
            record[field] = None

        def clear_durations(value):
            if isinstance(value, dict):
                for key, child in value.items():
                    if key.endswith("_ns") or key.endswith("_ns_manual"):
                        value[key] = None
                    else:
                        clear_durations(child)
            elif isinstance(value, list):
                for child in value:
                    clear_durations(child)

        clear_durations(record)
        interval = record["e2e_interval_metric_contract"]
        interval["validated_inner_envelope"]["union_not_greater"] = False
        interval["duration_clock_domains"]["interval_union"] = {
            "clock": "not_available", "unit": "ns", "value_kind": "duration",
            "rate_compatibility": "not_available",
        }
        validated = self._validate_q8_record(record)
        self.assertFalse(validated["accepted"])
        self.assertEqual(validated["record_status"], "invalid")
        self.assertFalse(validated["ordered_oracle"]["exact"])
        self.assertIsNone(validated["outer_e2e_ns"])
        bad = copy.deepcopy(record)
        bad["outer_e2e_ns"] = 1
        with self.assertRaises(aggregator.ContractError):
            self._validate_q8_record(bad)
        bad = copy.deepcopy(record)
        bad["tuple_status"]["accepted"] = 1
        with self.assertRaises(aggregator.ContractError):
            self._validate_q8_record(bad)

    @staticmethod
    def _selected_vulkan_receipt_rows(vulkan_seed, vulkan_chain):
        """Synthetic validator fixture only; never a measured result or ledger row."""
        common = {
            "gpu_device_uuid": "1" * 32,
            "gpu_driver": "validator-fixture-driver",
            "gpu_pipeline_cache_policy": "immutable_preseed_all_hit",
            "gpu_pipeline_cache_seed_sha256": "2" * 64,
            "gpu_pipeline_cache_seed_size": 64,
            "gpu_pipeline_cache_manifest_checksum": "3" * 16,
            "gpu_pipeline_cache_hit": True,
            "gpu_pipeline_cache_loads": 1,
            "gpu_pipeline_cache_seed_bytes": 64,
            "gpu_pipeline_cache_load_ns": 10,
            "gpu_pipeline_creation_ns": 20,
            "gpu_pipeline_creates": 1,
            "gpu_timestamp_query_failures": 0,
            "gpu_seed_dispatches": int(vulkan_seed),
            "gpu_chain_dispatches": int(vulkan_chain),
            "gpu_chain_backtrack_dispatches": 0,
            "gpu_extend_dispatches": 0,
            "gpu_target_window_request_dispatches": 0,
        }
        rows = {}
        for stage in aggregator.ALIGNMENT_SNAPSHOT_STAGES:
            gpu_stage = (vulkan_seed and stage in {
                "query_sketch", "target_sketch", "seed_lookup",
            }) or (vulkan_chain and stage == "chain_score")
            rows[stage] = {
                **common,
                "executed_vulkan_gpu_jobs": int(gpu_stage),
                "stage_gpu_device_ns": int(gpu_stage),
            }
        return rows

    @staticmethod
    def _empty_hybrid_legacy_gpu_ledger():
        """Synthetic reader fixture for the producer's exact empty revision-3 shape."""
        return {
            "schema": "bioseq.persisted_gpu_operation_stats",
            "revision": 3,
            "execution_id": 1,
            "backend_pid": 2,
            "database_oid": 3,
            "is_completed": True,
            "is_partial": False,
            "is_fully_drained": True,
            "is_consistent": False,
            "is_complete": True,
            "is_saturated": False,
            "dropped_operation_count": 0,
            "operation_count": 0,
            "accepted_operation_count": 0,
            "complete_operation_count": 0,
            "job_count": 0,
            "accepted_job_count": 0,
            "cache_validation": None,
            "operations": [],
        }

    def test_hybrid_vulkan_legacy_gpu_ledger_accepts_only_exact_empty_state(self):
        record = {
            "persisted_gpu_operation_stats":
                self._empty_hybrid_legacy_gpu_ledger(),
        }
        attestation = {"persisted_gpu_operation_stats_accepted": 0}
        aggregator._validate_hybrid_vulkan_legacy_gpu_ledger(
            record, attestation)

        leaked = copy.deepcopy(record)
        leaked["persisted_gpu_operation_stats"]["operation_count"] = 1
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_hybrid_vulkan_legacy_gpu_ledger(
                leaked, attestation)
        leaked = copy.deepcopy(record)
        leaked["persisted_gpu_operation_stats"]["operations"] = [{}]
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_hybrid_vulkan_legacy_gpu_ledger(
                leaked, attestation)

        # The hybrid-only reader gate must not reinterpret established CPU or
        # minimap2 sentinel records.
        self.assertTrue(self._validate_q8_record(self._raw_record(9))["accepted"])
        self.assertTrue(self._validate_q8_record(self._raw_record(5))["accepted"])

    def test_accepted_hybrid_attestation_rejects_legacy_ledger_acceptance(self):
        record = self._raw_record(9)
        identity, _, _ = aggregator._stable_identity(
            record, aggregator.Q8_CAMPAIGN_ID)
        identity["requested_minimizer_algorithm"] = "vulkan_minimap_integer"
        record["attestation"].update({
            "requested_minimizer_algorithm": "vulkan_minimap_integer",
            "observed_minimizer_algorithm": "vulkan_minimap_integer",
            "requested_backend": "hybrid_cpu_vulkan",
            "actual_backend": "hybrid_cpu_vulkan",
            "persisted_gpu_operation_stats_accepted": 1,
        })
        with self.assertRaisesRegex(
                aggregator.ContractError, "accepted legacy all-GPU ledger"):
            aggregator._validate_accepted_attestation(
                record, identity, "accepted", "", 8, True, True, False)

    def test_hybrid_vulkan_receipts_accept_independent_selected_stages(self):
        for seed, chain in ((True, False), (False, True), (True, True)):
            with self.subTest(seed=seed, chain=chain):
                aggregator._validate_selected_vulkan_stage_receipts(
                    self._selected_vulkan_receipt_rows(seed, chain), seed, chain)

    def test_hybrid_vulkan_receipts_reject_missing_or_unselected_physical_work(self):
        changes = {
            "missing_seed": ("gpu_seed_dispatches", 0),
            "missing_chain": ("gpu_chain_dispatches", 0),
            "gpu_backtracking": ("gpu_chain_backtrack_dispatches", 1),
            "gpu_alignment": ("gpu_extend_dispatches", 1),
            "gpu_target_window": ("gpu_target_window_request_dispatches", 1),
            "duplicate_cache_load": ("gpu_pipeline_cache_loads", 2),
            "wrong_seed_extent": ("gpu_pipeline_cache_seed_bytes", 63),
            "missing_pipeline": ("gpu_pipeline_creates", 0),
            "cache_miss": ("gpu_pipeline_cache_hit", False),
            "timestamp_failure": ("gpu_timestamp_query_failures", 1),
            "empty_device": ("gpu_device_uuid", "0" * 32),
        }
        for label, (field, value) in changes.items():
            with self.subTest(label=label):
                rows = self._selected_vulkan_receipt_rows(True, True)
                for row in rows.values():
                    row[field] = value
                with self.assertRaises(aggregator.ContractError):
                    aggregator._validate_selected_vulkan_stage_receipts(rows, True, True)
        for seed, chain in ((True, False), (False, True)):
            rows = self._selected_vulkan_receipt_rows(True, True)
            with self.assertRaises(aggregator.ContractError):
                aggregator._validate_selected_vulkan_stage_receipts(rows, seed, chain)
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_selected_vulkan_stage_receipts(
                self._selected_vulkan_receipt_rows(False, False), False, False)

    def test_hybrid_vulkan_receipts_reject_cross_stage_identity_and_host_device_time(self):
        for stage, field, value in (
                ("chain_score", "gpu_driver", "different-fixture-driver"),
                ("chain_backtrack", "stage_gpu_device_ns", 1),
                ("left_extension", "executed_vulkan_gpu_jobs", 1)):
            with self.subTest(stage=stage, field=field):
                rows = self._selected_vulkan_receipt_rows(True, True)
                rows[stage][field] = value
                with self.assertRaises(aggregator.ContractError):
                    aggregator._validate_selected_vulkan_stage_receipts(rows, True, True)

    @staticmethod
    def _raw_record(report_number: int, repeat: int = 0):
        path = (
            REPOSITORY_ROOT
            / f"reporting/bioseq_benchmark_e2e/report_{report_number}.raw.jsonl"
        )
        envelopes = [
            json.loads(line)
            for line in path.read_text(encoding="utf-8").splitlines()
        ]
        records = [value for value in envelopes if "kind" not in value]
        return copy.deepcopy(records[repeat])

    @staticmethod
    def _validate_q8_record(record, source=None):
        aggregator._activate_campaign_profile("Q8_RESULTS.yaml")
        return aggregator._validate_record(
            record,
            source or {},
            aggregator.Q8_CAMPAIGN_ID,
            8,
            True,
            True,
            False,
        )

    @staticmethod
    def _set_snapshot_field(record, stage, field, value):
        snapshot = record["alignment_snapshot"]
        ordinals = {
            column["name"]: column["ordinal"]
            for column in snapshot["columns"]
        }
        row = next(
            candidate for candidate in snapshot["rows"]
            if candidate[ordinals["stage"]] == stage)
        row[ordinals[field]] = value

    @classmethod
    def _remove_seed_lookup_work(cls, record):
        for field in (
                "eligible_jobs", "executed_host_io_jobs",
                "executed_scalar_cpu_jobs", "executed_simd_cpu_jobs",
                "executed_vulkan_gpu_jobs"):
            cls._set_snapshot_field(record, "seed_lookup", field, 0)

    @staticmethod
    def _unexecuted_attestation():
        return {
            "actual_backend": "",
            "actual_target_index_source": "",
            "backend_declines": -1,
            "backend_fallbacks": -1,
            "block_aligner_execution_stats": {
                "available": -1,
                "consistent": -1,
                "executed_calls": -1,
                "forbidden_ksw2_calls": -1,
                "forbidden_scalar_biological_fallbacks": -1,
                "nonempty_calls": -1,
                "nonempty_calls_with_simd": -1,
                "saturated": -1,
                "simd_dp_cells": -1,
                "simd_dp_vector_batches": -1,
            },
            "documented_minimizer_fallback_selected": False,
            "hash_major_index_reader_stats": None,
            "mapq_executed": None,
            "mapq_execution_available": None,
            "mapq_execution_conflict": None,
            "minimizer_fragment_reader_stats": None,
            "nthash_vector_activity": False,
            "observed_base_alignment_algorithm": "",
            "observed_chaining_algorithm": "",
            "observed_minimizer_algorithm": "",
            "observed_simd_alignment_algorithm": "",
            "observed_simd_chaining_policy": "",
            "persisted_gpu_operation_stats_accepted": -1,
            "prefilter_execution_stats": None,
            "requested_backend": "",
            "requested_base_alignment_algorithm": "",
            "requested_chaining_algorithm": "",
            "requested_minimizer_algorithm": "",
            "requested_simd_alignment_algorithm": "",
            "requested_simd_chaining_policy": "",
            "requested_target_index_source": "",
            "stage_engine_matrix_valid": -1,
            "stats_saturated": -1,
            "validation_accepted": 0,
            "validation_attached_to_measurement": 0,
            "validation_present": 0,
            "vulkan_synthetic_constant_mapq_not_executed": None,
        }

    @classmethod
    def _measured_connection_failure_record(cls):
        record = cls._raw_record(9)
        measured = copy.deepcopy(record["postgres"]["restart_identity"]["after"])
        recovered = copy.deepcopy(measured)
        recovered["backend_pid"] += 1
        record["tuple_status"].update({
            "accepted": 0,
            "disposition": "invalid",
            "phase": "measured_statement",
            "correctness_evaluated": 0,
            "rejection_reason": aggregator.MEASURED_QUERY_CONNECTION_FAILURE_REASON,
            "behavior_difference_comment": "",
            "sql_diagnostics": {
                "result_status": "PGRES_FATAL_ERROR",
                "sqlstate": "",
                "primary": "",
                "detail": "",
            },
        })
        record["postgres"] = {
            "measured_query_connection_failure": {
                "schema": aggregator.MEASURED_QUERY_CONNECTION_FAILURE_SCHEMA,
                "version": 1,
                "captured_status": "connection_failure",
                "same_postmaster": True,
                "same_database": True,
                "changed_live_backend": True,
                "live_identity_query_succeeded": True,
                "measured_session": measured,
                "recovered_session": recovered,
            },
        }
        record["alignment_snapshot"] = None
        record["persisted_gpu_operation_stats"] = None
        record["attestation"] = cls._unexecuted_attestation()
        record["correctness"].update({
            "evaluated": 0,
            "found": False,
            "match_count": 0,
            "correct_match_count": 0,
            "exact_primary_count_accepted": 0,
            "strand_result_count": 0,
            "correct_strand_count": 0,
            "cigar_result_count": 0,
            "correct_cigar_count": 0,
            "cigar_replay_count": 0,
            "mapq_result_count": 0,
            "max_abs_offset_bp": None,
        })
        record["benchmark_shape"].update({
            "actual_target_rows": None,
            "actual_alignment_jobs": None,
        })
        for field in (
                "outer_e2e_ns", "index_and_map_ns",
                "cigar_or_mapq_interval_union_ns",
                "outer_e2e_minus_cigar_or_mapq_union_ns",
                "downstream_active_wall_ns", "downstream_work_ns",
                "interval_receipt"):
            record[field] = None
        record["downstream_active_wall_available"] = None
        record["downstream_active_wall_invocation_count"] = None
        record["downstream_active_wall_unavailable_invocation_count"] = None
        record["downstream_work_saturated"] = None
        interval_contract = record["e2e_interval_metric_contract"]
        interval_contract["validated_inner_envelope"]["value_ns"] = None
        interval_contract["validated_inner_envelope"]["union_not_greater"] = False
        interval_contract["duration_clock_domains"]["interval_union"] = {
            "clock": "not_available",
            "unit": "ns",
            "value_kind": "duration",
            "rate_compatibility": "not_available",
        }
        return record

    def test_observed_rung_mints_execution_identity_without_reclassifying_legacy(self):
        identities = []
        for alphabet, kmer, window, minimizer, disposition in (
                ("dna4", 20, 15, "scalar", "rejected_setup"),
                ("dna5", 15, 10, "scalar", "rejected_setup"),
                ("dna4", 15, 10, "vulkan_minimap_integer", "rejected_sql")):
            identity = {
                "engine": "bioseq",
                "index_mode": "just_index",
                "kmer_size": kmer,
                "window_size": window,
                "alphabet": alphabet,
                "requested_minimizer_algorithm": minimizer,
            }
            identities.append((identity, disposition))

        prior_status_contract = generator.RESULT_STATUS_CONTRACT
        try:
            generator.RESULT_STATUS_CONTRACT = (
                generator.LEGACY_RESULT_STATUS_CONTRACT)
            for identity, disposition in identities:
                self.assertEqual(
                    generator._canonical_cell_rejection_disposition(
                        {"identity": identity}),
                    disposition,
                )
                self.assertEqual(
                    aggregator._canonical_cell_rejection_disposition(identity),
                    disposition,
                )

            generator.RESULT_STATUS_CONTRACT = (
                generator.OBSERVED_RESULT_STATUS_CONTRACT)
            for identity, _ in identities:
                self.assertIsNone(
                    generator._canonical_cell_rejection_disposition(
                        {"identity": identity}))
                self.assertIsNone(
                    aggregator._canonical_cell_rejection_disposition(
                        identity,
                        aggregator.OBSERVED_RESULT_STATUS_CONTRACT,
                    ))
            self.assertEqual(
                generator._allowed_cell_statuses("bioseq"), ["accepted"])
            self.assertEqual(
                aggregator._allowed_cell_statuses(
                    "bioseq", aggregator.OBSERVED_RESULT_STATUS_CONTRACT),
                ["accepted"],
            )
            self.assertEqual(
                generator._publication_status_contract()[
                    "nonpublishable_statuses"],
                ["rejected", "invalid", "pending"],
            )
        finally:
            generator.RESULT_STATUS_CONTRACT = prior_status_contract

        self.assertEqual(
            aggregator._allowed_cell_statuses(
                "bioseq", aggregator.LEGACY_RESULT_STATUS_CONTRACT),
            ["accepted", "rejected"],
        )

        self.assertEqual(generator.OBSERVED_RUNG_CAMPAIGN_VERSION, 2)
        self.assertEqual(
            generator.RUNG_QUERY_COUNTS,
            (2, 8, 16, 32, 64, 128, 256, 512, 1024),
        )
        self.assertEqual(generator.RUNG_QUERY_COUNTS, aggregator.RUNG_QUERY_COUNTS)
        self.assertEqual(
            generator._observed_rung_execution_contract_id(32),
            "bioseq-e2e-algorithm-matrix-local-q32-observed-v1",
        )
        self.assertNotEqual(
            generator._observed_rung_execution_contract_id(32),
            generator.CANONICAL_CAMPAIGN_ID,
        )
        self.assertNotEqual(
            generator._observed_rung_execution_contract_id(32),
            generator._observed_rung_execution_contract_id(64),
        )
        self.assertEqual(
            generator.CANONICAL_CAMPAIGN_ID, aggregator.Q1024_CAMPAIGN_ID)

    def test_observed_rung_fixture_is_exact_bounded_q1024_prefix(self):
        cases = (
            ("M2_D4_K15_W10", 2, "dna4", 15, 10),
            ("M2_D4_K20_W15", 2, "dna4", 20, 15),
            ("M2_D5_K15_W10", 2, "dna5", 15, 10),
            ("M2_D5_K20_W15", 2, "dna5", 20, 15),
            ("M4_D4_K15_W10", 4, "dna4", 15, 10),
            ("M4_D4_K20_W15", 4, "dna4", 20, 15),
            ("M4_D5_K15_W10", 4, "dna5", 15, 10),
            ("M4_D5_K20_W15", 4, "dna5", 20, 15),
        )
        canonical = {
            "test_cases": {
                name: {
                    "concurrency": 1,
                    "description": "synthetic canonical q1024",
                    "data": {
                        "reference_length": target * 1_000_000_000,
                        "fragment_count": 1024,
                        "alphabet": alphabet,
                        "kmer_size": kmer,
                        "window_size": window,
                        "seed": 33_300_000 + target * 10_000 + kmer,
                    },
                }
                for name, target, alphabet, kmer, window in cases
            },
            "bench_e2e": {
                "tags": ["e2e", "algorithm-matrix", "full-campaign"],
                "description": "synthetic canonical q1024",
            },
        }
        projected = aggregator._expected_observed_rung_config(canonical, 32)
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "synthetic_q32.json"
            fixture.write_text(
                json.dumps(projected, sort_keys=True), encoding="utf-8")
            reparsed = json.loads(fixture.read_text(encoding="utf-8"))
        self.assertEqual(
            {case["data"]["fragment_count"]
             for case in reparsed["test_cases"].values()},
            {32},
        )
        self.assertEqual(
            reparsed["bench_e2e"]["tags"],
            ["e2e", "algorithm-matrix", "q32-campaign"],
        )
        fixture_digest = aggregator._rung_fixture_projection_sha256(reparsed, 32)
        changed = copy.deepcopy(reparsed)
        changed["test_cases"]["M2_D4_K15_W10"]["data"]["seed"] += 1
        self.assertNotEqual(
            fixture_digest,
            aggregator._rung_fixture_projection_sha256(changed, 32),
        )
        self.assertEqual(
            {case["data"]["fragment_count"]
             for case in canonical["test_cases"].values()},
            {1024},
        )

    def test_observed_rung_revalidates_canonical_manifest_config_chain(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bench = root / "bench"
            bench.mkdir()
            config_path = bench / "synthetic_q1024.json"
            config_raw = b'{"synthetic":"q1024"}\n'
            config_path.write_bytes(config_raw)
            manifest_path = bench / "synthetic_q1024_manifest.json"
            manifest = {
                "schema": generator.CAMPAIGN_SCHEMA,
                "version": generator.CAMPAIGN_VERSION,
                "campaign": {
                    "campaign_id": generator.CANONICAL_CAMPAIGN_ID,
                    "campaign_digest_algorithm":
                        "sha256-canonical-json-without-campaign_digest",
                },
                "source": {
                    "full_config": "bench/synthetic_q1024.json",
                    "full_config_sha256": hashlib.sha256(config_raw).hexdigest(),
                },
            }
            campaign_digest = generator._sha256(
                generator._canonical_bytes(manifest))
            manifest["campaign"]["campaign_digest"] = campaign_digest
            manifest_path.write_bytes(generator._render(manifest))

            prior = (
                generator.REPOSITORY_ROOT,
                generator.SOURCE_CONFIG,
                generator.CANONICAL_CONTRACT_PATH,
                generator.EXPECTED_CAMPAIGN_DIGEST,
            )
            try:
                generator.REPOSITORY_ROOT = root
                generator.SOURCE_CONFIG = config_path
                generator.CANONICAL_CONTRACT_PATH = manifest_path
                generator.EXPECTED_CAMPAIGN_DIGEST = campaign_digest
                binding = generator._canonical_q1024_binding(config_raw)
                self.assertEqual(binding["config_sha256"], hashlib.sha256(
                    config_raw).hexdigest())
                self.assertEqual(binding["campaign_digest"], campaign_digest)
                with self.assertRaisesRegex(
                        generator.GenerationError, "source config"):
                    generator._canonical_q1024_binding(config_raw + b" ")
            finally:
                (generator.REPOSITORY_ROOT,
                 generator.SOURCE_CONFIG,
                 generator.CANONICAL_CONTRACT_PATH,
                 generator.EXPECTED_CAMPAIGN_DIGEST) = prior

    def test_observed_rung_status_gate_requires_five_successful_repeats(self):
        records = [
            {
                "accepted": True,
                "record_status": "accepted",
                "identity": {
                    "engine": "bioseq",
                    "alphabet": "dna4",
                    "requested_minimizer_algorithm": "scalar",
                    "requested_chaining_algorithm": "scalar_minimap2",
                    "requested_base_alignment_algorithm": "ksw2",
                },
                "observed": {
                    "observed_minimizer_algorithm": "scalar",
                    "observed_chaining_algorithm": "scalar_minimap2",
                    "observed_base_alignment_algorithm": "ksw2",
                },
                "disposition": "accepted",
                "transition_classifications": {
                    "minimizer": "exact",
                    "chaining": "exact",
                    "base_alignment": "exact",
                },
                "stable_key": "synthetic-observed-cell",
                "repeat": repeat,
            }
            for repeat in range(5)
        ]
        self.assertEqual(
            aggregator._validated_observed_rung_stable_keys(records),
            {"synthetic-observed-cell"},
        )
        accepted_timing = [{
            "record_status": "accepted",
            "disposition": "accepted",
            "outer_e2e_ns": 100,
            "adjusted_ns": 80,
        }]
        aggregator._validate_config_disposition_records(
            accepted_timing, {"expected_all_records_disposition": None})
        with self.assertRaisesRegex(
                aggregator.ContractError, "all-rejected canonical shard"):
            aggregator._validate_config_disposition_records(
                accepted_timing,
                {"expected_all_records_disposition": "rejected_setup"},
            )
        rejected = copy.deepcopy(records)
        rejected[2].update({"accepted": False, "record_status": "rejected"})
        with self.assertRaisesRegex(
                aggregator.ContractError, "nonpublishable cell status"):
            aggregator._validated_observed_rung_stable_keys(rejected)
        with self.assertRaisesRegex(
                aggregator.ContractError, "five-repeat cell ownership"):
            aggregator._validated_observed_rung_stable_keys(records[:-1])

        args = aggregator._arguments([
            "validate-observed-rung-report",
            "--manifest", "bench/synthetic_manifest.json",
            "--expected-campaign-digest", "a" * 64,
            "--query-count", "32",
            "--report", "reporting/bioseq_benchmark_e2e/report_1.json",
            "--config", "bench/synthetic_config.json",
            "--run-receipt-id", "rr_" + "b" * 32,
        ])
        self.assertEqual(
            args.observed_rung_command,
            "validate-observed-rung-report",
        )
        self.assertIsNone(args.approved_command)
        self.assertEqual(args.query_count, 32)

    def test_observed_rung_accepts_exact_manifest_resolved_absolute_config(self):
        config_entry = {
            "path": (
                "bench/e2e_algorithm_matrix_q2_observed_singletons/"
                "bench_e2e_algorithm_matrix_q2_observed_singleton_"
                "0725_925f4f05e3a6c239.json"),
            "execution_authority": "observed_rung_singleton",
        }
        config_path = aggregator._canonical_repository_file(
            config_entry["path"], "observed rung manifest config")
        with mock.patch.object(
                aggregator, "ACTIVE_CAMPAIGN_PROFILE", "observed_rung"):
            self.assertEqual(
                aggregator._sealed_report_config_location(
                    str(config_path), config_path, config_entry,
                    "observed rung report config location"),
                config_path,
            )

    def test_observed_rung_rejects_wrong_absolute_config(self):
        config_entry = {
            "path": (
                "bench/e2e_algorithm_matrix_q2_observed_singletons/"
                "bench_e2e_algorithm_matrix_q2_observed_singleton_"
                "0725_925f4f05e3a6c239.json"),
            "execution_authority": "observed_rung_singleton",
        }
        config_path = aggregator._canonical_repository_file(
            config_entry["path"], "observed rung manifest config")
        wrong_path = aggregator.Q1024_CONFIG_PATH
        with mock.patch.object(
                aggregator, "ACTIVE_CAMPAIGN_PROFILE", "observed_rung"):
            with self.assertRaisesRegex(
                    aggregator.ContractError,
                    "absolute spelling is not manifest authority"):
                aggregator._sealed_report_config_location(
                    str(wrong_path), config_path, config_entry,
                    "observed rung report config location")

    def test_observed_rung_permits_only_dna5_reference_scalar_exception(self):
        dna5_reference_scalar = [
            {
                "accepted": True,
                "record_status": "accepted",
                "identity": {
                    "engine": "bioseq",
                    "alphabet": "dna5",
                    "requested_minimizer_algorithm":
                        "simd_minimap2_reference",
                    "requested_chaining_algorithm": "simd_minimap2",
                    "requested_base_alignment_algorithm": "block_aligner",
                },
                "observed": {
                    "observed_minimizer_algorithm": "scalar",
                    "observed_chaining_algorithm": "simd_minimap2",
                    "observed_base_alignment_algorithm": "block_aligner",
                },
                "disposition": "accepted_fallback",
                "transition_classifications": {
                    "minimizer": "documented_minimizer_fallback",
                    "chaining": "exact",
                    "base_alignment": "exact",
                },
                "stable_key": "synthetic-dna5-reference-cell",
                "repeat": repeat,
            }
            for repeat in range(5)
        ]
        self.assertEqual(
            aggregator._validated_observed_rung_stable_keys(
                dna5_reference_scalar),
            {"synthetic-dna5-reference-cell"},
        )
        self.assertEqual(
            set(aggregator._observed_rung_path_classifications(
                dna5_reference_scalar).values()),
            {aggregator.OBSERVED_RUNG_REQUESTED_PATH_INELIGIBLE},
        )

        dna4_reference_scalar = copy.deepcopy(dna5_reference_scalar)
        for item in dna4_reference_scalar:
            item["identity"]["alphabet"] = "dna4"
        bounded_to_scalar = copy.deepcopy(dna5_reference_scalar)
        for item in bounded_to_scalar:
            item["identity"]["requested_minimizer_algorithm"] = (
                "bounded_nthash_simd")
        two_stack_to_scalar = copy.deepcopy(dna5_reference_scalar)
        for item in two_stack_to_scalar:
            item["identity"]["requested_minimizer_algorithm"] = (
                "two_stack_nthash")
        vulkan_to_scalar = copy.deepcopy(dna5_reference_scalar)
        for item in vulkan_to_scalar:
            item["identity"]["requested_minimizer_algorithm"] = (
                "vulkan_minimap_integer")
        chaining_fallback = copy.deepcopy(dna5_reference_scalar)
        for item in chaining_fallback:
            item["observed"]["observed_chaining_algorithm"] = "scalar_minimap2"
            item["transition_classifications"]["chaining"] = (
                "documented_chaining_fallback")
        base_alignment_fallback = copy.deepcopy(dna5_reference_scalar)
        for item in base_alignment_fallback:
            item["observed"]["observed_base_alignment_algorithm"] = "ksw2"
            item["transition_classifications"]["base_alignment"] = (
                "documented_base_alignment_fallback")

        for label, records in (
                ("dna4-reference-scalar", dna4_reference_scalar),
                ("bounded-to-scalar", bounded_to_scalar),
                ("two-stack-to-scalar", two_stack_to_scalar),
                ("vulkan-to-scalar", vulkan_to_scalar),
                ("chaining-fallback", chaining_fallback),
                ("base-alignment-fallback", base_alignment_fallback)):
            with self.subTest(fallback=label):
                with self.assertRaisesRegex(
                        aggregator.ContractError, "fallback substitution"):
                    aggregator._validated_observed_rung_stable_keys(records)

    def test_q1024_hybrid_partition_is_exact_and_canonical_topology_is_unchanged(self):
        aggregator._activate_campaign_profile("ALL_RESULTS.yaml")
        contract = aggregator._load_campaign_contract()
        singleton_entries = [
            entry for entry in contract["configs_by_path"].values()
            if entry.get("execution_authority") == "q1024_singleton"
        ]
        packed_entries = [
            entry for entry in contract["configs_by_path"].values()
            if entry.get("execution_authority") == "q1024_packed"
        ]
        adopted_entries = [
            entry for entry in contract["configs_by_path"].values()
            if entry.get("execution_authority")
                == "q1024_deadline_v2_adoption"
        ]
        self.assertEqual(len(contract["canonical_cells"]), 1296)
        self.assertEqual(len(singleton_entries), 1142)
        self.assertEqual(len(packed_entries), 25)
        self.assertEqual(len(adopted_entries), 4)
        self.assertEqual(
            sum(entry["expected_result_cells"] for entry in packed_entries),
            150,
        )
        execution_keys = []
        for entry in [*singleton_entries, *packed_entries, *adopted_entries]:
            execution_keys.extend(entry["descriptors_by_key"])
        self.assertEqual(len(execution_keys), 1296)
        self.assertEqual(len(set(execution_keys)), 1296)
        repeat_keys = [
            (stable_key, repeat)
            for stable_key in execution_keys
            for repeat in range(5)
        ]
        self.assertEqual(len(repeat_keys), 6480)
        self.assertEqual(len(set(repeat_keys)), 6480)
        packed_manifest = json.loads((
            REPOSITORY_ROOT
            / "bench/bench_e2e_algorithm_matrix_q1024_"
              "packed_partition_manifest.json"
        ).read_text(encoding="utf-8"))
        plan = packed_manifest["execution_plan"]
        self.assertEqual(
            plan["six_day"]["supported_cells_including_adoptions"]
            + plan["remaining_scope_after_six_day"]
                ["total_supported_cells"],
            304,
        )
        self.assertEqual(
            plan["seven_day"]["supported_cells_including_adoptions"]
            + plan["remaining_scope_after_seven_day"]
                ["total_supported_cells"],
            304,
        )
        minimap2_cells = sum(
            cell["identity"]["engine"] == "minimap2"
            for cell in contract["canonical_cells"].values()
        )
        self.assertEqual(minimap2_cells, 16)
        self.assertEqual(304 - minimap2_cells, 288)
        self.assertEqual(
            contract["campaign_digest"],
            "011fbb1ef33a214ab80d3568c2341e8d0f8311f82e21a5955bdcb93b3c63b979",
        )
        self.assertEqual(
            contract["execution_authority_sha256"],
            aggregator.Q1024_PACKED_PARTITION_SHA256,
        )
        first = contract["configs_by_path"][
            REPOSITORY_ROOT
            / "bench/e2e_algorithm_matrix_deadline_v2_shards/"
            "bench_e2e_algorithm_matrix_deadline_v2_s056_q1024_"
            "q1024_anchor_m2_d4_k15_w10_no_index_minimap2.json"
        ]
        self.assertEqual(first["cells"][0]["tuple_index"], 0)
        self.assertEqual(
            next(iter(first["descriptors_by_key"].values()))["tuple_index"],
            161,
        )
        self.assertEqual(len(first["declared_metric_names"]), 21)

    def test_q1024_empty_ledger_migrates_without_losing_top_level_fields(self):
        aggregator._activate_campaign_profile("ALL_RESULTS.yaml")
        contract = aggregator._load_campaign_contract()
        document = _revision4_empty_ledger()
        top_level = set(document)

        aggregator._upgrade_q1024_ledger_contract(document, contract)
        aggregator._validate_all_results(document, contract)

        self.assertEqual(set(document), top_level)
        self.assertEqual(document["schema_version"], 6)
        self.assertEqual(document["contract"]["version"], 4)
        self.assertEqual(document["aggregation_state"]["build_receipts"], [])
        self.assertEqual(document["campaign"]["status"], "aggregation_in_progress")
        self.assertEqual(document["publication_state"]["result_count"], 0)

    def test_q1024_legacy_migration_rejects_every_populated_state_variant(self):
        aggregator._activate_campaign_profile("ALL_RESULTS.yaml")
        contract = aggregator._load_campaign_contract()
        original = _revision4_empty_ledger()
        mutations = {
            "result_cell_count": lambda value: value["aggregation_state"].__setitem__(
                "result_cell_count", 1),
            "accepted_cell_count": lambda value: value["aggregation_state"].__setitem__(
                "accepted_cell_count", 1),
            "measurements_collected": lambda value: value["campaign"].__setitem__(
                "measurements_collected", 5),
            "processed_report_membership": lambda value: value["aggregation_state"].__setitem__(
                "processed_reports", [{}]),
            "report_count": lambda value: value["aggregation_state"].__setitem__(
                "report_count", 1),
            "raw_record_count": lambda value: value["aggregation_state"].__setitem__(
                "raw_record_count", 5),
            "rejected_cell_count": lambda value: value["aggregation_state"].__setitem__(
                "rejected_cell_count", 1),
            "invalid_cell_count": lambda value: value["aggregation_state"].__setitem__(
                "invalid_cell_count", 1),
            "pending_cell_count": lambda value: value["aggregation_state"].__setitem__(
                "pending_cell_count", 1),
            "missing_cell_count": lambda value: value["aggregation_state"].__setitem__(
                "missing_canonical_cell_count", 1295),
            "membership_complete": lambda value: value["aggregation_state"].__setitem__(
                "canonical_membership_complete", True),
            "statuses_invalid": lambda value: value["aggregation_state"].__setitem__(
                "canonical_statuses_valid", False),
            "result_collection": lambda value: value.__setitem__("results", [{}]),
            "publication_result_count": lambda value: value["publication_state"].__setitem__(
                "result_count", 1),
            "publication_ready": lambda value: value["publication_state"].__setitem__(
                "status", "complete"),
            "publishable": lambda value: value["campaign"].__setitem__(
                "results_are_publishable", True),
            "deadline_checkpoint_tamper": lambda value: value.__setitem__(
                "deadline_v2_checkpoint", {}),
            "legacy_receipt_membership": lambda value: value["aggregation_state"].__setitem__(
                "build_receipts", []),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                document = copy.deepcopy(original)
                mutate(document)
                self.assertFalse(aggregator._legacy_q1024_ledger_is_exactly_empty(
                    document,
                    contract["campaign_id"],
                    contract["campaign_digest"],
                ))
                with self.assertRaisesRegex(
                        aggregator.ContractError, "exact legacy empty contract"):
                    aggregator._upgrade_q1024_ledger_contract(document, contract)

    def test_q1024_empty_ledger_enforces_deterministic_first_ingestion(self):
        aggregator._activate_campaign_profile("ALL_RESULTS.yaml")
        ledger_path = REPOSITORY_ROOT / "ALL_RESULTS.yaml"
        fixture = _revision4_empty_ledger()
        fixture_raw = REVISION4_EMPTY_LEDGER_FIXTURE.read_bytes()
        read_json_file = aggregator._read_json_file

        def read_with_fixture(path, label):
            if Path(path) == ledger_path:
                return copy.deepcopy(fixture), fixture_raw
            return read_json_file(path, label)

        with mock.patch.object(
                aggregator, "_read_json_file",
                side_effect=read_with_fixture), mock.patch.object(
                    aggregator, "_exclusive_results_lock",
                    return_value=contextlib.nullcontext()), mock.patch.object(
                        aggregator, "_atomic_write") as atomic_write:
            with self.assertRaisesRegex(
                    aggregator.ContractError,
                    "must start with deterministic.*baseline"):
                aggregator.merge(
                    REPOSITORY_ROOT
                    / "reporting/bioseq_benchmark_e2e/report_9.json",
                    REPOSITORY_ROOT
                    / "bench/e2e_algorithm_matrix_shards/"
                    "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
                    "0000_0ed6b7419bc8e21f.json",
                    ledger_path,
                )
        atomic_write.assert_not_called()

    def test_q8_rescue_union_reuses_core_raw_and_parity_validation(self):
        aggregator._activate_campaign_profile("Q8_RESULTS.yaml")
        with self._load_q8_contract_with_synthetic_source_evidence() as loaded:
            contract, repository_bytes = loaded
            document, _ = aggregator._read_json_file(
                REPOSITORY_ROOT / "Q8_RESULTS.yaml", "Q8_RESULTS.yaml")
            aggregator._refresh_q8_ledger_proof_binding(document, contract)
            aggregator._validate_all_results(document, contract)
            records, processed = aggregator._load_existing_reports(
                document, contract, {})
            processed_paths = {entry["report"] for entry in processed}
            for report_number in (9, 10, 11, 12):
                report_path = (
                    REPOSITORY_ROOT
                    / f"reporting/bioseq_benchmark_e2e/report_{report_number}.json"
                )
                report = json.loads(
                    repository_bytes[report_path].decode("utf-8"))
                config_path = Path(
                    report["benchmarks"]["bench_e2e"]["location"])
                if report_path.relative_to(REPOSITORY_ROOT).as_posix() \
                        in processed_paths:
                    continue
                loaded_records, source = aggregator._load_report(
                    report_path, config_path, contract)
                records.extend(loaded_records)
                processed.append(source)
            rows = aggregator._aggregate(records)

        self.assertEqual(len(processed), 5)
        self.assertEqual(len(records), 25)
        self.assertEqual(len(rows), 5)
        self.assertTrue(all(row["status"] == "accepted" for row in rows))
        self.assertTrue(all(row["COMMENTS"] is None for row in rows))
        p50_by_stable_key = {
            (
                row["engine"], row["bioseq_index_name"],
                row["requested_base_alignment_algorithm"],
            ): (
                row["outer_e2e_ns_p50"],
                row["outer_e2e_minus_cigar_or_mapq_union_ns_p50"],
            )
            for row in rows
        }
        self.assertEqual(
            sorted(p50_by_stable_key.values()),
            sorted([
                (41910820292, 41907189292),
                (214237952792, 214214777003),
                (217095317542, 217091647336),
                (1786874695375, 1786715083583),
                (1787744955708, 1787583535460),
            ]),
        )
        self.assertEqual(1296 - len(rows), 1291)
        self.assertEqual(
            [entry["report"] for entry in processed],
            [
                "reporting/bioseq_benchmark_e2e/report_5.json",
                "reporting/bioseq_benchmark_e2e/report_9.json",
                "reporting/bioseq_benchmark_e2e/report_10.json",
                "reporting/bioseq_benchmark_e2e/report_11.json",
                "reporting/bioseq_benchmark_e2e/report_12.json",
            ],
        )

    def test_embedded_q8_receipts_remain_valid_without_receipt_store_lookup(self):
        aggregator._activate_campaign_profile("Q8_RESULTS.yaml")
        with self._load_q8_contract_with_synthetic_source_evidence() as loaded:
            contract, _ = loaded
            rescue_entries = [
                entry for entry in contract["configs_by_path"].values()
                if entry.get("execution_authority") == "q8_rescue"
            ]
        self.assertEqual(len(rescue_entries), 4)
        receipt_ids = {
            entry["sealed_evidence"]["run_receipt_id"]
            for entry in rescue_entries
        }
        self.assertEqual(receipt_ids, {
            "rr_f5c3b0b95787c1ac2783ef782b0d5e21",
            "rr_3e7eab959234858337df1ebae6484516",
            "rr_f5a7f58b35eeee20a7d4b3d18daf7ed9",
            "rr_3d8c2f2baecf4642cd23585bb4250840",
        })

    def test_run_receipt_accepts_only_exact_optional_debug_instrumentation(self):
        manifest = json.loads(
            aggregator.Q8_RESCUE_MANIFEST_PATH.read_text(encoding="utf-8"))
        child = manifest["children"][0]
        build_receipt = manifest["build_receipt"]
        report_path = REPOSITORY_ROOT / child["report"]["path"]
        config_path = REPOSITORY_ROOT / child["path"]

        def receipt_with_flags(flags):
            receipt = copy.deepcopy(child["run_receipt"])
            receipt["bench_flags"] = flags
            payload = copy.deepcopy(receipt)
            del payload["content_sha256"]
            receipt["content_sha256"] = aggregator._sha256(
                aggregator._canonical_bytes(payload))
            return receipt

        baseline, _ = aggregator._validate_run_receipt_snapshot(
            child["run_receipt"],
            build_receipt,
            report_path,
            config_path,
            "baseline run receipt",
        )
        self.assertEqual(
            baseline["bench_flags"],
            ["--filter", "benchmark=bench_e2e"],
        )

        instrumented = receipt_with_flags(
            ["--filter", "benchmark=bench_e2e", "--debug"])
        validated, _ = aggregator._validate_run_receipt_snapshot(
            instrumented,
            build_receipt,
            report_path,
            config_path,
            "instrumented run receipt",
        )
        self.assertEqual(validated["bench_flags"], instrumented["bench_flags"])

        invalid_flags = (
            ["--debug", "--filter", "benchmark=bench_e2e"],
            ["--filter", "benchmark=bench_e2e", "--debug", "--extra"],
            ["--filter", "benchmark=bench_e2e,bench_sort_scan", "--debug"],
        )
        for flags in invalid_flags:
            with self.subTest(flags=flags), self.assertRaisesRegex(
                    aggregator.ContractError,
                    "does not bind the exact report/config/build execution"):
                aggregator._validate_run_receipt_snapshot(
                    receipt_with_flags(flags),
                    build_receipt,
                    report_path,
                    config_path,
                    "invalid instrumented run receipt",
                )

    def test_closed_attestation_rejects_independent_report9_corruptions(self):
        mutations = {
            "unproved_minimizer_transition": lambda value: value["attestation"].__setitem__(
                "observed_minimizer_algorithm", "vulkan_minimap_integer"),
            "missing_prefilter_execution_stats": lambda value: value["attestation"].pop(
                "prefilter_execution_stats"),
            "unexplained_backend_fallbacks": lambda value: value["attestation"].__setitem__(
                "backend_fallbacks", 17),
            "missing_block_aligner_execution_stats": lambda value: value["attestation"].pop(
                "block_aligner_execution_stats"),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                record = self._raw_record(9)
                mutate(record)
                with self.assertRaises(aggregator.ContractError):
                    self._validate_q8_record(record)

    def test_closed_attestation_rejects_hidden_signals_and_missing_seed_work(self):
        mutations = {
            "stage_gpu_timestamp_failure": lambda value: self._set_snapshot_field(
                value, "query_sketch", "gpu_timestamp_query_failures", 1),
            "nested_reader_identity_mismatch": lambda value: value["attestation"][
                "minimizer_fragment_reader_stats"].__setitem__(
                    "file_action_identity_mismatches", 1),
            "nested_simd_saturation": lambda value: self._set_snapshot_field(
                value, "query_sketch", "simd_execution_stats", {
                    **value["alignment_snapshot"]["rows"][0][150],
                    "downstream_work_saturated": True,
                }),
            "missing_seed_lookup_work": self._remove_seed_lookup_work,
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                record = self._raw_record(9)
                mutate(record)
                with self.assertRaises(aggregator.ContractError):
                    self._validate_q8_record(record)

    def test_measured_connection_failure_is_invalid_and_untimed(self):
        record = self._measured_connection_failure_record()
        validated = self._validate_q8_record(record)

        self.assertFalse(validated["accepted"])
        self.assertEqual(validated["record_status"], "invalid")
        self.assertEqual(validated["disposition"], "invalid")
        self.assertFalse(validated["correctness_evaluated"])
        self.assertIsNone(validated["outer_e2e_ns"])
        self.assertIsNone(validated["adjusted_ns"])

    def test_measured_connection_failure_proof_fails_closed(self):
        proof_path = ("postgres", "measured_query_connection_failure")

        def proof(record):
            return record[proof_path[0]][proof_path[1]]

        mutations = {
            "changed_postmaster": lambda value: proof(value)[
                "recovered_session"].__setitem__("postmaster_start", "changed"),
            "changed_database": lambda value: proof(value)[
                "recovered_session"].__setitem__("database_oid", 1),
            "unchanged_backend": lambda value: proof(value)[
                "recovered_session"].__setitem__(
                    "backend_pid", proof(value)["measured_session"]["backend_pid"]),
            "identity_query_not_live": lambda value: proof(value).__setitem__(
                "live_identity_query_succeeded", False),
            "missing_exact_field": lambda value: proof(value).pop("captured_status"),
            "server_error_sqlstate": lambda value: value["tuple_status"][
                "sql_diagnostics"].__setitem__("sqlstate", "08006"),
            "unexpected_result_status": lambda value: value["tuple_status"][
                "sql_diagnostics"].__setitem__("result_status", "PGRES_BAD_RESPONSE"),
            "retained_server_primary": lambda value: value["tuple_status"][
                "sql_diagnostics"].__setitem__("primary", "backend terminated"),
            "retained_timing": lambda value: value.__setitem__("outer_e2e_ns", 1),
            "retained_correctness": lambda value: value["correctness"].__setitem__(
                "match_count", 1),
            "retained_route": lambda value: value["attestation"].__setitem__(
                "observed_minimizer_algorithm", "two_stack_nthash"),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                record = self._measured_connection_failure_record()
                mutate(record)
                with self.assertRaises(aggregator.ContractError):
                    self._validate_q8_record(record)

    def test_documented_minimizer_fallback_is_accepted_and_classified(self):
        requested = "simd_minimap2_reference"
        comment = (
            "simd_minimap2_reference completed through the operation-observed "
            "scalar minimizer route")
        validated = []
        for repeat in range(5):
            record = self._raw_record(9, repeat)
            record["params"]["minimizer_algorithm"] = requested
            record["tuple_status"].update({
                "requested_minimizer_algorithm": requested,
                "disposition": "accepted_fallback",
                "behavior_difference_comment": comment,
            })
            record["attestation"].update({
                "requested_minimizer_algorithm": requested,
                "requested_backend": "mixed_cpu",
                "actual_backend": "mixed_cpu",
                "documented_minimizer_fallback_selected": True,
            })
            snapshot = record["alignment_snapshot"]
            ordinals = {
                column["name"]: column["ordinal"]
                for column in snapshot["columns"]
            }
            for row in snapshot["rows"]:
                row[ordinals["requested_backend"]] = "mixed_cpu"
                row[ordinals["actual_backend"]] = "mixed_cpu"
                row[ordinals["simd_execution_stats"]][
                    "requested_minimizer_algorithm"] = requested
            validated.append(self._validate_q8_record(record, {
                "report": "synthetic/fallback.json",
                "raw_sidecar": "synthetic/fallback.raw.jsonl",
                "run_id": "synthetic-fallback",
                "schedule_sorted_checksum": "0" * 16,
            }))
            validated.append(self._validate_q8_record(
                self._raw_record(5, repeat),
                {
                    "report": "synthetic/baseline.json",
                    "raw_sidecar": "synthetic/baseline.raw.jsonl",
                    "run_id": "synthetic-baseline",
                    "schedule_sorted_checksum": "1" * 16,
                },
            ))

        item = validated[0]
        rows = aggregator._aggregate(validated)
        bioseq_row = next(row for row in rows if row["engine"] == "bioseq")
        self.assertEqual(item["record_status"], "accepted")
        self.assertEqual(
            item["transition_classifications"]["minimizer"],
            "documented_minimizer_fallback",
        )
        self.assertEqual(bioseq_row["status"], "accepted")
        self.assertIn("classification=documented_minimizer_fallback", bioseq_row["COMMENTS"])
        self.assertEqual(
            bioseq_row["observed_minimizer_algorithm"], "scalar")

    def test_approved_tuple_yaml_surface_is_closed_and_canonical(self):
        document = self._synthetic_approved_authority()
        raw = aggregator._canonical_approved_tuples_yaml_v3(document)
        parsed = aggregator._parse_approved_tuples_yaml(raw, "synthetic authority")
        self.assertEqual(parsed, document)
        aggregator._validate_approved_tuple_authority_document(parsed, raw)

        mutations = {
            "duplicate_key": raw.replace(
                b"schema: bioseq.approved-e2e-result-tuples\n",
                b"schema: bioseq.approved-e2e-result-tuples\nschema: duplicate\n", 1),
            "tag": raw.replace(b"purpose: ", b"purpose: !fixture ", 1),
            "anchor": raw.replace(b"purpose: ", b"purpose: &fixture ", 1),
            "alias": raw.replace(b"purpose: ", b"purpose: *fixture\nignored: ", 1),
            "flow": raw.replace(b"target_size_gbp:\n", b"target_size_gbp: [2,4]\n", 1),
            "literal_null": raw.replace(b"comments:\n", b"comments: null\n", 1),
            "crlf": raw.replace(b"\n", b"\r\n"),
            "extra_terminal_lf": raw + b"\n",
        }
        for name, mutated in mutations.items():
            with self.subTest(name=name):
                self.assertNotEqual(mutated, raw)
                with self.assertRaises(aggregator.ContractError):
                    aggregator._parse_approved_tuples_yaml(mutated, name)

        legacy, legacy_raw = self._legacy_approved_authority()
        self.assertEqual(len(legacy["approved_results"]), 66)
        self.assertEqual(
            aggregator._approved_projection_sha256(legacy["approved_results"]),
            aggregator.APPROVED_TUPLES_LEGACY_PROJECTION_SHA256)
        self.assertEqual(
            hashlib.sha256(legacy_raw).hexdigest(),
            aggregator.APPROVED_TUPLES_LEGACY_SHA256)
        for name, predicate in {
                "dna4_scalar_fallback": lambda row: (
                    row["alphabet"] == "dna4"
                    and row["execution_conformance"]
                        == "documented_scalar_minimizer_fallback"),
                "user_preserved_path": lambda row: (
                    row["execution_conformance"]
                    == "exact_requested_path_user_preserved"),
        }.items():
            with self.subTest(query_scope_legacy_exception=name):
                legacy_exception = copy.deepcopy(next(
                    row for row in document["approved_results"] if predicate(row)))
                legacy_exception["timing_scope"] = (
                    aggregator.APPROVED_TUPLES_QUERY_TIMING_SCOPE)
                with self.assertRaises(aggregator.ContractError):
                    aggregator._validate_approved_tuple_row(
                        legacy_exception, aggregator.Q1024_CAMPAIGN_ID,
                        document["matrix_scope"],
                        f"query-scope copied {name}",
                        allow_legacy_preservation=True,
                        require_field_order=True)

    def test_remote_authority_bootstrap_is_cpu_only_and_never_rewrites_local_v3(self):
        local, local_raw = self._current_approved_authority()
        historical_sha256 = local["projection_provenance"][
            "historical_v2_preservation"]["source_authority_sha256"]
        legacy_seal = mock.patch.object(
            aggregator, "APPROVED_TUPLES_LEGACY_SHA256", historical_sha256)
        legacy_seal.start()
        self.addCleanup(legacy_seal.stop)
        aggregator._validate_approved_tuple_authority_document(local, local_raw)
        local_sha256 = hashlib.sha256(local_raw).hexdigest()
        expected_machine_policy = {
            "C7G": (
                "i-0cab5f0ab9a1cb002", "c7g.xlarge", "arm64", "aarch64",
                4, 4, 1, 8192, "vol-0a68963a0efa7f81b", 500,
                "native-arm64-clang22-linux-e2e"),
            "M4": (
                "i-09b8f0b9dc8c60564", "m4.xlarge", "x86_64", "x86_64",
                4, 2, 2, 16384, "vol-0d447fc0269e11320", 500,
                "native-x64-clang22-linux-e2e"),
            "M7A": (
                "i-0c724c456e6a87366", "m7a.xlarge", "x86_64", "x86_64",
                4, 4, 1, 16384, "vol-09e1fb2d8a352b673", 500,
                "native-x64-clang22-linux-e2e"),
            "M7I": (
                "i-0e647495fa0691f94", "m7i.xlarge", "x86_64", "x86_64",
                4, 2, 2, 16384, "vol-032e918f7fc9c0763", 500,
                "native-x64-clang22-linux-e2e"),
        }
        for machine_id, expected in expected_machine_policy.items():
            with self.subTest(remote_machine_policy=machine_id):
                policy = aggregator.APPROVED_TUPLES_REMOTE_MACHINE_POLICIES[
                    machine_id]
                self.assertEqual(
                    (policy["instance_id"], policy["instance_type"],
                     policy["ec2_architecture"], policy["architecture"],
                     policy["vcpu_count"], policy["core_count"],
                     policy["threads_per_core"],
                     policy["physical_memory_mib"], policy["volume_id"],
                     policy["capacity_gib"], policy["build_preset"]),
                    expected)
                descriptor = self._synthetic_remote_host_descriptor(machine_id)
                self.assertEqual(
                    aggregator._validate_remote_host_descriptor(
                        descriptor, f"{machine_id} descriptor",
                        require_field_order=True),
                    descriptor)
                self.assertTrue(aggregator._approved_e2e_receipt_preset_matches({
                    "preset_family": "e2e",
                    "preset": policy["build_preset"],
                    "host": descriptor["receipt_host"],
                }))
        wrong_host_preset = self._synthetic_remote_host_descriptor("M7A")
        self.assertFalse(aggregator._approved_e2e_receipt_preset_matches({
            "preset_family": "e2e",
            "preset": "aws-x64-linux-e2e",
            "host": wrong_host_preset["receipt_host"],
        }))
        rejected_old_size = self._synthetic_remote_host_descriptor("M7A")
        rejected_old_size["instance_type"] = "m7a.4xlarge"
        rejected_old_size["descriptor_sha256"] = (
            aggregator._remote_host_descriptor_sha256(rejected_old_size))
        with self.assertRaisesRegex(
                aggregator.ContractError, "closed machine policy"):
            aggregator._validate_remote_host_descriptor(
                rejected_old_size, "obsolete-size descriptor",
                require_field_order=True)
        missing_inventory = self._synthetic_remote_host_descriptor("M7I")
        del missing_inventory["cpu"]
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_remote_host_descriptor(
                missing_inventory, "missing-inventory descriptor",
                require_field_order=False)
        for name, section, field, replacement in (
                ("instance", "aws_identity", "instance_id", "i-00000000000000000"),
                ("cpu", "cpu", "model", "unknown"),
                ("memory", "memory", "physical_mib", 8192),
                ("storage", "storage", "volume_id", "vol-00000000000000000"),
                ("gpu", "gpu", "aws_gpu_info", {"Gpus": 1})):
            with self.subTest(tampered_remote_inventory=name):
                tampered = self._synthetic_remote_host_descriptor("M7I")
                tampered[section][field] = replacement
                tampered["descriptor_sha256"] = (
                    aggregator._remote_host_descriptor_sha256(tampered))
                with self.assertRaises(aggregator.ContractError):
                    aggregator._validate_remote_host_descriptor(
                        tampered, f"tampered-{name} descriptor",
                        require_field_order=True)
        host = self._synthetic_remote_host_descriptor("M7A")
        seed = aggregator._new_remote_approved_tuple_authority(
            local, local_sha256, host)
        expected_rows = aggregator._remote_eligible_rows_from_local_authority(
            local["approved_results"])
        cpu_identity = copy.deepcopy(aggregator._approved_row_identity(next(
            row for row in expected_rows
            if not aggregator._remote_identity_requires_vulkan(
                aggregator._approved_row_identity(row)))))
        for field in (
                "requested_minimizer_algorithm",
                "requested_chaining_algorithm",
                "requested_base_alignment_algorithm"):
            with self.subTest(requested_gpu_stage=field):
                identity = copy.deepcopy(cpu_identity)
                identity[field] = "vulkan_test_stage"
                self.assertTrue(
                    aggregator._remote_identity_requires_vulkan(identity))
        baseline_row = copy.deepcopy(next(
            row for row in expected_rows if row["pipeline"] == "minimap2"))
        baseline_row["timing_scope"] = (
            aggregator.APPROVED_TUPLES_QUERY_TIMING_SCOPE)
        baseline_row["metrics"] = {"unit": "ns", "p50_a": 110, "p50_b": 100}
        baseline_row = aggregator._approved_row_in_fixed_order(baseline_row)
        candidate = self._remoteize_candidate(
            self._synthetic_candidate(
                local, [baseline_row], authority_raw=local_raw), seed)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            local_path = root / "ALL_RESULTS_TUPLES.yaml"
            local_path.write_bytes(local_raw)
            descriptor_path = root / "remote_host.json"
            descriptor_path.write_text(
                json.dumps(host, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8")
            candidate_path = root / "approved_remote_baseline_candidate.json"
            candidate_path.write_text(
                json.dumps(candidate, sort_keys=True, separators=(",", ":"))
                + "\n", encoding="utf-8")
            remote_path = root / "ALL_RESULTS_TUPLES_REMOTE_M7A.yaml"
            receipt = aggregator.bootstrap_remote_approved_tuples(
                remote_path, local_path, local_sha256, descriptor_path,
                candidate_path)
            self.assertEqual(local_path.read_bytes(), local_raw)
            self.assertEqual(receipt["machine_id"], "M7A")
            self.assertEqual(receipt["approved_result_count"], 1)
            self.assertEqual(stat.S_IMODE(remote_path.stat().st_mode), 0o600)
            remote_raw = remote_path.read_bytes()
            remote = aggregator._parse_approved_tuples_yaml(
                remote_raw, "remote authority")
            aggregator._validate_approved_tuple_authority_document(
                remote, remote_raw)
            self.assertEqual(
                remote["eligible_stable_keys"],
                [row["stable_key"] for row in expected_rows])
            self.assertEqual(
                len(remote["eligible_stable_keys"]),
                len({row["stable_key"] for row in local["approved_results"]}))
            self.assertEqual(
                [row["stable_key"]
                 for row in remote["hardware_unavailable_cells"]],
                [row["stable_key"] for row in expected_rows
                 if aggregator._remote_identity_requires_vulkan(
                     aggregator._approved_row_identity(row))])
            self.assertTrue(all(
                row["run_count"] == 0
                and row["metrics"] == {
                    "unit": "ns", "p50_a": None, "p50_b": None}
                and row["execution_attempted"] is False
                for row in remote["hardware_unavailable_cells"]))
            stable_remote = remote_path.read_bytes()
            with self.assertRaises(aggregator.ApprovedTupleIOError):
                aggregator.bootstrap_remote_approved_tuples(
                    remote_path, local_path, local_sha256, descriptor_path,
                    candidate_path)
            self.assertEqual(remote_path.read_bytes(), stable_remote)

    def test_remote_candidate_binds_host_allowlist_and_preapproved_baseline(self):
        local, local_raw = self._current_approved_authority()
        historical_sha256 = local["projection_provenance"][
            "historical_v2_preservation"]["source_authority_sha256"]
        legacy_seal = mock.patch.object(
            aggregator, "APPROVED_TUPLES_LEGACY_SHA256", historical_sha256)
        legacy_seal.start()
        self.addCleanup(legacy_seal.stop)
        aggregator._validate_approved_tuple_authority_document(local, local_raw)
        host = self._synthetic_remote_host_descriptor("M7A")
        remote = aggregator._new_remote_approved_tuple_authority(
            local, hashlib.sha256(local_raw).hexdigest(), host)
        eligible_rows = aggregator._remote_eligible_rows_from_local_authority(
            local["approved_results"])
        eligible_by_key = {row["stable_key"]: row for row in eligible_rows}
        cpu_row = copy.deepcopy(next(
            row for row in eligible_rows
            if row["pipeline"] == "bioseq"
            and row["execution_conformance"] == "exact_requested_path"
            and not aggregator._remote_identity_requires_vulkan(
                aggregator._approved_row_identity(row))
            and aggregator._remote_baseline_stable_key(row) in eligible_by_key))
        baseline_row = copy.deepcopy(
            eligible_by_key[aggregator._remote_baseline_stable_key(cpu_row)])
        for row in (baseline_row, cpu_row):
            row["timing_scope"] = aggregator.APPROVED_TUPLES_QUERY_TIMING_SCOPE
            row["metrics"] = {"unit": "ns", "p50_a": 110, "p50_b": 100}
        baseline_row = aggregator._approved_row_in_fixed_order(baseline_row)
        cpu_row = aggregator._approved_row_in_fixed_order(cpu_row)

        baseline_candidate = self._remoteize_candidate(
            self._synthetic_candidate(local, [baseline_row],
                                      authority_raw=local_raw), remote)
        remote_sha256 = aggregator._remote_bootstrap_seed_sha256(remote)
        _, baseline_merge = aggregator._validate_approved_append_candidate_document(
            baseline_candidate, remote, remote_sha256)
        aggregator._regenerate_remote_approved_tuple_projections(
            remote, baseline_merge)
        remote_raw = aggregator._canonical_approved_tuples_yaml_v3(remote)
        aggregator._validate_approved_tuple_authority_document(remote, remote_raw)

        cpu_candidate = self._remoteize_candidate(
            self._synthetic_candidate(local, [cpu_row], authority_raw=local_raw),
            remote)
        remote_sha256 = hashlib.sha256(remote_raw).hexdigest()
        candidate_rows, merged = (
            aggregator._validate_approved_append_candidate_document(
                cpu_candidate, remote, remote_sha256))
        self.assertEqual(candidate_rows, [cpu_row])
        self.assertEqual(len(merged), 2)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            authority_path = root / "ALL_RESULTS_TUPLES_REMOTE_M7A.yaml"
            authority_path.write_bytes(remote_raw)
            candidate_path = root / "approved_remote_cpu_candidate.json"
            candidate_path.write_text(
                json.dumps(cpu_candidate, sort_keys=True, separators=(",", ":"))
                + "\n", encoding="utf-8")
            receipt = aggregator.append_approved_tuples(
                authority_path, remote_sha256, candidate_path)
            self.assertEqual(receipt["approved_result_count"], 2)
            self.assertEqual(receipt["machine_id"], "M7A")
            appended_raw = authority_path.read_bytes()
            appended = aggregator._parse_approved_tuples_yaml(
                appended_raw, "appended remote authority")
            aggregator._validate_approved_tuple_authority_document(
                appended, appended_raw)
            self.assertEqual(appended["approved_results"], merged)
            self.assertEqual(stat.S_IMODE(authority_path.stat().st_mode), 0o600)
            self.assertEqual(
                (root / ".ALL_RESULTS_TUPLES_REMOTE_M7A.yaml.lock").stat().st_size,
                0)

        missing_baseline = aggregator._new_remote_approved_tuple_authority(
            local, hashlib.sha256(local_raw).hexdigest(), host)
        rebound = self._remoteize_candidate(
            self._synthetic_candidate(local, [cpu_row], authority_raw=local_raw),
            missing_baseline)
        with self.assertRaisesRegex(
                aggregator.ApprovedTupleProofError, "same-host minimap2 baseline"):
            aggregator._validate_approved_append_candidate_document(
                rebound, missing_baseline,
                aggregator._remote_bootstrap_seed_sha256(missing_baseline))

        wrong_host = copy.deepcopy(cpu_candidate)
        wrong_host["remote_host_binding"]["machine_id"] = "M4"
        self._reseal_synthetic_candidate(wrong_host)
        with self.assertRaisesRegex(
                aggregator.ApprovedTupleProofError, "different remote host"):
            aggregator._validate_approved_append_candidate_document(
                wrong_host, remote, remote_sha256)

        wrong_preset = copy.deepcopy(cpu_candidate)
        wrong_preset["verification_manifest"]["proofs"]["build"][
            "payload"]["preset"] = "unlisted-remote-e2e"
        self._reseal_synthetic_candidate(wrong_preset)
        with self.assertRaisesRegex(
                aggregator.ApprovedTupleProofError, "remote authority host"):
            aggregator._validate_approved_append_candidate_document(
                wrong_preset, remote, remote_sha256)
        self.assertEqual(
            aggregator._canonical_approved_tuples_yaml_v3(local), local_raw)

    def test_validate_approved_tuples_cli_is_read_only_and_reports_exact_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            authority_path, authority_raw, _, candidate_path, _ = (
                self._write_synthetic_approved_inputs(directory))
            expected_sha = hashlib.sha256(authority_raw).hexdigest()
            stdout = io.StringIO()
            stderr = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                exit_code = aggregator.main([
                    "validate-approved-tuples",
                    "--authority", str(authority_path),
                    "--expected-sha256", expected_sha,
                ])
            self.assertEqual(exit_code, 0, stderr.getvalue())
            receipt = json.loads(stdout.getvalue())
            self.assertEqual(receipt["sha256"], expected_sha)
            self.assertEqual(receipt["byte_count"], len(authority_raw))
            self.assertEqual(
                receipt["lf_terminated_line_count"], authority_raw.count(b"\n"))
            self.assertFalse((Path(directory) / ".ALL_RESULTS_TUPLES.yaml.lock").exists())
            self.assertEqual(authority_path.read_bytes(), authority_raw)
            with contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                candidate_exit_code = aggregator.main([
                    "validate-append-candidate",
                    "--candidate", str(candidate_path),
                    "--authority", str(authority_path),
                    "--expected-sha256", expected_sha,
                ])
            self.assertEqual(candidate_exit_code, 0)
            self.assertFalse((Path(directory) / ".ALL_RESULTS_TUPLES.yaml.lock").exists())
            self.assertEqual(authority_path.read_bytes(), authority_raw)

    def test_approved_subcommand_dispatch_preserves_legacy_cli_shape(self):
        args = aggregator._arguments([
            "--report", "report.json",
            "--config", "config.json",
            "--all-results", "ALL_RESULTS.yaml",
            "--run-receipt-id", "rr_synthetic",
        ])
        self.assertIsNone(args.approved_command)
        self.assertEqual(args.report, "report.json")
        self.assertEqual(args.config, "config.json")
        self.assertEqual(args.all_results, "ALL_RESULTS.yaml")
        self.assertEqual(args.run_receipt_id, "rr_synthetic")

    def test_approved_gate8_v2_binds_one_source_to_five_actual_repeats(self):
        authority = self._synthetic_approved_authority()
        authority_sha = hashlib.sha256(
            aggregator._canonical_approved_tuples_yaml_v3(authority)).hexdigest()
        candidate = self._upgrade_candidate_gate8_v2(
            self._synthetic_candidate(authority))
        candidate_rows, _ = aggregator._validate_approved_append_candidate_document(
            candidate, authority, authority_sha)
        self.assertEqual(len(candidate_rows), 1)

        def gate8(value):
            return value["verification_manifest"]["proofs"][
                "measurement_contract"]["payload"]["rows"][0]["gate8_sql"]

        payloads = {
            name: proof["payload"]
            for name, proof in candidate["verification_manifest"][
                "proofs"].items()
        }
        execution_manifest = self._candidate_execution_manifest(candidate)
        records, source, build_receipt = (
            self._synthetic_validated_gate8_v2_records(
                candidate["candidate_rows"][0], 0, payloads,
                execution_manifest["rung"]["fixture_projection_sha256"]))
        aggregator._validate_gate8_cell_qualifications(
            records, source, build_receipt)
        projected = aggregator._project_approved_gate8_sql_v2(
            records, candidate["timing_scope"])
        self.assertEqual(gate8(candidate), projected)
        raw_qualification = records[0]["gate8_sql"]["qualification"]
        raw_fixture = raw_qualification["query_fixture"]
        wrapper = {
            "dataset_key": raw_qualification["runtime_cell"]["dataset_key"],
            "target_state": raw_qualification["runtime_cell"]["target_state"],
            "target_table": raw_qualification["runtime_cell"]["target_table"],
            "query_fixture": raw_fixture,
        }
        leaf_sha256 = hashlib.sha256(
            aggregator._canonical_bytes(raw_fixture)).hexdigest()
        wrapper_sha256 = hashlib.sha256(
            aggregator._canonical_bytes(wrapper)).hexdigest()
        projected_qualification = projected["qualification"]
        self.assertNotEqual(wrapper_sha256, leaf_sha256)
        self.assertEqual(
            projected_qualification["qualification_key"][
                "fixture_qualification_sha256"], wrapper_sha256)
        self.assertEqual(
            projected_qualification["query_fixture_sha256"], leaf_sha256)
        self.assertTrue(all(
            binding["current_query_fixture_sha256"] == leaf_sha256
            for binding in projected["bindings"]))
        raw_measurement = records[2]["gate8_sql"]["measurement"]
        projected_binding = projected["bindings"][2]
        self.assertEqual(
            projected_binding["current_measurement_sha256"],
            hashlib.sha256(aggregator._canonical_bytes(
                raw_measurement)).hexdigest())
        self.assertEqual(
            projected_binding["measurement_sql_sha256"],
            hashlib.sha256(raw_measurement["sql"].encode("utf-8")).hexdigest())
        self.assertEqual(
            projected_binding["pre_statement_gucs_sha256"],
            hashlib.sha256(aggregator._canonical_bytes(
                raw_measurement["pre_statement_gucs"])).hexdigest())
        self.assertEqual(
            projected_binding["ordered_primary_counts_sha256"],
            hashlib.sha256(aggregator._canonical_bytes(
                raw_measurement["ordered_primary_match_counts"])).hexdigest())
        self.assertEqual(
            projected_qualification["explain_json_sha256"],
            hashlib.sha256(raw_qualification["detached"][
                "explain_json_text"].encode("utf-8")).hexdigest())

        def reseal_gate8_key(value):
            proof = gate8(value)
            key_sha256 = hashlib.sha256(aggregator._canonical_bytes(
                proof["qualification"]["qualification_key"])).hexdigest()
            proof["qualification"]["qualification_key_sha256"] = key_sha256
            for binding in proof["bindings"]:
                binding["qualification_key_sha256"] = key_sha256

        def key_drift(field, replacement):
            def mutate(value):
                gate8(value)["qualification"]["qualification_key"][field] = replacement
                reseal_gate8_key(value)
            return mutate

        mutations = {
            "missing_current_binding": lambda value: gate8(value)["bindings"].pop(),
            "duplicate_source_role": lambda value: gate8(value)["bindings"][1].update({
                "role": "source", "source_epoch_is_current": True}),
            "replayed_current_epoch": lambda value: gate8(value)["bindings"][2][
                "current_postgres"].__setitem__(
                    "postmaster_start_time", "synthetic-epoch-0-1"),
            "cross_cell_source": lambda value: gate8(value)["bindings"][3][
                "source_postgres"].__setitem__("database_oid", 201),
            "qualification_key_seal": lambda value: gate8(value)["qualification"].__setitem__(
                "qualification_key_sha256", "f" * 64),
            "config_binding": key_drift("config_sha256", "c" * 64),
            "build_binding": key_drift("build_receipt_sha256", "d" * 64),
            "fixture_binding": key_drift(
                "fixture_projection_sha256", "e" * 64),
            "current_fixture_provenance": lambda value: gate8(value)[
                "bindings"][2].__setitem__(
                    "current_query_fixture_sha256", "f" * 64),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                bad = copy.deepcopy(candidate)
                mutate(bad)
                self._reseal_synthetic_candidate(bad)
                with self.assertRaises(aggregator.ApprovedTupleProofError):
                    aggregator._validate_approved_append_candidate_document(
                        bad, authority, authority_sha)

    def test_gate8_report_version_uniformity_is_local(self):
        authority = self._synthetic_approved_authority()
        candidate = self._synthetic_candidate(authority)
        payloads = {
            name: proof["payload"]
            for name, proof in candidate["verification_manifest"][
                "proofs"].items()
        }
        execution_manifest = self._candidate_execution_manifest(candidate)
        records, source, build_receipt = (
            self._synthetic_validated_gate8_v2_records(
                candidate["candidate_rows"][0], 0, payloads,
                execution_manifest["rung"]["fixture_projection_sha256"]))

        def as_v1(proof):
            return {
                "schema": proof["schema"],
                "version": 1,
                "scope": proof["scope"],
                "measurement": copy.deepcopy(proof["measurement"]),
                "query_fixture": copy.deepcopy(proof["query_fixture"]),
                "detached": copy.deepcopy(proof["qualification"]["detached"]),
            }

        all_v1 = copy.deepcopy(records)
        for item in all_v1:
            item["gate8_sql"] = as_v1(item["gate8_sql"])
        v1_source = copy.deepcopy(source)
        v1_source["report_sha256"] = "5" * 64
        aggregator._validate_gate8_cell_qualifications(
            all_v1, v1_source, build_receipt)
        self.assertTrue(all(
            "gate8_qualification_binding" not in item for item in all_v1))

        all_v2 = copy.deepcopy(records)
        aggregator._validate_gate8_cell_qualifications(
            all_v2, source, build_receipt)
        self.assertTrue(all(
            item["gate8_qualification_binding"]["version"] == 2
            for item in all_v2))

        mixed = copy.deepcopy(records)
        mixed[-1]["gate8_sql"] = as_v1(mixed[-1]["gate8_sql"])
        with self.assertRaisesRegex(
                aggregator.ContractError, "may not mix Gate-8 v1 and v2"):
            aggregator._validate_gate8_cell_qualifications(
                mixed, source, build_receipt)

    def test_approved_candidate_recomputes_repeat_math_match_parity_and_bindings(self):
        authority = self._synthetic_approved_authority()
        authority_sha = hashlib.sha256(
            aggregator._canonical_approved_tuples_yaml_v3(authority)).hexdigest()
        candidate = self._synthetic_candidate(authority)
        candidate_rows, merged_rows = (
            aggregator._validate_approved_append_candidate_document(
                candidate, authority, authority_sha))
        self.assertEqual(len(candidate_rows), 1)
        self.assertEqual(
            len(merged_rows), len(authority["approved_results"]) + 1)

        def chunk_repeat(value):
            return value["verification_manifest"]["proofs"]["chunk_plan"][
                "payload"]["rows"][0]["repeats"][0]

        def gate8(value):
            return value["verification_manifest"]["proofs"][
                "measurement_contract"]["payload"]["rows"][0]["gate8_sql"]

        mutations = {
            "boolean_gate8_version": lambda value: gate8(value).__setitem__("version", True),
            "boolean_gate8_repeat": lambda value: gate8(value)["repeats"][0].__setitem__(
                "repeat", False),
            "boolean_gate8_scan_count": lambda value: gate8(value)["repeats"][0].__setitem__(
                "custom_scan_node_count", True),
            "fractional_gate8_guc_count": lambda value: gate8(value)["repeats"][0].__setitem__(
                "complete_guc_count", 22.0),
            "boolean_gate8_prefilter_count": lambda value: gate8(value)["repeats"][0].__setitem__(
                "prefilter_activity_count", False),
            "boolean_gate8_cache_count": lambda value: gate8(value)["repeats"][0][
                "cache_evidence"].__setitem__("gpu_persisted_resident_cache_hits", False),
            "wrong_execution_rung": lambda value: value["verification_manifest"]["proofs"][
                "config"]["payload"]["execution_contract"].__setitem__(
                    "identity", aggregator._observed_rung_execution_contract_id(512)),
            "forged_execution_digest": lambda value: value["verification_manifest"]["proofs"][
                "config"]["payload"]["execution_contract"].__setitem__(
                    "sha256", "f" * 64),
            "B_not_A_minus_union": lambda value: chunk_repeat(value).__setitem__(
                "B_ns", chunk_repeat(value)["B_ns"] + 1),
            "third_sorted_p50": lambda value: (
                chunk_repeat(value).__setitem__("A_ns", 200),
                chunk_repeat(value).__setitem__("B_ns", 190)),
            "match_vector_mismatch": lambda value: chunk_repeat(value).__setitem__(
                "baseline_match_vector_sha256", "f" * 64),
            "match_total_mismatch": lambda value: chunk_repeat(value).__setitem__(
                "baseline_match_total", 18),
            "repeat_missing": lambda value: value["verification_manifest"]["proofs"][
                "chunk_plan"]["payload"]["rows"][0]["repeats"].pop(),
            "repeat_cross_identity": lambda value: chunk_repeat(value).__setitem__(
                "repeat_identity", "another-cell:repeat:0"),
            "query_partition_drift": lambda value: value["verification_manifest"][
                "proofs"]["chunk_plan"]["payload"]["rows"][0]["repeats"][1][
                    "query_partition"].__setitem__("identity", "different-partition"),
            "physical_fallback": lambda value: value["verification_manifest"][
                "proofs"]["measurement_contract"]["payload"]["rows"][0].__setitem__(
                    "backend_fallbacks", 1),
            "gate8_explain_provider": lambda value: value["verification_manifest"][
                "proofs"]["measurement_contract"]["payload"]["rows"][0][
                    "gate8_sql"]["repeats"][0].__setitem__(
                        "custom_plan_provider", "name-only"),
            "binding_version": lambda value: value["verification_manifest"][
                "row_bindings"][0]["proofs"]["source"].__setitem__("version", 99),
            "proof_timing_scope": lambda value: value["verification_manifest"][
                "proofs"]["source"]["payload"].__setitem__(
                    "timing_scope", aggregator.APPROVED_TUPLES_HISTORICAL_TIMING_SCOPE),
            "binding_timing_scope": lambda value: value["verification_manifest"][
                "row_bindings"][0].__setitem__(
                    "timing_scope", aggregator.APPROVED_TUPLES_HISTORICAL_TIMING_SCOPE),
            "external_proof_path": lambda value: value["verification_manifest"][
                "proofs"]["source"]["payload"].__setitem__("report_path", "elsewhere"),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                bad = copy.deepcopy(candidate)
                mutate(bad)
                self._reseal_synthetic_candidate(bad)
                with self.assertRaises(aggregator.ApprovedTupleProofError):
                    aggregator._validate_approved_append_candidate_document(
                        bad, authority, authority_sha)

        def tamper_generator(manifest):
            manifest["source"]["generator"] = "bench/foreign_generator.py"

        def mismatch_config(manifest):
            manifest["source"]["full_config_sha256"] = "b" * 64

        def mismatch_rung(manifest):
            manifest["rung"]["query_count"] = 512

        for name, mutate_manifest in {
                "tampered_manifest_contents": tamper_generator,
                "mismatched_manifest_config": mismatch_config,
                "mismatched_manifest_rung": mismatch_rung,
                "boolean_rung_version": lambda manifest: manifest["rung"].__setitem__(
                    "version", True),
                "boolean_rung_ordinal": lambda manifest: manifest["rung"][
                    "query_ordinal_interval"].__setitem__("begin", False),
        }.items():
            with self.subTest(name=name):
                bad = copy.deepcopy(candidate)
                manifest = self._candidate_execution_manifest(bad)
                mutate_manifest(manifest)
                self._replace_candidate_execution_manifest(bad, manifest)
                self._reseal_synthetic_candidate(bad)
                with self.assertRaises(aggregator.ApprovedTupleProofError):
                    aggregator._validate_approved_append_candidate_document(
                        bad, authority, authority_sha)

        resealed_config_drift = copy.deepcopy(candidate)
        projected_config = self._candidate_observed_config(resealed_config_drift)
        projected_config["test_cases"]["M2_D4_K15_W10"]["data"]["seed"] += 1
        self._replace_candidate_observed_config(
            resealed_config_drift, projected_config)
        self._reseal_synthetic_candidate(resealed_config_drift)
        with self.assertRaises(aggregator.ApprovedTupleProofError):
            aggregator._validate_approved_append_candidate_document(
                resealed_config_drift, authority, authority_sha)

        empty = copy.deepcopy(candidate)
        empty["candidate_rows"] = []
        self._reseal_synthetic_candidate(empty)
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_approved_append_candidate_document(
                empty, authority, authority_sha)
        unknown = copy.deepcopy(candidate)
        unknown["candidate_rows"][0]["receipt_refs"] = []
        self._reseal_synthetic_candidate(unknown)
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_approved_append_candidate_document(
                unknown, authority, authority_sha)
        ambiguous_v1 = copy.deepcopy(candidate)
        ambiguous_v1["version"] = 1
        self._reseal_synthetic_candidate(ambiguous_v1)
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_approved_append_candidate_document(
                ambiguous_v1, authority, authority_sha)

    def test_append_cli_roundtrip_lock_mode_and_replay_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            authority_path, authority_raw, authority, candidate_path, candidate = (
                self._write_synthetic_approved_inputs(directory, legacy=True))
            before_sha = hashlib.sha256(authority_raw).hexdigest()
            stdout = io.StringIO()
            stderr = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                exit_code = aggregator.main([
                    "append-approved-tuples",
                    "--candidate", str(candidate_path),
                    "--authority", str(authority_path),
                    "--expected-sha256", before_sha,
                ])
            self.assertEqual(exit_code, 0, stderr.getvalue())
            receipt = json.loads(stdout.getvalue())
            final_raw = authority_path.read_bytes()
            self.assertEqual(receipt["sha256"], hashlib.sha256(final_raw).hexdigest())
            self.assertEqual(receipt["approved_results_projection_sha256"],
                             candidate["candidate_projection_sha256"])
            final_document = aggregator._parse_approved_tuples_yaml(
                final_raw, "synthetic final authority")
            self.assertEqual(final_document["version"], aggregator.APPROVED_TUPLES_VERSION)
            self.assertEqual(
                len(final_document["approved_results"]),
                len(authority["approved_results"]) + 1)
            candidate_key = candidate["candidate_rows"][0]["stable_key"]
            self.assertEqual(sum(
                row["stable_key"] == candidate_key
                for row in final_document["approved_results"]), 2)
            self.assertEqual(
                aggregator._approved_projection_sha256(
                    aggregator._approved_historical_v2_rows(
                        final_document["approved_results"])),
                aggregator.APPROVED_TUPLES_LEGACY_PROJECTION_SHA256)
            self.assertEqual(
                aggregator._approved_historical_v2_rows(
                    final_document["approved_results"]),
                authority["approved_results"])
            overall = final_document["corrected_summaries"]["overall"]
            self.assertEqual(overall["approved_logical_cells"], 66)
            self.assertEqual(overall["approved_rows"], 67)
            self.assertEqual(overall["approved_valid_runs"], 335)
            self.assertEqual(
                overall["missing_cells"],
                authority["corrected_summaries"]["overall"]["missing_cells"])
            self.assertEqual(overall["query_only_approved_cells"], 1)
            self.assertEqual(overall["query_only_missing_cells"], 1295)
            self.assertEqual(overall["planned_valid_runs_still_needed"], 6475)
            self.assertEqual(final_raw,
                             aggregator._canonical_approved_tuples_yaml_v3(final_document))
            lock_path = Path(directory) / ".ALL_RESULTS_TUPLES.yaml.lock"
            self.assertTrue(lock_path.exists())
            self.assertEqual(stat.S_IMODE(lock_path.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(authority_path.stat().st_mode), 0o600)

            # An exact old envelope is stale after the successful transaction.
            stable_final = final_raw
            with contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                replay_code = aggregator.main([
                    "append-approved-tuples",
                    "--candidate", str(candidate_path),
                    "--authority", str(authority_path),
                    "--expected-sha256", before_sha,
                ])
            self.assertEqual(replay_code, 4)
            self.assertEqual(authority_path.read_bytes(), stable_final)

            # Even a freshly rebound envelope cannot replay the same observation pair.
            rebound = copy.deepcopy(candidate)
            final_sha = hashlib.sha256(final_raw).hexdigest()
            rebound["authority_before_sha256"] = final_sha
            rebound["candidate_projection_sha256"] = "a" * 64
            self._reseal_synthetic_candidate(rebound)
            candidate_path.write_text(
                json.dumps(rebound, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                duplicate_code = aggregator.main([
                    "append-approved-tuples",
                    "--candidate", str(candidate_path),
                    "--authority", str(authority_path),
                    "--expected-sha256", final_sha,
                ])
            self.assertEqual(duplicate_code, 3)
            self.assertEqual(authority_path.read_bytes(), stable_final)

    def test_append_pre_replace_proof_failure_preserves_exact_authority_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            authority_path, authority_raw, _, candidate_path, candidate = (
                self._write_synthetic_approved_inputs(directory))
            repeat = candidate["verification_manifest"]["proofs"]["chunk_plan"][
                "payload"]["rows"][0]["repeats"][2]
            repeat["B_ns"] += 1
            self._reseal_synthetic_candidate(candidate)
            candidate_path.write_text(
                json.dumps(candidate, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                exit_code = aggregator.main([
                    "append-approved-tuples",
                    "--candidate", str(candidate_path),
                    "--authority", str(authority_path),
                    "--expected-sha256", hashlib.sha256(authority_raw).hexdigest(),
                ])
            self.assertEqual(exit_code, 4)
            self.assertEqual(authority_path.read_bytes(), authority_raw)
            self.assertEqual(
                list(Path(directory).glob(".ALL_RESULTS_TUPLES.yaml.*.tmp")), [])

    def test_temporary_byte_validation_failure_is_atomic_and_cleans_temporary(self):
        with tempfile.TemporaryDirectory() as directory:
            authority_path, authority_raw, authority, _, candidate = (
                self._write_synthetic_approved_inputs(directory))
            _, merged_rows = aggregator._validate_approved_append_candidate_document(
                candidate, authority, hashlib.sha256(authority_raw).hexdigest())
            merged = copy.deepcopy(authority)
            aggregator._regenerate_approved_tuple_projections(merged, merged_rows)
            original_reader = aggregator._read_approved_tuple_file

            def corrupt_temporary(path, label):
                raw = original_reader(path, label)
                return raw + b" " if label == "approved tuple temporary" else raw

            aggregator._read_approved_tuple_file = corrupt_temporary
            try:
                with aggregator._exclusive_results_lock(
                        authority_path, approved_tuples=True) as lock_descriptor:
                    with self.assertRaises(aggregator.ContractError):
                        aggregator._atomic_write_approved_tuples(
                            authority_path,
                            hashlib.sha256(authority_raw).hexdigest(), merged,
                            candidate=candidate, prior_authority=authority,
                            lock_descriptor=lock_descriptor)
            finally:
                aggregator._read_approved_tuple_file = original_reader
            self.assertEqual(authority_path.read_bytes(), authority_raw)
            self.assertEqual(
                list(Path(directory).glob(".ALL_RESULTS_TUPLES.yaml.*.tmp")), [])

    @classmethod
    def _approved_row_from_historical_identity(cls, historical):
        row = cls._approved_tuple_row(
            target=historical["target_size_gbp"], alphabet=historical["alphabet"],
            kmer=historical["kmer_size"], window=historical["window_size"],
            mode=historical["mode"], p50_a=110, p50_b=100)
        row["index_type"] = historical["index_type"]
        row["requested_algorithms"] = copy.deepcopy(historical["requested_algorithms"])
        row["observed_algorithms"] = copy.deepcopy(row["requested_algorithms"])
        row["effective_path"] = " -> ".join(row["observed_algorithms"].values())
        row["stable_key"] = aggregator._stable_key_for_identity(
            aggregator._approved_row_identity(row))
        return row

    def test_repaired_rejected_and_invalid_cells_append_without_double_counting(self):
        historical = aggregator._approved_historical_nonvalid_rows()
        self.assertEqual(len(historical), 48)
        self.assertEqual(len({row["stable_key"] for row in historical}), 48)
        self.assertEqual(sum(row["status"] == "rejected" for row in historical), 47)
        for disposition in ("rejected", "invalid"):
            with self.subTest(prior_disposition=disposition):
                authority = self._synthetic_approved_authority()
                old = next(row for row in historical if row["status"] == disposition)
                row = self._approved_row_from_historical_identity(old)
                candidate = self._synthetic_candidate(authority, [row])
                chunk = candidate["verification_manifest"]["proofs"]["chunk_plan"][
                    "payload"]["rows"][0]
                _, merged_rows = aggregator._validate_approved_append_candidate_document(
                    candidate, authority, candidate["authority_before_sha256"])
                before = authority["corrected_summaries"]["overall"]
                was_logically_approved = row["stable_key"] in {
                    existing["stable_key"] for existing in authority["approved_results"]}
                merged = copy.deepcopy(authority)
                aggregator._regenerate_approved_tuple_projections(merged, merged_rows)
                aggregator._validate_approved_tuple_authority_document(merged)
                after = merged["corrected_summaries"]["overall"]
                self.assertEqual(after["approved_rows"], before["approved_rows"] + 1)
                self.assertEqual(after["materialized_cells"], before["materialized_cells"])
                self.assertEqual(after["missing_cells"], before["missing_cells"])
                self.assertEqual(
                    after["historically_nonvalid_now_approved_cells"],
                    before["historically_nonvalid_now_approved_cells"]
                    + (0 if was_logically_approved else 1))
                self.assertEqual(after["recorded_attempts"], before["recorded_attempts"] + 5)
                self.assertEqual(merged["excluded_and_nonvalid_summary"]["rejected"]["recorded_attempts"], 235)
                self.assertEqual(merged["excluded_and_nonvalid_summary"]["invalid"]["recorded_attempts"], 5)
                chunk["prior_disposition"] = disposition
                self._reseal_synthetic_candidate(candidate)
                with self.assertRaises(aggregator.ApprovedTupleProofError):
                    aggregator._validate_approved_append_candidate_document(
                        candidate, authority, candidate["authority_before_sha256"])

    def test_missing_cell_cannot_forge_rejected_or_invalid_prior_disposition(self):
        authority = self._synthetic_approved_authority()
        historical_keys = {
            row["stable_key"] for row in aggregator._approved_historical_nonvalid_rows()}
        for disposition in ("rejected", "invalid"):
            with self.subTest(prior_disposition=disposition):
                candidate = self._synthetic_candidate(authority)
                self.assertNotIn(candidate["candidate_rows"][0]["stable_key"], historical_keys)
                chunk = candidate["verification_manifest"]["proofs"]["chunk_plan"][
                    "payload"]["rows"][0]
                chunk["prior_disposition"] = disposition
                self._reseal_synthetic_candidate(candidate)
                with self.assertRaises(aggregator.ApprovedTupleProofError):
                    aggregator._validate_approved_append_candidate_document(
                        candidate, authority, candidate["authority_before_sha256"])

    def test_complete_matrix_remains_possible_after_historical_failures(self):
        authority = self._synthetic_approved_authority()
        self.assertFalse(authority["authority"]["full_matrix_complete"])
        self.assertEqual(
            authority["corrected_summaries"]["overall"]["query_only_missing_cells"],
            1296)
        matrix = authority["matrix_scope"]
        rows = copy.deepcopy(authority["approved_results"])
        for target in matrix["target_size_gbp"]:
            for alphabet in matrix["alphabets"]:
                for pair in matrix["window_kmer_pairs"]:
                    for mode in matrix["modes"]:
                        template = self._approved_tuple_row(
                            target=target, alphabet=alphabet,
                            kmer=pair["kmer_size"], window=pair["window_size"], mode=mode)
                        for index in matrix["bioseq_index_types"]:
                            for seed in matrix["bioseq_minimizer_algorithms"]:
                                for chain in matrix["bioseq_chaining_algorithms"]:
                                    for base in matrix["bioseq_base_alignment_algorithms"]:
                                        row = copy.deepcopy(template)
                                        row["index_type"] = index
                                        row["requested_algorithms"] = {
                                            "minimizer": seed, "chaining": chain,
                                            "base_alignment": base}
                                        row["observed_algorithms"] = copy.deepcopy(row["requested_algorithms"])
                                        if alphabet == "dna5" and seed == "simd_minimap2_reference":
                                            row["observed_algorithms"]["minimizer"] = "scalar"
                                            row["execution_conformance"] = "documented_scalar_minimizer_fallback"
                                            row["comparison_eligibility"] = {
                                                "requested_path": False, "effective_path": True,
                                                "scope": "planned_dna5_reference_scalar_only"}
                                            row["comments"] = "DNA5 simd_minimap2_reference binds scalar; not SIMD performance."
                                        row["effective_path"] = " -> ".join(row["observed_algorithms"].values())
                                        row["stable_key"] = aggregator._stable_key_for_identity(
                                            aggregator._approved_row_identity(row))
                                        rows.append(row)
                        baseline = copy.deepcopy(template)
                        baseline.update({
                            "pipeline": "minimap2", "pipeline_mode": None,
                            "index_type": "minimap2_internal", "effective_path": "minimap2",
                            "execution_conformance": "external_minimap2_baseline",
                            "comparison_eligibility": {
                                "requested_path": True, "effective_path": True,
                                "scope": "external_minimap2_baseline"}})
                        for field in ("requested_algorithms", "observed_algorithms"):
                            baseline[field] = dict.fromkeys(aggregator.APPROVED_TUPLES_ALGORITHM_FIELDS)
                        baseline["stable_key"] = aggregator._stable_key_for_identity(
                            aggregator._approved_row_identity(baseline))
                        rows.append(baseline)
        rows.sort(key=lambda row: row["stable_key"])
        aggregator._regenerate_approved_tuple_projections(authority, rows)
        aggregator._validate_approved_tuple_authority_document(authority)
        overall = authority["corrected_summaries"]["overall"]
        self.assertEqual(overall["approved_rows"], 1296 + 66)
        self.assertEqual(overall["approved_valid_runs"], 6480 + 330)
        self.assertEqual(overall["approved_logical_cells"], 1296)
        self.assertEqual(overall["query_only_approved_cells"], 1296)
        self.assertEqual(overall["query_only_contributing_runs"], 6480)
        self.assertEqual(overall["materialized_cells"], 1296)
        self.assertEqual(overall["recorded_attempts"], 6480 + 330 + 240)
        self.assertEqual(overall["historically_nonvalid_now_approved_cells"], 48)
        for key in ("missing_cells", "rejected_cells_excluded", "invalid_cells_excluded",
                    "unrecorded_planned_runs", "planned_valid_runs_still_needed"):
            self.assertEqual(overall[key], 0)
        self.assertTrue(authority["authority"]["full_matrix_complete"])
        self.assertEqual(
            authority["projection_provenance"]["current_p50_audit"][
                "by_timing_scope"],
            [
                {
                    "timing_scope":
                        aggregator.APPROVED_TUPLES_HISTORICAL_TIMING_SCOPE,
                    "approved_rows": 66,
                    "contributing_runs": 330,
                },
                {
                    "timing_scope": aggregator.APPROVED_TUPLES_QUERY_TIMING_SCOPE,
                    "approved_rows": 1296,
                    "contributing_runs": 6480,
                },
            ])
        corrected_scopes = authority["high_priority_families"][
            "corrected_user_intended"]["valid_run_vectors_by_timing_scope"]
        self.assertEqual(corrected_scopes[0]["valid_run_vector"], [10, 0, 5, 0, 40, 40])
        self.assertEqual(corrected_scopes[1]["valid_run_vector"], [40] * 6)

    def test_post_replace_failure_latches_further_appends_in_existing_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            authority_path, authority_raw, _, candidate_path, _ = (
                self._write_synthetic_approved_inputs(directory))
            original_reader = aggregator._read_approved_tuple_file

            def fail_final_read(path, label):
                if label == "final approved tuple authority":
                    raise aggregator.ApprovedTupleIOError("synthetic final read failure")
                return original_reader(path, label)

            aggregator._read_approved_tuple_file = fail_final_read
            try:
                with contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(aggregator.ApprovedTupleIOError):
                        aggregator.append_approved_tuples(
                            authority_path, hashlib.sha256(authority_raw).hexdigest(),
                            candidate_path)
            finally:
                aggregator._read_approved_tuple_file = original_reader
            after = authority_path.read_bytes()
            self.assertNotEqual(after, authority_raw)
            lock_path = Path(directory) / ".ALL_RESULTS_TUPLES.yaml.lock"
            marker = json.loads(lock_path.read_text(encoding="utf-8"))
            self.assertEqual(marker["state"], "fatal_post_replace_validation_failure")
            self.assertEqual(marker["observed_sha256"], hashlib.sha256(after).hexdigest())
            self.assertEqual(marker["intended_after_sha256"], hashlib.sha256(after).hexdigest())
            with self.assertRaisesRegex(aggregator.ApprovedTupleIOError, "requires recovery"):
                aggregator.append_approved_tuples(
                    authority_path, hashlib.sha256(after).hexdigest(), candidate_path)
            self.assertEqual(authority_path.read_bytes(), after)

    def test_post_replace_marker_io_failure_retains_pre_replace_intent(self):
        with tempfile.TemporaryDirectory() as directory:
            authority_path, authority_raw, _, candidate_path, _ = (
                self._write_synthetic_approved_inputs(directory))
            original_reader = aggregator._read_approved_tuple_file
            original_marker_writer = aggregator._write_approved_publication_marker

            def fail_final_read(path, label):
                if label == "final approved tuple authority":
                    raise aggregator.ApprovedTupleIOError("synthetic final read failure")
                return original_reader(path, label)

            def fail_fatal_marker(descriptor, payload):
                if b"fatal_post_replace_validation_failure" in payload:
                    raise aggregator.ApprovedTupleIOError(
                        "synthetic fatal marker persistence failure")
                return original_marker_writer(descriptor, payload)

            aggregator._read_approved_tuple_file = fail_final_read
            aggregator._write_approved_publication_marker = fail_fatal_marker
            try:
                with contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(aggregator.ApprovedTupleIOError):
                        aggregator.append_approved_tuples(
                            authority_path, hashlib.sha256(authority_raw).hexdigest(),
                            candidate_path)
            finally:
                aggregator._read_approved_tuple_file = original_reader
                aggregator._write_approved_publication_marker = original_marker_writer
            after = authority_path.read_bytes()
            self.assertNotEqual(after, authority_raw)
            lock_path = Path(directory) / ".ALL_RESULTS_TUPLES.yaml.lock"
            marker = json.loads(lock_path.read_text(encoding="utf-8"))
            self.assertEqual(marker["state"], "publication_pending_final_validation")
            with self.assertRaisesRegex(aggregator.ApprovedTupleIOError, "requires recovery"):
                aggregator.append_approved_tuples(
                    authority_path, hashlib.sha256(after).hexdigest(), candidate_path)
            self.assertEqual(authority_path.read_bytes(), after)

    def test_pre_replace_failure_clears_intent_only_for_unchanged_authority(self):
        with tempfile.TemporaryDirectory() as directory:
            authority_path, authority_raw, _, candidate_path, _ = (
                self._write_synthetic_approved_inputs(directory))
            original_replace = aggregator.os.replace

            def refuse_replace(source, destination):
                raise OSError("synthetic replace failure")

            aggregator.os.replace = refuse_replace
            try:
                with self.assertRaises(aggregator.ApprovedTupleIOError):
                    aggregator.append_approved_tuples(
                        authority_path, hashlib.sha256(authority_raw).hexdigest(),
                        candidate_path)
            finally:
                aggregator.os.replace = original_replace
            self.assertEqual(authority_path.read_bytes(), authority_raw)
            lock_path = Path(directory) / ".ALL_RESULTS_TUPLES.yaml.lock"
            self.assertEqual(lock_path.read_bytes(), b"")
            aggregator.append_approved_tuples(
                authority_path, hashlib.sha256(authority_raw).hexdigest(), candidate_path)
            self.assertEqual(lock_path.read_bytes(), b"")

    def test_publication_marker_retries_short_writes_and_surfaces_io_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            lock_path = Path(directory) / ".ALL_RESULTS_TUPLES.yaml.lock"
            descriptor = os.open(lock_path, os.O_RDWR | os.O_CREAT, 0o600)
            payload = b'{"state":"publication_pending_final_validation"}\n'
            original_pwrite = aggregator.os.pwrite
            short_write_calls = 0

            def short_pwrite(fd, data, offset):
                nonlocal short_write_calls
                short_write_calls += 1
                return original_pwrite(fd, data[:3], offset)

            try:
                try:
                    aggregator.os.pwrite = short_pwrite
                    aggregator._write_approved_publication_marker(descriptor, payload)
                finally:
                    aggregator.os.pwrite = original_pwrite
                self.assertGreater(short_write_calls, 1)
                self.assertEqual(lock_path.read_bytes(), payload)
                for operation in ("pwrite", "ftruncate", "fsync"):
                    with self.subTest(operation=operation):
                        original = getattr(aggregator.os, operation)

                        def fail_io(*_args, operation=operation):
                            raise OSError(f"synthetic {operation} failure")

                        setattr(aggregator.os, operation, fail_io)
                        try:
                            with self.assertRaises(aggregator.ApprovedTupleIOError):
                                aggregator._write_approved_publication_marker(
                                    descriptor, payload)
                        finally:
                            setattr(aggregator.os, operation, original)
                        self.assertNotEqual(lock_path.read_bytes(), b"")
            finally:
                os.close(descriptor)

    def test_keyboard_interrupt_after_replace_retains_pending_recovery_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            authority_path, authority_raw, _, candidate_path, _ = (
                self._write_synthetic_approved_inputs(directory))
            original_reader = aggregator._read_approved_tuple_file

            def interrupt_final_read(path, label):
                if label == "final approved tuple authority":
                    raise KeyboardInterrupt("synthetic crash edge")
                return original_reader(path, label)

            aggregator._read_approved_tuple_file = interrupt_final_read
            try:
                with self.assertRaises(KeyboardInterrupt):
                    aggregator.append_approved_tuples(
                        authority_path, hashlib.sha256(authority_raw).hexdigest(),
                        candidate_path)
            finally:
                aggregator._read_approved_tuple_file = original_reader
            after = authority_path.read_bytes()
            self.assertNotEqual(after, authority_raw)
            lock_path = Path(directory) / ".ALL_RESULTS_TUPLES.yaml.lock"
            marker = json.loads(lock_path.read_text(encoding="utf-8"))
            self.assertEqual(marker["state"], "publication_pending_final_validation")
            with self.assertRaisesRegex(aggregator.ApprovedTupleIOError, "requires recovery"):
                aggregator.append_approved_tuples(
                    authority_path, hashlib.sha256(after).hexdigest(), candidate_path)
            self.assertEqual(authority_path.read_bytes(), after)

    def test_exact_path_accepts_mapq_annotation_without_claiming_fallback(self):
        authority = self._synthetic_approved_authority()
        row = self._approved_tuple_row()
        row["comments"] = "Numeric MAPQ differs; match counts and positional tolerance passed."
        validated = aggregator._validate_approved_tuple_row(
            row, aggregator.Q1024_CAMPAIGN_ID, authority["matrix_scope"], "synthetic row",
            allow_legacy_preservation=False, require_field_order=True)
        self.assertEqual(validated["comments"], row["comments"])
        self.assertTrue(validated["comparison_eligibility"]["requested_path"])

    def test_dna5_reference_scalar_binding_is_not_mislabeled_historical_simd(self):
        authority = self._synthetic_approved_authority()
        row = self._approved_tuple_row(
            alphabet="dna5", p50_a=110, p50_b=100)
        row["requested_algorithms"]["minimizer"] = "simd_minimap2_reference"
        row["observed_algorithms"]["minimizer"] = "scalar"
        row["effective_path"] = "scalar -> scalar_minimap2 -> ksw2"
        row["execution_conformance"] = "documented_scalar_minimizer_fallback"
        row["comparison_eligibility"] = {
            "requested_path": False, "effective_path": True,
            "scope": "planned_dna5_reference_scalar_only",
        }
        row["comments"] = "DNA5 simd_minimap2_reference binds scalar before execution; not SIMD performance."
        row["stable_key"] = aggregator._stable_key_for_identity(
            aggregator._approved_row_identity(row))
        validated = aggregator._validate_approved_tuple_row(
            row, aggregator.Q1024_CAMPAIGN_ID, authority["matrix_scope"], "synthetic row",
            allow_legacy_preservation=False, require_field_order=True)
        self.assertFalse(validated["comparison_eligibility"]["requested_path"])
        candidate = self._synthetic_candidate(authority, [row])
        candidate_rows, _ = aggregator._validate_approved_append_candidate_document(
            candidate, authority, candidate["authority_before_sha256"])
        self.assertEqual(candidate_rows, [row])

    def test_dna4_reference_scalar_fallback_is_legacy_only_not_appendable(self):
        authority = self._synthetic_approved_authority()
        row = self._approved_tuple_row(
            alphabet="dna4",
            timing_scope=aggregator.APPROVED_TUPLES_HISTORICAL_TIMING_SCOPE)
        row["requested_algorithms"]["minimizer"] = "simd_minimap2_reference"
        row["observed_algorithms"]["minimizer"] = "scalar"
        row["effective_path"] = "scalar -> scalar_minimap2 -> ksw2"
        row["execution_conformance"] = "documented_scalar_minimizer_fallback"
        row["comparison_eligibility"] = {
            "requested_path": False,
            "effective_path": True,
            "scope": "historical_scalar_fallback_only",
        }
        row["comments"] = "Historical simd_minimap2_reference selected scalar."
        row["stable_key"] = aggregator._stable_key_for_identity(
            aggregator._approved_row_identity(row))
        preserved = aggregator._validate_approved_tuple_row(
            row, aggregator.Q1024_CAMPAIGN_ID, authority["matrix_scope"], "legacy row",
            allow_legacy_preservation=True, require_field_order=True)
        self.assertEqual(
            preserved["comparison_eligibility"]["scope"],
            "historical_scalar_fallback_only")
        candidate_row = copy.deepcopy(row)
        candidate_row["timing_scope"] = aggregator.APPROVED_TUPLES_QUERY_TIMING_SCOPE
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_approved_tuple_row(
                candidate_row, aggregator.Q1024_CAMPAIGN_ID,
                authority["matrix_scope"], "query-scope legacy fallback",
                allow_legacy_preservation=True, require_field_order=True)
        candidate = self._synthetic_candidate(authority, [candidate_row])
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_approved_append_candidate_document(
                candidate, authority, candidate["authority_before_sha256"])

        preserved_query = self._approved_tuple_row()
        preserved_query["execution_conformance"] = (
            "exact_requested_path_user_preserved")
        preserved_query["comparison_eligibility"] = {
            "requested_path": True,
            "effective_path": True,
            "scope": "historical_user_preserved_requested_path",
        }
        preserved_query["comments"] = "Synthetic historical preservation marker."
        with self.assertRaises(aggregator.ContractError):
            aggregator._validate_approved_tuple_row(
                preserved_query, aggregator.Q1024_CAMPAIGN_ID,
                authority["matrix_scope"], "query-scope user preservation",
                allow_legacy_preservation=True, require_field_order=True)

    def test_candidate_order_and_authority_derived_errors_use_fixed_exit_codes(self):
        with tempfile.TemporaryDirectory() as directory:
            authority_path, authority_raw, authority, candidate_path, _ = (
                self._write_synthetic_approved_inputs(directory))
            rows = [
                self._approved_tuple_row(
                    target=4, alphabet="dna5", kmer=20, window=15,
                    mode="just_index", p50_a=110, p50_b=100),
                self._approved_tuple_row(
                    target=2, alphabet="dna5", kmer=20, window=15,
                    mode="no_index", p50_a=110, p50_b=100),
            ]
            candidate = self._synthetic_candidate(authority, rows)
            candidate_path.write_text(
                json.dumps(candidate, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                order_code = aggregator.main([
                    "validate-append-candidate",
                    "--candidate", str(candidate_path),
                    "--authority", str(authority_path),
                    "--expected-sha256", hashlib.sha256(authority_raw).hexdigest(),
                ])
            self.assertEqual(order_code, 3)
            self.assertEqual(authority_path.read_bytes(), authority_raw)

            corrupt = copy.deepcopy(authority)
            corrupt["corrected_summaries"]["overall"]["approved_rows"] += 1
            corrupt_raw = aggregator._canonical_approved_tuples_yaml_v3(corrupt)
            authority_path.write_bytes(corrupt_raw)
            with contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                derived_code = aggregator.main([
                    "validate-approved-tuples",
                    "--authority", str(authority_path),
                    "--expected-sha256", hashlib.sha256(corrupt_raw).hexdigest(),
                ])
            self.assertEqual(derived_code, 2)

    @unittest.skipUnless(
        hasattr(os, "symlink") and hasattr(os, "O_NOFOLLOW"),
        "no-follow symlink contract unavailable")
    def test_append_rejects_symlink_lock_as_io_without_touching_authority(self):
        with tempfile.TemporaryDirectory() as directory:
            authority_path, authority_raw, _, candidate_path, _ = (
                self._write_synthetic_approved_inputs(directory))
            target = Path(directory) / "lock-target"
            target.write_text("synthetic\n", encoding="utf-8")
            lock_path = Path(directory) / ".ALL_RESULTS_TUPLES.yaml.lock"
            os.symlink(target, lock_path)
            with contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                exit_code = aggregator.main([
                    "append-approved-tuples",
                    "--candidate", str(candidate_path),
                    "--authority", str(authority_path),
                    "--expected-sha256", hashlib.sha256(authority_raw).hexdigest(),
                ])
            self.assertEqual(exit_code, 5)
            self.assertEqual(authority_path.read_bytes(), authority_raw)


if __name__ == "__main__":
    unittest.main()
