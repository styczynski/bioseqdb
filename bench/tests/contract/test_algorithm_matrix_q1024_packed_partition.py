#!/usr/bin/env python3
"""Contract tests for the sealed hybrid packed q1024 authority."""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
BENCH_DIRECTORY = REPOSITORY_ROOT / "bench"
sys.path.insert(0, str(BENCH_DIRECTORY))

import generate_e2e_algorithm_matrix_q1024_packed_partition as generator


def _load_aggregator():
    path = BENCH_DIRECTORY / "aggregate_e2e_algorithm_matrix.py"
    spec = importlib.util.spec_from_file_location(
        "q1024_packed_contract_aggregator", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load algorithm-matrix aggregator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _contract_modules():
    return (_load_aggregator(),)


PREVIOUS_METADATA_MANIFEST_SHA256 = (
    "65d31b8b3aa49400598d80d50641b2a9a4b1f78c08c85c8a33afbef761fd891b")
UNCHANGED_OWNER_IDENTITIES = {
    (
        "bench/e2e_algorithm_matrix_deadline_v2_shards/"
        "bench_e2e_algorithm_matrix_deadline_v2_s056_q1024_q1024_anchor_"
        "m2_d4_k15_w10_no_index_minimap2.json"
    ): {
        "config_sha256":
            "4e1d86c1aba8bbafb8c8a5d203cb7b19f6a402fc9d732c990d50d298ca4e5434",
        "config_role": "shard",
        "expected_stable_keys_sha256":
            "423ec685980909f514b861a6440901f927a978430cbf4458fef3d227f727ae5d",
        "execution_authority": "q1024_deadline_v2_adoption",
        "canonical_config": (
            "bench/e2e_algorithm_matrix_shards/"
            "bench_e2e_algorithm_matrix_campaign_shard_"
            "m2_d4_k15_w10_no_index_baselines.json"),
        "canonical_config_sha256":
            "74e066c4d66c2abdf5b6bb23213d6c96117c3f3f07dd0887712d0a50a5bbb90f",
        "canonical_stable_keys_sha256":
            "423ec685980909f514b861a6440901f927a978430cbf4458fef3d227f727ae5d",
    },
    (
        "bench/e2e_algorithm_matrix_deadline_v2_shards/"
        "bench_e2e_algorithm_matrix_deadline_v2_s057_q1024_q1024_anchor_"
        "m2_d4_k15_w10_just_index_minimap2.json"
    ): {
        "config_sha256":
            "038cf71ab49ddc7f52ad9594009e77aed8171e6fab3567838eaf362e5d8bfa78",
        "config_role": "shard",
        "expected_stable_keys_sha256":
            "e2662dcb9020741ce1ddef01f8142744cb8bfb109737e1437609552efbf635ba",
        "execution_authority": "q1024_deadline_v2_adoption",
        "canonical_config": (
            "bench/e2e_algorithm_matrix_shards/"
            "bench_e2e_algorithm_matrix_campaign_shard_"
            "m2_d4_k15_w10_no_index_baselines.json"),
        "canonical_config_sha256":
            "74e066c4d66c2abdf5b6bb23213d6c96117c3f3f07dd0887712d0a50a5bbb90f",
        "canonical_stable_keys_sha256":
            "e2662dcb9020741ce1ddef01f8142744cb8bfb109737e1437609552efbf635ba",
    },
}
MEMBER23_PATH = (
    "bench/e2e_algorithm_matrix_q1024_packed_shards/"
    "bench_e2e_algorithm_matrix_q1024_packed_m2_d4_k20_w15_"
    "just_index_scalar_mpfragment_block_aligner_residual_after_0739.json")
MEMBER23_PREVIOUS_CONFIG_SHA256 = (
    "72be93f76248c5a7e8160a9540ce1bebe087d2c7725bcc6e79ec96a1e71bb741")


def _synthetic_processed_report(
        authority: dict, config_path: str, report_ordinal: int) -> dict:
    """Build one minimal legacy binding from an unchanged canonical owner."""
    owner = authority["configs_by_path"][REPOSITORY_ROOT / config_path]
    parent = owner["canonical_parent"]
    processed_authority = {
        "q1024_singleton": "q1024_singleton_fallback",
        "q1024_packed": "q1024_packed_partition",
        "q1024_deadline_v2_adoption": "q1024_deadline_v2_adoption",
    }[owner["execution_authority"]]
    identity = {
        "config_sha256": owner["config_sha256"],
        "config_role": owner["role"],
        "expected_stable_keys_sha256": owner["expected_stable_keys_sha256"],
        "execution_authority": processed_authority,
        "canonical_config": parent["path"],
        "canonical_config_sha256": parent["config_sha256"],
        "canonical_stable_keys_sha256": owner["expected_stable_keys_sha256"],
    }
    expected_identity = UNCHANGED_OWNER_IDENTITIES.get(config_path)
    if expected_identity is not None and identity != expected_identity:
        raise AssertionError("fixed synthetic owner identity changed")
    return {
        "report": f"synthetic/report_{report_ordinal}.json",
        "report_sha256": f"{report_ordinal:064x}",
        "config": config_path,
        **identity,
        "partition_manifest": authority["execution_authority"],
        "partition_manifest_sha256": PREVIOUS_METADATA_MANIFEST_SHA256,
        "synthetic_entry_sentinel": f"entry-{report_ordinal}",
    }


def _synthetic_legacy_document(module, authority: dict) -> dict:
    """Return a small immutable-data fixture for the previous metadata seal."""
    probe: dict = {}
    previous_contract = copy.deepcopy(
        module._current_all_results_contract_for_document(authority, probe))
    previous_contract["execution_partition"]["manifest_sha256"] = (
        PREVIOUS_METADATA_MANIFEST_SHA256)
    return {
        "contract": previous_contract,
        "wiring": module._expected_wiring(),
        "aggregation_state": {
            "processed_reports": [
                _synthetic_processed_report(authority, config_path, ordinal)
                for ordinal, config_path in enumerate(
                    UNCHANGED_OWNER_IDENTITIES, start=1)
            ],
            "synthetic_state_sentinel": {"retained": True},
        },
        "results": [
            {"stable_key": "synthetic-result-a", "sentinel": [1, 2, 3]},
            {"stable_key": "synthetic-result-b", "sentinel": {"kept": True}},
        ],
        "synthetic_top_level_sentinel": "unchanged",
    }


class Q1024PackedPartitionContractTests(unittest.TestCase):
    def test_generator_reproduces_every_output_byte_for_byte(self):
        outputs, manifest_raw = generator._generate()
        expected = {
            **outputs,
            generator.OUTPUT_MANIFEST_PATH: manifest_raw,
        }
        self.assertEqual(len(outputs), 10)
        for path, raw in expected.items():
            with self.subTest(path=path):
                self.assertEqual(path.read_bytes(), raw)
        self.assertEqual(
            hashlib.sha256(manifest_raw).hexdigest(),
            "5351e563a269c013a4a6674c6bea8bce0ef24016d1c4965582d9df670c7252ae",
        )

    def test_manifest_coverage_and_adoption_policy_are_exact(self):
        manifest = json.loads(generator.OUTPUT_MANIFEST_PATH.read_text(
            encoding="utf-8"))
        contract = manifest["execution_contract"]
        self.assertEqual(len(manifest["packed_members"]), 25)
        self.assertEqual(contract["packed_result_cells"], 150)
        self.assertEqual(contract["packed_raw_records"], 750)
        self.assertEqual(contract["adopted_result_cells"], 4)
        self.assertEqual(contract["superseded_singleton_cells"], 154)
        self.assertEqual(contract["singleton_fallback_cells"], 1142)
        self.assertEqual(contract["scheduled_result_cells"], 157)
        self.assertEqual(contract["scheduled_supported_cells"], 122)
        self.assertEqual(contract["scheduled_selector_rejection_cells"], 35)
        self.assertEqual(contract["repeat_indices"], list(range(5)))
        self.assertFalse(contract["prefilter_enabled"])
        self.assertTrue(contract["cigar_projection"])
        self.assertTrue(contract["mapq_projection"])
        self.assertEqual(
            contract["excluded_base_alignment_algorithms"],
            ["ksw2_batched"],
        )
        self.assertEqual(
            [entry["source_ordinal"] for entry in manifest["adoptions"]],
            [56, 57, 58, 59],
        )
        self.assertTrue(all(
            entry["sealed_artifacts"] is not None
            for entry in manifest["adoptions"][:3]))
        self.assertIsNone(manifest["adoptions"][3]["sealed_artifacts"])
        receipt_paths = [
            entry["run_receipt_config"]["path"]
            for entry in manifest["adoptions"]]
        self.assertIn("e2e_algorithm_matrix_deadline_shards", receipt_paths[0])
        self.assertIn("e2e_algorithm_matrix_deadline_shards", receipt_paths[1])
        self.assertEqual(
            receipt_paths[2:],
            [entry["source_path"] for entry in manifest["adoptions"][2:]],
        )
        self.assertTrue(all(
            entry["run_receipt_config"]["sha256"]
                == entry["source_config_sha256"]
            for entry in manifest["adoptions"]))
        self.assertEqual(
            [entry["run_receipt_id"] for entry in manifest["adoptions"]],
            [
                "rr_f6564879423bfcd46562dbb1920d6111",
                "rr_aa3f5446efbfe0d2de6fdae382532ca9",
                "rr_1e43d9e01a8a6eee985b4240ffe98d64",
                None,
            ],
        )
        self.assertEqual(
            {entry["execution_cell"]["test_case"]
             for entry in manifest["adoptions"]},
            {"M2_D4_K15_W10_Q1024_DEADLINE"},
        )
        self.assertEqual(
            manifest["adoptions"][3]["source_config_sha256"],
            "59d2204ce94d06590eda2ae351dbbb8133695d731d4d17dbd858eb1ab44b0f35",
        )

    def test_four_m4_mps_parents_are_exact_disk_gated_reusable_packs(self):
        manifest = json.loads(generator.OUTPUT_MANIFEST_PATH.read_text(
            encoding="utf-8"))
        expected = {
            "m4_d4_k15_w10_scalar_mpsequence": (
                "bench/e2e_algorithm_matrix_shards/"
                "bench_e2e_algorithm_matrix_campaign_shard_"
                "m4_d4_k15_w10_just_index_scalar_mpsequence.json",
                "cb5814ff0a012b1cc3de81c473c118a9f71af71a7ba527f8667b3448dc6dac2e",
            ),
            "m4_d4_k20_w15_scalar_mpsequence": (
                "bench/e2e_algorithm_matrix_shards/"
                "bench_e2e_algorithm_matrix_campaign_shard_"
                "m4_d4_k20_w15_just_index_scalar_mpsequence.json",
                "f39d1446bd2b4d5c2e56b1b800b2fba64f8c5ba18cc8d1bbf9de3c9752fcc502",
            ),
            "m4_d5_k15_w10_scalar_mpsequence": (
                "bench/e2e_algorithm_matrix_shards/"
                "bench_e2e_algorithm_matrix_campaign_shard_"
                "m4_d5_k15_w10_just_index_scalar_mpsequence.json",
                "0aac963a8a7602f82b19741539f8bfb4c2218da4c84b2ada6d2222d23d82da54",
            ),
            "m4_d5_k20_w15_scalar_mpsequence": (
                "bench/e2e_algorithm_matrix_shards/"
                "bench_e2e_algorithm_matrix_campaign_shard_"
                "m4_d5_k20_w15_just_index_scalar_mpsequence.json",
                "543f6bdd5e6872cf3df31db1798aea85ea0f7920bc89fd8da21bb0b21fb02f98",
            ),
        }
        members = {entry["id"]: entry for entry in manifest["packed_members"]}
        gated_paths = []
        for member_id, (path, sha256) in expected.items():
            member = members[member_id]
            self.assertEqual(member["kind"], "canonical_parent")
            self.assertEqual(member["path"], path)
            self.assertEqual(member["source_parent_path"], path)
            self.assertEqual(member["config_sha256"], sha256)
            self.assertEqual(member["expected_result_cells"], 8)
            self.assertEqual(member["expected_raw_records"], 40)
            self.assertEqual(member["repetitions_per_cell"], 5)
            self.assertEqual(member["planned_supported_cells"], 6)
            self.assertEqual(member["planned_selector_rejection_cells"], 2)
            self.assertEqual(
                [cell["tuple_index"] for cell in member["execution_cells"]],
                list(range(8)),
            )
            self.assertEqual(
                {cell["test_case_index"] for cell in member["execution_cells"]},
                {0},
            )
            identities = [cell["identity"] for cell in member["execution_cells"]]
            self.assertEqual(
                {identity["bioseq_index_name"] for identity in identities},
                {"minimizers_per_sequence"},
            )
            self.assertEqual(
                {identity["requested_minimizer_algorithm"]
                 for identity in identities},
                {"scalar"},
            )
            self.assertEqual(
                {identity["requested_base_alignment_algorithm"]
                 for identity in identities},
                {"ksw2", "block_aligner"},
            )
            gated_paths.append(path)

        plan = manifest["execution_plan"]
        self.assertEqual(
            plan["disk_preflight"]["gated_packed_configs"], gated_paths)
        steps_by_path = {
            entry["path"]: entry for entry in plan["run_order"]
            if "path" in entry
        }
        self.assertTrue(all(
            steps_by_path[path].get("gate") == "disk_preflight"
            for path in gated_paths))
        self.assertEqual(
            plan["disk_preflight"]
                ["m4_just_index_mps_supported_cells_blocked"],
            24,
        )

    def test_aggregator_exposes_one_owner_per_canonical_stable_key(self):
        aggregator = _load_aggregator()
        aggregator._activate_campaign_profile("ALL_RESULTS.yaml")
        authority = aggregator._load_campaign_contract()
        owners = [
            entry for entry in authority["configs_by_path"].values()
            if entry.get("execution_authority") in {
                "q1024_singleton", "q1024_packed",
                "q1024_deadline_v2_adoption"}
        ]
        keys = [
            stable_key for entry in owners
            for stable_key in entry["descriptors_by_key"]]
        self.assertEqual(len(keys), 1296)
        self.assertEqual(len(set(keys)), 1296)
        self.assertEqual(
            authority["deterministic_first_ingestion"],
            manifest_first_ingestion(),
        )

    def test_previous_metadata_migration_changes_only_manifest_bindings(self):
        for module in _contract_modules():
            with self.subTest(module=module.__name__):
                module._activate_campaign_profile("ALL_RESULTS.yaml")
                authority = module._load_campaign_contract()
                document = _synthetic_legacy_document(module, authority)
                expected = copy.deepcopy(document)
                expected["contract"] = (
                    module._current_all_results_contract_for_document(
                        authority, expected))
                for entry in expected["aggregation_state"]["processed_reports"]:
                    self.assertEqual(
                        entry["partition_manifest_sha256"],
                        PREVIOUS_METADATA_MANIFEST_SHA256)
                    entry["partition_manifest_sha256"] = authority[
                        "execution_authority_sha256"]
                module._refresh_q1024_partition_binding(document, authority)
                self.assertEqual(document, expected)

    def test_previous_metadata_migration_rejects_wrong_config_atomically(self):
        for module in _contract_modules():
            with self.subTest(module=module.__name__):
                module._activate_campaign_profile("ALL_RESULTS.yaml")
                authority = module._load_campaign_contract()
                document = _synthetic_legacy_document(module, authority)
                document["aggregation_state"]["processed_reports"][-1][
                    "config_sha256"] = "0" * 64
                original = copy.deepcopy(document)
                with self.assertRaisesRegex(
                        module.ContractError,
                        "previous metadata report configuration identity changed"):
                    module._refresh_q1024_partition_binding(document, authority)
                self.assertEqual(document, original)

    def test_previous_metadata_migration_rejects_changed_member23_atomically(self):
        for module in _contract_modules():
            with self.subTest(module=module.__name__):
                module._activate_campaign_profile("ALL_RESULTS.yaml")
                authority = module._load_campaign_contract()
                document = _synthetic_legacy_document(module, authority)
                entry = _synthetic_processed_report(
                    authority, MEMBER23_PATH, report_ordinal=23)
                entry["config_sha256"] = MEMBER23_PREVIOUS_CONFIG_SHA256
                document["aggregation_state"]["processed_reports"][-1] = entry
                original = copy.deepcopy(document)
                with self.assertRaisesRegex(
                        module.ContractError,
                        "previous metadata report configuration identity changed"):
                    module._refresh_q1024_partition_binding(document, authority)
                self.assertEqual(document, original)


def manifest_first_ingestion() -> str:
    manifest = json.loads(generator.OUTPUT_MANIFEST_PATH.read_text(
        encoding="utf-8"))
    return manifest["execution_contract"]["deterministic_first_ingestion"]


if __name__ == "__main__":
    unittest.main()
