#!/usr/bin/env python3
"""Focused contracts for the sealed deadline-v2 production campaign."""

from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import itertools
import json
from pathlib import Path
import sys
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "build_mcp"))

from bench import generate_e2e_algorithm_matrix_deadline_campaign as generator
import build_worker


class DeadlineCampaignContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.outputs, cls.manifest_raw = generator._expected_outputs()
        cls.manifest = json.loads(cls.manifest_raw)
        cls.schedule = cls.manifest["schedule"]
        cls.parent = json.loads(generator.PARENT_MANIFEST.read_text(encoding="utf-8"))
        cls._current_source_authority_cache = {}

    @classmethod
    def _current_source_authority_fixture(cls, version):
        cached = cls._current_source_authority_cache.get(version)
        if cached is not None:
            return cached

        manifest = copy.deepcopy(cls.parent if version == 1 else cls.manifest)
        source_digests = {}
        for relative in manifest["source_bindings"]:
            source_path = (ROOT / relative).resolve()
            source_digests[source_path] = hashlib.sha256(
                source_path.read_bytes()).hexdigest()
        manifest["source_bindings"] = {
            relative: source_digests[(ROOT / relative).resolve()]
            for relative in manifest["source_bindings"]
        }
        digest_payload = copy.deepcopy(manifest)
        digest_payload["campaign"].pop("campaign_digest", None)
        manifest["campaign"]["campaign_digest"] = (
            build_worker.canonical_json_sha256(digest_payload))
        manifest_raw = generator._render(manifest)
        cached = (manifest, manifest_raw, source_digests)
        cls._current_source_authority_cache[version] = cached
        return cached

    @classmethod
    @contextlib.contextmanager
    def _with_current_source_authority(cls, version=2):
        """Exercise worker guards against one in-memory current-source seal."""
        manifest, manifest_raw, source_digests = (
            cls._current_source_authority_fixture(version))
        if version == 1:
            manifest_relative = build_worker._ALGORITHM_MATRIX_DEADLINE_MANIFEST
            sha_constant = "_ALGORITHM_MATRIX_DEADLINE_MANIFEST_SHA256"
            digest_constant = "_ALGORITHM_MATRIX_DEADLINE_CAMPAIGN_DIGEST"
        else:
            manifest_relative = build_worker._ALGORITHM_MATRIX_DEADLINE_V2_MANIFEST
            sha_constant = "_ALGORITHM_MATRIX_DEADLINE_V2_MANIFEST_SHA256"
            digest_constant = "_ALGORITHM_MATRIX_DEADLINE_V2_CAMPAIGN_DIGEST"
        manifest_path = (ROOT / manifest_relative).resolve()
        original_strict_json = build_worker._strict_json_object
        original_sha256_file = build_worker.sha256_file

        def load(path, label):
            value, raw = original_strict_json(path, label)
            if Path(path).resolve() == manifest_path:
                return copy.deepcopy(manifest), manifest_raw
            return value, raw

        def sha256_file(path):
            if not isinstance(path, int):
                source_digest = source_digests.get(Path(path).resolve())
                if source_digest is not None:
                    return source_digest
            return original_sha256_file(path)

        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(
                build_worker, "_strict_json_object", side_effect=load))
            stack.enter_context(mock.patch.object(
                build_worker, "sha256_file", side_effect=sha256_file))
            stack.enter_context(mock.patch.object(
                build_worker, sha_constant,
                hashlib.sha256(manifest_raw).hexdigest()))
            stack.enter_context(mock.patch.object(
                build_worker, digest_constant,
                manifest["campaign"]["campaign_digest"]))
            yield manifest

    def test_historical_tree_is_explicitly_source_stale(self):
        self.assertEqual(
            generator._check(self.outputs, self.manifest_raw),
            ["stale manifest: "
             "bench/bench_e2e_algorithm_matrix_deadline_v2_manifest.json"],
        )
        authority, error = build_worker._algorithm_matrix_campaign_authority_status(
            self.schedule[0]["path"])
        self.assertIsNone(authority)
        self.assertEqual(error, "deadline v2 source binding is stale")
        expected_shards = (
            len(generator.MINIMIZERS)
            * len(generator.CHAINING)
            * len(generator.BASE_ALIGNERS)
            + 2 * len(generator.base.EXPECTED_DATASETS)
            + 4
        )
        self.assertEqual(expected_shards, 60)
        self.assertEqual(len(self.outputs), expected_shards)
        self.assertEqual(len(self.schedule), expected_shards)
        campaign = self.manifest["campaign"]
        self.assertEqual(campaign["expected_shards"], expected_shards)
        self.assertEqual(campaign["expected_result_cells"], expected_shards)
        self.assertEqual(campaign["expected_raw_records"], expected_shards * 5)
        self.assertEqual(campaign["estimated_wall_seconds"], 586_467)
        self.assertEqual(campaign["deadline_seconds"], 604_800)
        self.assertEqual(campaign["operational_margin_seconds"], 18_333)

    def test_v2_lineage_is_exact_and_write_targets_are_disjoint(self):
        self.assertEqual(
            self.manifest["lineage"], generator._validated_parent_lineage())
        self.assertEqual(
            self.manifest["lineage"]["parent_manifest_sha256"],
            generator.PARENT_MANIFEST_SHA256)
        self.assertEqual(
            self.manifest["lineage"]["parent_campaign_digest"],
            generator.PARENT_CAMPAIGN_DIGEST)
        self.assertNotEqual(generator.MANIFEST_PATH, generator.PARENT_MANIFEST)
        self.assertNotEqual(
            generator.OUTPUT_DIRECTORY, generator.PARENT_OUTPUT_DIRECTORY)
        self.assertNotIn(ROOT / "research_benchmarks/paper/e2e/ALL_RESULTS.yaml", self.outputs)

    def test_q16_cartesian_is_exactly_five_by_four_by_two(self):
        full = [entry for entry in self.schedule
                if entry["identity"]["tier"] == "full_cartesian"]
        actual = {
            (
                entry["identity"]["requested_minimizer_algorithm"],
                entry["identity"]["requested_chaining_algorithm"],
                entry["identity"]["requested_base_alignment_algorithm"],
            ) for entry in full
        }
        expected = set(itertools.product(
            generator.MINIMIZERS, generator.CHAINING, generator.BASE_ALIGNERS))
        self.assertEqual(actual, expected)
        self.assertEqual(len(full), 40)
        self.assertEqual({entry["identity"]["query_count"] for entry in full}, {16})
        self.assertEqual({entry["identity"]["index_mode"] for entry in full},
                         {"no_index"})
        self.assertEqual(
            {entry["identity"]["bioseq_index_name"] for entry in full},
            {"minimizers_per_fragment"},
        )
        self.assertNotIn("ksw2_batched", json.dumps(full, sort_keys=True))

    def test_q2_compact_pairs_match_supported_storage_routes(self):
        outer = [entry for entry in self.schedule
                 if entry["identity"]["tier"] == "compact_outer"]
        bioseq = {entry["identity"]["dataset"]: entry["identity"]
                  for entry in outer if entry["identity"]["engine"] == "bioseq"}
        minimap2 = {entry["identity"]["dataset"]: entry["identity"]
                    for entry in outer
                    if entry["identity"]["engine"] == "minimap2"}
        expected = {
            "M2_D4_K15_W10": ("just_index", "minimizers_per_sequence"),
            "M2_D4_K20_W15": ("no_index", "minimizers_per_sequence"),
            "M2_D5_K15_W10": ("no_index", "minimizers_per_sequence"),
            "M2_D5_K20_W15": ("no_index", "minimizers_per_fragment"),
            "M4_D4_K15_W10": ("no_index", "minimizers_per_fragment"),
            "M4_D4_K20_W15": ("no_index", "minimizers_per_sequence"),
            "M4_D5_K15_W10": ("no_index", "minimizers_per_sequence"),
            "M4_D5_K20_W15": ("no_index", "minimizers_per_fragment"),
        }
        self.assertEqual(set(bioseq), set(expected))
        self.assertEqual(set(minimap2), set(expected))
        for dataset, (mode, index_name) in expected.items():
            with self.subTest(dataset=dataset):
                self.assertEqual(bioseq[dataset]["index_mode"], mode)
                self.assertEqual(bioseq[dataset]["bioseq_index_name"], index_name)
                self.assertEqual(minimap2[dataset]["index_mode"], mode)
        for identity in bioseq.values():
            if "K20" in identity["dataset"]:
                self.assertEqual(identity["index_mode"], "no_index")
                expected_index = (
                    "minimizers_per_sequence"
                    if "_D4_" in identity["dataset"]
                    else "minimizers_per_fragment")
                self.assertEqual(identity["bioseq_index_name"], expected_index)

    def test_q1024_anchors_and_deadline_exclusions_are_exact(self):
        anchors = [entry for entry in self.schedule
                   if entry["identity"]["tier"] == "q1024_anchor"]
        self.assertEqual(len(anchors), 4)
        self.assertEqual(
            [entry["identity"]["engine"] for entry in anchors].count("minimap2"),
            2)
        extended = [entry for entry in anchors
                    if entry["runtime_timeout_seconds"] == 172_800]
        self.assertEqual([entry["ordinal"] for entry in extended], [58, 59])
        self.assertEqual(
            [entry["identity"]["index_mode"] for entry in extended],
            ["just_index", "no_index"],
        )
        for entry in extended:
            identity = entry["identity"]
            self.assertEqual(identity["engine"], "bioseq")
            self.assertEqual(
                identity["bioseq_index_name"], "minimizers_per_fragment")
            self.assertEqual(identity["requested_minimizer_algorithm"], "scalar")
            self.assertEqual(
                identity["requested_chaining_algorithm"], "scalar_minimap2")
            self.assertEqual(identity["requested_base_alignment_algorithm"], "ksw2")
        exclusions = self.manifest["deadline_exclusions"]
        self.assertEqual(len(exclusions), 2)
        self.assertTrue(all(item["classification"]
                            == "deadline_infeasible_not_complete"
                            for item in exclusions))
        self.assertTrue(all("minimizers_per_sequence" in item["identity"]
                            for item in exclusions))
        self.assertFalse(any("minimizers_per_fragment" in item["identity"]
                             for item in exclusions))

    def test_every_config_is_five_repeat_cigar_mapq_prefilter_off(self):
        for entry in self.schedule:
            config = json.loads((ROOT / entry["path"]).read_text(encoding="utf-8"))
            self.assertEqual(list(config["bench_e2e"]["matrix"]["for"].values()),
                             [5])
            data = next(iter(config["test_cases"].values()))["data"]
            self.assertTrue(data["cigar_projection"])
            self.assertTrue(data["mapq_projection"])
            serialized = json.dumps(config, sort_keys=True)
            self.assertNotIn("ksw2_batched", serialized)
            self.assertNotIn('"prefilter_enabled": true', serialized)
            if entry["identity"]["engine"] == "bioseq":
                self.assertTrue(generator.REQUIRED_BIOSEQ_METRICS.issubset(
                    config["bench_e2e"]["matrix"]["metrics"]))

    def test_publication_is_deferred_and_results_preserving(self):
        publication = self.manifest["publication_contract"]
        self.assertEqual(
            publication["merge_into_ALL_RESULTS_yaml"],
            "eligible_after_terminal_per_shard_audit",
        )
        self.assertFalse(publication["automatic_merge_during_run_only"])
        self.assertFalse(publication["merge_into_Q8_RESULTS_yaml"])
        self.assertTrue(publication["existing_ledgers_unchanged_by_generation"])
        self.assertEqual(self.manifest["campaign"]["q1_membership"], 0)

    def test_worker_accepts_v1_and_exact_v2_members(self):
        with self._with_current_source_authority():
            for entry in (self.schedule[0], self.schedule[40], self.schedule[58],
                          self.schedule[59]):
                authority, error = (
                    build_worker._algorithm_matrix_campaign_authority_status(
                        entry["path"]))
                self.assertIsNone(error)
                self.assertEqual(authority["name"], "deadline")
                self.assertEqual(authority["campaign_version"], 2)
                self.assertEqual(
                    authority["execution_role"], "deadline_singleton")
                self.assertEqual(
                    authority["selected_shard"]["ordinal"], entry["ordinal"])
                self.assertTrue(
                    authority["all_results_eligible_after_terminal_audit"])
                self.assertFalse(authority["q8_results_eligible"])

            ordinary = build_worker._run_only_runtime_timeout_seconds(
                "benchmark-e2e", self.schedule[0]["path"], 14_400)
            persisted = build_worker._run_only_runtime_timeout_seconds(
                "benchmark-e2e", self.schedule[58]["path"], 14_400)
            no_index = build_worker._run_only_runtime_timeout_seconds(
                "benchmark-e2e", self.schedule[59]["path"], 14_400)
            self.assertEqual(ordinary, 14_400)
            self.assertEqual(persisted, 43_200)
            self.assertEqual(no_index, 43_200)

        parent_entry = self.parent["schedule"][0]
        with self._with_current_source_authority(1) as current_parent:
            parent_authority, parent_error = (
                build_worker._algorithm_matrix_campaign_authority_status(
                    parent_entry["path"]))
            self.assertIsNone(parent_error)
            self.assertEqual(parent_authority["name"], "deadline")
            self.assertEqual(
                parent_authority["campaign_digest"],
                current_parent["campaign"]["campaign_digest"])

    def _mutated_v2_manifest_error(self, mutate):
        with self._with_current_source_authority():
            strict_json = build_worker._strict_json_object
            v2_path = ROOT / build_worker._ALGORITHM_MATRIX_DEADLINE_V2_MANIFEST

            def load(path, label):
                value, raw = strict_json(path, label)
                if Path(path).resolve() == v2_path.resolve():
                    value = copy.deepcopy(value)
                    mutate(value)
                return value, raw

            with mock.patch.object(
                    build_worker, "_strict_json_object", side_effect=load):
                authority, error = (
                    build_worker._algorithm_matrix_campaign_authority_status(
                        self.schedule[0]["path"]))
        self.assertIsNone(authority)
        self.assertIsNotNone(error)
        return error

    def test_worker_rejects_v2_lineage_hash_and_cardinality_drift(self):
        lineage_error = self._mutated_v2_manifest_error(
            lambda manifest: manifest["lineage"].update(
                {"parent_campaign_digest": "0" * 64}))
        self.assertIn("runtime/publication contract", lineage_error)

        cardinality_error = self._mutated_v2_manifest_error(
            lambda manifest: manifest["campaign"].update(
                {"expected_raw_records": 295}))
        self.assertIn("runtime/publication contract", cardinality_error)

        with (
            self._with_current_source_authority(),
            mock.patch.object(
                build_worker,
                "_ALGORITHM_MATRIX_DEADLINE_V2_MANIFEST_SHA256",
                "0" * 64,
            ),
        ):
            authority, hash_error = (
                build_worker._algorithm_matrix_campaign_authority_status(
                    self.schedule[0]["path"]))
        self.assertIsNone(authority)
        self.assertIn("manifest SHA-256", hash_error)

    def test_worker_rejects_v2_impostor_membership(self):
        with self._with_current_source_authority():
            authority, error = (
                build_worker._algorithm_matrix_campaign_authority_status(
                    generator.OUTPUT_DIRECTORY.relative_to(ROOT).as_posix()
                    + "/impostor.json"))
        self.assertIsNone(authority)
        self.assertIn("membership", error)

    def test_run_only_merge_is_deferred_without_touching_ledgers(self):
        output = io.StringIO()
        with self._with_current_source_authority():
            build_worker._merge_algorithm_matrix_all_results(
                mode="benchmark-e2e",
                report_path=None,
                bench_e2e_config=self.schedule[0]["path"],
                environment={},
                log_handle=output,
                deadline=time.time() + 60.0,
            )
        self.assertIn("ALL_RESULTS.yaml candidate", output.getvalue())
        self.assertIn("automatic run_only merge skipped", output.getvalue())
        self.assertIn("Q8_RESULTS.yaml remains unauthorized", output.getvalue())

    def test_start_build_requires_receipt_pinned_run_only(self):
        with (
            self._with_current_source_authority(),
            mock.patch.object(
                build_worker, "_refuse_root_build_identity", return_value=None,
            ),
            mock.patch.object(
                build_worker, "_ensure_build_worker_timesharing_policy",
            ),
        ):
            result = build_worker.tool_start_build(
                mode="benchmark-e2e",
                bench_filter={"benchmarks": ["bench_e2e"]},
                filter_supplied=True,
                bench_e2e_config=self.schedule[0]["path"],
            )
        self.assertIn("requires run_only=true", result)

    def test_cpp_gate_is_v1_v2_digest_and_membership_closed(self):
        source = (ROOT / "bench/e2e/index.cppm").read_text(encoding="utf-8")
        self.assertIn("canonical_deadline_campaign_e2e_config_identity", source)
        self.assertIn("canonical_deadline_campaign_v2_e2e_config_identity", source)
        self.assertIn(generator.CAMPAIGN_ID, source)
        self.assertIn(self.manifest["campaign"]["campaign_digest"], source)
        self.assertIn(generator.PARENT_CAMPAIGN_ID, source)
        self.assertIn(generator.PARENT_CAMPAIGN_DIGEST, source)
        self.assertIn("expected_shards * repetitions", source)
        self.assertIn("persisted_q1024 && no_index_q1024", source)
        self.assertNotIn("return identity.starts_with(directory)", source)

    def test_cpp_measured_query_connection_failure_gate_is_closed(self):
        source = (ROOT / "bench/e2e/index.cppm").read_text(encoding="utf-8")
        self.assertEqual(
            source.count("record_measured_query_connection_failure("), 4)
        self.assertEqual(
            source.count(
                "reconnect_and_prove_measured_query_connection_failure("), 4)
        self.assertIn(
            "captured.status != captured_query_status::connection_failure", source)
        self.assertIn(
            'captured.result_status == "PGRES_FATAL_ERROR"', source)
        self.assertIn(
            'captured.result_status == "null_result"', source)
        self.assertIn("captured.sqlstate.empty()", source)
        self.assertIn(
            "same postmaster/database and a changed live backend were proven",
            source,
        )
        self.assertIn(
            'out.cell_disposition = "invalid";', source)
        self.assertIn(
            'out.phase = "measured_statement";', source)
        self.assertIn(
            'payload["payload_schema_version"] =\n'
            '        bioseq::bench::framework::e2e_raw_payload_schema_version;',
            source,
        )
        self.assertIn(
            '"measured_query_connection_failure",\n'
            "                out.measured_query_connection_failure_proof",
            source,
        )


if __name__ == "__main__":
    unittest.main()
