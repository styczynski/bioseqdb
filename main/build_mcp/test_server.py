"""Focused unit tests for the safe local bench-e2e config override."""

from pathlib import Path
import hashlib
import json
import socket
import tempfile
import time
import unittest
from unittest import mock

import server


class BenchE2eConfigOverrideTests(unittest.TestCase):
    def test_accepts_workspace_relative_json_and_returns_canonical_path(self):
        resolved, error = server._validate_bench_e2e_config_override(
            "bench-e2e", "main/bench/bench_e2e_configuration.json")

        self.assertIsNone(error)
        self.assertEqual(
            resolved,
            str((server.WORKSPACE_ROOT
                 / "main/bench/bench_e2e_configuration.json").resolve()),
        )

    def test_rejects_override_for_every_other_mode(self):
        resolved, error = server._validate_bench_e2e_config_override(
            "bench", "main/bench/bench_e2e_configuration.json")

        self.assertIsNone(resolved)
        self.assertIn("only for mode='bench-e2e'", error)

    def test_rejects_existing_json_outside_workspace(self):
        outside_json = Path("/tmp/bioseq-build-e2e-outside.json")
        outside_json.write_text("{}", encoding="utf-8")
        self.addCleanup(outside_json.unlink, missing_ok=True)

        resolved, error = server._validate_bench_e2e_config_override(
            "bench-e2e", str(outside_json))

        self.assertIsNone(resolved)
        self.assertIn("inside the workspace", error)

    def test_rejects_missing_and_non_json_paths(self):
        missing, missing_error = server._validate_bench_e2e_config_override(
            "bench-e2e", "main/bench/does-not-exist.json")
        non_json, non_json_error = server._validate_bench_e2e_config_override(
            "bench-e2e", "main/build_mcp/server.py")

        self.assertIsNone(missing)
        self.assertIn("existing file", missing_error)
        self.assertIsNone(non_json)
        self.assertIn("must name a .json file", non_json_error)


class SqlGuardrailWorkflowRequestTests(unittest.TestCase):
    def setUp(self):
        build_root = server.PROJECT_ROOT / "build"
        build_root.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(
            prefix="sql-guardrail-request-test-",
            dir=build_root,
        )
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        original_ledger = server.SQL_GUARDRAIL_PUBLICATION_LEDGER
        server.SQL_GUARDRAIL_PUBLICATION_LEDGER = (
            self.directory / "publication-ledger.json"
        )
        self.addCleanup(
            setattr,
            server,
            "SQL_GUARDRAIL_PUBLICATION_LEDGER",
            original_ledger,
        )

    @staticmethod
    def _seal(value):
        result = dict(value)
        result["content_sha256"] = hashlib.sha256(json.dumps(
            result, sort_keys=True, separators=(",", ":")
        ).encode("utf-8")).hexdigest()
        return result

    def _write_request(self, value, name="request.json"):
        path = self.directory / name
        path.write_text(
            json.dumps(self._seal(value), sort_keys=True),
            encoding="utf-8",
        )
        return path

    def _write_input(self, name, value):
        path = self.directory / name
        path.write_text(json.dumps(value, sort_keys=True), encoding="utf-8")
        return path, hashlib.sha256(path.read_bytes()).hexdigest()

    def _write_baseline_publication(
            self, baseline, baseline_value, environment_key):
        publication_id = "1" * 64
        promotion_request = {"phase": "baseline_promotion"}
        promotion_request_sha256 = server._canonical_json_sha256(
            promotion_request)
        receipt_path = server._sql_guardrail_baseline_receipt_path(
            baseline)
        receipt = self._seal({
            "schema_version": 1,
            "artifact_kind":
                "doc0388_sql_guardrail_baseline_publication",
            "publication_id": publication_id,
            "baseline_path": str(baseline.resolve()),
            "baseline_file_sha256":
                hashlib.sha256(baseline.read_bytes()).hexdigest(),
            "baseline_content_sha256":
                baseline_value["content_sha256"],
            "capture_bundle_sha256": "2" * 64,
            "canonical_environment_key": environment_key,
            "promotion_request_file_sha256": "3" * 64,
            "promotion_request_content_sha256": "4" * 64,
            "promotion_request_snapshot_sha256":
                promotion_request_sha256,
            "promotion_request": promotion_request,
            "worker_pid": 123,
            "worker_session_id": 123,
            "worker_started_at": 1.0,
            "published_monotonic_ns": 1,
        })
        receipt_path.write_text(
            json.dumps(receipt, sort_keys=True), encoding="utf-8")
        ledger = self._seal({
            "schema_version": 1,
            "artifact_kind":
                "doc0388_sql_guardrail_publication_ledger",
            "publications": {
                publication_id: {
                    "receipt_path": str(receipt_path.resolve()),
                    "receipt_file_sha256": hashlib.sha256(
                        receipt_path.read_bytes()).hexdigest(),
                    "baseline_path": str(baseline.resolve()),
                    "baseline_file_sha256": hashlib.sha256(
                        baseline.read_bytes()).hexdigest(),
                    "canonical_environment_key": environment_key,
                    "promotion_request_snapshot_sha256":
                        promotion_request_sha256,
                },
            },
        })
        server.SQL_GUARDRAIL_PUBLICATION_LEDGER.write_text(
            json.dumps(ledger, sort_keys=True), encoding="utf-8")
        return receipt_path

    def test_calibration_capture_has_no_predeclared_review(self):
        request = self._write_request({
            "schema_version": 1,
            "artifact_kind": server._SQL_GUARDRAIL_WORKFLOW_KIND,
            "phase": "calibration_capture",
            "capture_bundle_output": str(
                self.directory / "capture-bundle.json"),
        })

        parsed = server._sql_guardrail_workflow_request(str(request))

        self.assertEqual(parsed["phase"], "calibration_capture")
        self.assertNotIn("reviewer", parsed)
        self.assertNotIn("limits", parsed)
        self.assertEqual(
            parsed["request_file_sha256"],
            hashlib.sha256(request.read_bytes()).hexdigest(),
        )

    def test_stage0_and_combined_capture_review_are_rejected(self):
        stage0 = self._write_request({
            "schema_version": 1,
            "artifact_kind": server._SQL_GUARDRAIL_WORKFLOW_KIND,
            "phase": "stage0_capture",
            "capture_bundle_output": str(
                self.directory / "stage0.json"),
        }, "stage0.json")
        combined = self._write_request({
            "schema_version": 1,
            "artifact_kind": server._SQL_GUARDRAIL_WORKFLOW_KIND,
            "phase": "calibration_capture",
            "capture_bundle_output": str(
                self.directory / "combined-output.json"),
            "reviewer": "premature",
        }, "combined.json")

        with self.assertRaisesRegex(ValueError, "phase is invalid"):
            server._sql_guardrail_workflow_request(str(stage0))
        with self.assertRaisesRegex(ValueError, "fields must be exactly"):
            server._sql_guardrail_workflow_request(str(combined))

    def test_promotion_binds_bundle_and_limits_hashes(self):
        bundle, bundle_sha = self._write_input(
            "capture.json", {"capture": "measured"})
        limits, limits_sha = self._write_input(
            "limits.json", {"limits": "reviewed after capture"})
        request = self._write_request({
            "schema_version": 1,
            "artifact_kind": server._SQL_GUARDRAIL_WORKFLOW_KIND,
            "phase": "baseline_promotion",
            "capture_bundle": str(bundle),
            "capture_bundle_sha256": bundle_sha,
            "limits": str(limits),
            "limits_sha256": limits_sha,
            "baseline_output": str(self.directory / "baseline.json"),
            "baseline_kind": "recovered_post_cutover",
            "reviewer": "independent reviewer",
            "evidence_reference": "capture review record",
            "spec_deviation": "Stage-0 numeric evidence is absent",
            "approval_reference": "approved deviation",
        })

        parsed = server._sql_guardrail_workflow_request(str(request))
        self.assertEqual(parsed["capture_bundle_sha256"], bundle_sha)
        self.assertEqual(parsed["limits_sha256"], limits_sha)
        self.assertEqual(
            parsed["baseline_kind"], "recovered_post_cutover")

        limits.write_text('{"mutated":true}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "does not match"):
            server._sql_guardrail_workflow_request(str(request))

    def test_promotion_cannot_claim_stage0(self):
        bundle, bundle_sha = self._write_input("capture.json", {})
        limits, limits_sha = self._write_input("limits.json", {})
        request = self._write_request({
            "schema_version": 1,
            "artifact_kind": server._SQL_GUARDRAIL_WORKFLOW_KIND,
            "phase": "baseline_promotion",
            "capture_bundle": str(bundle),
            "capture_bundle_sha256": bundle_sha,
            "limits": str(limits),
            "limits_sha256": limits_sha,
            "baseline_output": str(self.directory / "baseline.json"),
            "baseline_kind": "stage0",
            "reviewer": "reviewer",
            "evidence_reference": "evidence",
            "spec_deviation": "missing",
            "approval_reference": "approval",
        })

        with self.assertRaisesRegex(
                ValueError, "only recovered_post_cutover"):
            server._sql_guardrail_workflow_request(str(request))

    def test_candidate_binds_baseline_environment_and_unique_output(self):
        environment_key = "a" * 64
        baseline_value = self._seal({
            "canonical_environment_key": environment_key,
        })
        baseline, baseline_sha = self._write_input(
            "baseline.json", baseline_value)
        receipt = self._write_baseline_publication(
            baseline, baseline_value, environment_key)
        request = self._write_request({
            "schema_version": 1,
            "artifact_kind": server._SQL_GUARDRAIL_WORKFLOW_KIND,
            "phase": "candidate_qualification",
            "baseline": str(baseline),
            "baseline_sha256": baseline_sha,
            "canonical_environment_key": environment_key,
            "qualification_output": str(
                self.directory / "qualification.json"),
        })

        parsed = server._sql_guardrail_workflow_request(str(request))

        self.assertEqual(parsed["baseline_sha256"], baseline_sha)
        self.assertEqual(
            parsed["canonical_environment_key"], environment_key)
        self.assertEqual(
            parsed["baseline_publication_receipt"], str(receipt))

    def test_direct_cli_baseline_without_publication_is_rejected(self):
        environment_key = "a" * 64
        baseline_value = self._seal({
            "canonical_environment_key": environment_key,
        })
        baseline, baseline_sha = self._write_input(
            "direct-cli-baseline.json", baseline_value)
        request = self._write_request({
            "schema_version": 1,
            "artifact_kind": server._SQL_GUARDRAIL_WORKFLOW_KIND,
            "phase": "candidate_qualification",
            "baseline": str(baseline),
            "baseline_sha256": baseline_sha,
            "canonical_environment_key": environment_key,
            "qualification_output": str(
                self.directory / "direct-cli-qualification.json"),
        }, "direct-cli-request.json")

        with self.assertRaisesRegex(
                ValueError, "no valid MCP publication receipt"):
            server._sql_guardrail_workflow_request(str(request))

    def test_live_authority_callback_rejects_nonce_and_replay(self):
        socket_path = self.directory / "authority.sock"
        broker = server._SqlGuardrailLiveAuthorityBroker(
            path=socket_path,
            run_nonce=123,
            authority_file_sha256="a" * 64,
            request_snapshot_sha256="b" * 64,
        )
        broker.start()

        def claim(nonce):
            client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            client.settimeout(2.0)
            try:
                client.connect(str(socket_path))
                client.sendall((json.dumps({
                    "schema_version": 1,
                    "artifact_kind":
                        "doc0388_sql_guardrail_authority_claim",
                    "run_nonce": nonce,
                    "authority_file_sha256": "a" * 64,
                    "request_snapshot_sha256": "b" * 64,
                }) + "\n").encode("utf-8"))
                return json.loads(
                    client.makefile("r", encoding="utf-8").readline())
            finally:
                client.close()

        self.assertFalse(claim(124)["accepted"])
        self.assertFalse(broker.consumed)
        self.assertTrue(claim(123)["accepted"])
        self.assertTrue(broker.consumed)
        broker.stop()
        self.assertFalse(socket_path.exists())
        replay = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            with self.assertRaises(OSError):
                replay.connect(str(socket_path))
        finally:
            replay.close()

    def test_request_content_seal_is_mandatory(self):
        request = self._write_request({
            "schema_version": 1,
            "artifact_kind": server._SQL_GUARDRAIL_WORKFLOW_KIND,
            "phase": "calibration_capture",
            "capture_bundle_output": str(
                self.directory / "capture-bundle.json"),
        })
        value = json.loads(request.read_text(encoding="utf-8"))
        value["capture_bundle_output"] = str(
            self.directory / "tampered.json")
        request.write_text(json.dumps(value), encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "seal does not match"):
            server._sql_guardrail_workflow_request(str(request))

    def test_capture_bundle_validates_exact_hash_and_nonce_joins(self):
        def write(name, value):
            path = self.directory / name
            if isinstance(value, bytes):
                path.write_bytes(value)
            else:
                path.write_text(
                    json.dumps(value, sort_keys=True), encoding="utf-8")
            return path

        def record(path):
            body = path.read_bytes()
            return {
                "path": str(path.resolve().relative_to(
                    server.WORKSPACE_ROOT.resolve())),
                "bytes": len(body),
                "sha256": hashlib.sha256(body).hexdigest(),
            }

        environment = {
            "sha256": "a" * 64,
            "logical_cpu_count": 8,
        }
        source = {
            "tracked_diff_base64": "",
            "tracked_diff_sha256": hashlib.sha256(b"").hexdigest(),
            "untracked_files": [],
            "inventory_entries": [],
        }
        tool = write("sql_codegen_guardrail.py", b"tool")
        benchmark_configuration = write(
            "bench_sql_codegen_guardrail_configuration.json",
            b"qualification config",
        )
        calibration_configuration = write(
            "bench_sql_codegen_guardrail_calibration_configuration.json",
            b"calibration config",
        )
        generator_output_inventory = write(
            "generator-outputs.txt",
            b"/synthetic/build/final.stamp\n",
        )
        source_input_paths = {
            "tool": "main/bench/sql_codegen_guardrail.py",
            "benchmark_configuration":
                "main/bench/"
                "bench_sql_codegen_guardrail_configuration.json",
            "calibration_configuration":
                "main/bench/"
                "bench_sql_codegen_guardrail_calibration_configuration.json",
        }
        source_files = {
            "tool": tool,
            "benchmark_configuration": benchmark_configuration,
            "calibration_configuration": calibration_configuration,
        }
        for field, original_path in source_input_paths.items():
            source["inventory_entries"].append({
                "path": original_path,
                "bytes": record(source_files[field])["bytes"],
                "sha256": record(source_files[field])["sha256"],
            })
        benchmark = write("benchmark", b"benchmark")
        comparator = write("comparator.dylib", b"comparator")
        generator = write("generator", b"generator")
        production = write("production.dylib", b"production")
        generated_records = []
        measurement_entries = {"sql": [], "json": [], "cpp": []}
        for group in ("sql", "json", "cpp"):
            generated = write(f"generated.{group}", group.encode())
            generated_record = record(generated)
            original = f"/synthetic/original.{group}"
            generated_records.append({
                **generated_record,
                "group": group,
                "original_path": original,
            })
            measurement_entries[group].append({
                "path": original,
                "bytes": generated_record["bytes"],
                "sha256": generated_record["sha256"],
            })
        measurement_value = {
            "role": "candidate",
            "content_sha256": "d" * 64,
            "source": source,
            "environment": environment,
            "runtime_artifacts": {
                "generator_executable": {
                    "path": "/synthetic/generator",
                    "bytes": record(generator)["bytes"],
                    "sha256": record(generator)["sha256"],
                },
                "production_dso": {
                    "path": "/synthetic/build/production.dylib",
                    "bytes": record(production)["bytes"],
                    "sha256": record(production)["sha256"],
                },
                "installed_production_dso": {
                    "path": "/synthetic/install/production.dylib",
                    "bytes": record(production)["bytes"],
                    "sha256": record(production)["sha256"],
                },
                "benchmark_executable": {
                    "path": "/synthetic/benchmark",
                    "bytes": record(benchmark)["bytes"],
                    "sha256": record(benchmark)["sha256"],
                },
                "comparator_dso": {
                    "path": "/synthetic/comparator.dylib",
                    "bytes": record(comparator)["bytes"],
                    "sha256": record(comparator)["sha256"],
                },
            },
            "artifacts": {
                group: {"entries": entries}
                for group, entries in measurement_entries.items()
            },
            "measurement_context": {
                "generator_boundary": {
                    "output_inventory_file_sha256":
                        record(generator_output_inventory)["sha256"],
                },
            },
        }
        measurement = write("measurement.json", measurement_value)
        measurement_record = record(measurement)
        reports = []
        for index in range(1, 4):
            report = write(
                f"report-{index}.json", {"run_nonce": index})
            host_states = {}
            for boundary, capture_ordinal in (
                ("before", index * 2 - 1),
                ("after", index * 2),
            ):
                snapshot = {
                    "captured_monotonic_ns": capture_ordinal,
                    "load_1": 1.0,
                    "load_5": 1.0,
                    "load_15": 1.0,
                    "logical_cpu_count": 8,
                    "normalized_load_1": 0.125,
                    "available_memory_bytes": 8 * 1024**3,
                    "cpu_governor_states": ["synthetic:performance"],
                }
                host_state = write(
                    f"report-{index}-{boundary}-host-state.json",
                    self._seal({
                        "schema_version": 1,
                        "artifact_kind": "runtime_host_state",
                        "phase": boundary,
                        "run_nonce": index,
                        "measurement_sha256":
                            measurement_record["sha256"],
                        "measurement_content_sha256":
                            measurement_value["content_sha256"],
                        "source": source,
                        "environment": environment,
                        "snapshot": snapshot,
                    }),
                )
                host_states[boundary] = {
                    "record": record(host_state),
                    "document": json.loads(
                        host_state.read_text(encoding="utf-8")),
                }
            paired_value = self._seal({
                "schema_version": 1,
                "artifact_kind": "paired_runtime_samples",
                "run_nonce": index,
                "measurement_sha256": measurement_record["sha256"],
                "measurement_content_sha256":
                    measurement_value["content_sha256"],
                "report_sha256": record(report)["sha256"],
                "raw_file_sha256": str(index) * 64,
                "source": source,
                "environment": environment,
                "algorithms": {
                    algorithm: [
                        {
                            "sample_ordinal": ordinal,
                            "generated_query_ns": 1000 + ordinal,
                            "handwritten_query_ns": 900 + ordinal,
                            "generated_to_handwritten_ratio":
                                (1000 + ordinal) / (900 + ordinal),
                        }
                        for ordinal in range(1, 16)
                    ]
                    for algorithm in (
                        "typed_wrapper_generated_vs_handwritten",
                        "row_codec_generated_vs_handwritten",
                    )
                },
            })
            paired = write(
                f"report-{index}-paired.json", paired_value)
            reports.append({
                **record(report),
                "run_nonce": index,
                "host_state_before":
                    host_states["before"]["record"],
                "host_state_after":
                    host_states["after"]["record"],
                "paired_runtime_samples": record(paired),
                "_host_states": host_states,
                "_paired_document": paired_value,
            })
        runtime_value = {
            "source": source,
            "environment": environment,
            "measurement_sha256": measurement_record["sha256"],
            "provenance": {
                "reports": [
                    {
                        "sha256": report["sha256"],
                        "run_nonce": report["run_nonce"],
                        "host_state": {
                            "before_file_sha256":
                                report["host_state_before"]["sha256"],
                            "before_content_sha256":
                                report["_host_states"]["before"][
                                    "document"]["content_sha256"],
                            "before":
                                report["_host_states"]["before"][
                                    "document"]["snapshot"],
                            "after_file_sha256":
                                report["host_state_after"]["sha256"],
                            "after_content_sha256":
                                report["_host_states"]["after"][
                                    "document"]["content_sha256"],
                            "after":
                                report["_host_states"]["after"][
                                    "document"]["snapshot"],
                        },
                        "paired_runtime_samples": {
                            "file_sha256":
                                report["paired_runtime_samples"]["sha256"],
                            "content_sha256":
                                report["_paired_document"][
                                    "content_sha256"],
                        },
                    }
                    for report in reports
                ],
            },
        }
        runtime = write("runtime.json", runtime_value)
        retained = {
            field: record(path)
            for field, path in source_files.items()
        }
        retained["artifact_inventory"] = record(
            write("artifact_inventory", b"artifact inventory"))
        retained["generator_output_inventory"] = record(
            generator_output_inventory)
        normalized_request = {
            "phase": "calibration_capture",
            "request_file_sha256": "b" * 64,
            "request_content_sha256": "c" * 64,
        }
        bundle = {
            "schema_version": 1,
            "artifact_kind":
                "doc0388_sql_codegen_guardrail_capture_bundle",
            "status": "unreviewed_calibration",
            "baseline_eligibility": "recovered_post_cutover_only",
            "capture_request": {
                "path": "/synthetic/request.json",
                "file_sha256": "b" * 64,
                "content_sha256": "c" * 64,
                "normalized_sha256":
                    server._canonical_json_sha256(normalized_request),
                "normalized_request": normalized_request,
            },
            "canonical_environment_key": environment["sha256"],
            "source": source,
            "environment": environment,
            "measurement": measurement_record,
            "runtime_samples": record(runtime),
            "calibration_reports": [
                {
                    key: value
                    for key, value in report.items()
                    if not key.startswith("_")
                }
                for report in reports
            ],
            "generated_artifacts": generated_records,
            "source_input_bindings": source_input_paths,
            "untracked_source_files": [],
            "generator_executable": record(generator),
            "production_dso": record(production),
            "benchmark_executable": record(benchmark),
            "comparator_dso": record(comparator),
            **retained,
        }
        sealed_bundle = self._seal(bundle)
        bundle_path = write("bundle.json", sealed_bundle)
        bundle_sha = hashlib.sha256(bundle_path.read_bytes()).hexdigest()

        loaded, _ = server._load_sql_guardrail_capture_bundle(
            bundle_path, expected_sha256=bundle_sha)
        self.assertEqual(
            loaded["canonical_environment_key"], environment["sha256"])

        (self.directory / "report-1.json").write_text(
            '{"mutated":true}', encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "hash changed"):
            server._load_sql_guardrail_capture_bundle(
                bundle_path, expected_sha256=bundle_sha)


class OrdinaryBenchE2eFilterTests(unittest.TestCase):
    def test_omitted_filter_selects_exact_preexisting_e2e_suite(self):
        bench_filter = server._ordinary_bench_e2e_default_filter(
            "bench-e2e",
            None,
            build_only=False,
            debug_report=False,
            filter_supplied=False,
            sql_guardrail_workflow=None,
        )

        self.assertEqual(
            bench_filter,
            {
                "benchmarks": [
                    "bench_e2e",
                    "bench_e2e_signoff",
                    "bench_e2e_gpu_batch_proof",
                    "bench_e2e_routing_poc",
                    "bench_sort_scan",
                ],
            },
        )
        cli_args, error = server._filter_to_cli_args(bench_filter)
        self.assertIsNone(error)
        self.assertEqual(
            cli_args,
            [
                "--filter",
                "benchmark=bench_e2e,bench_e2e_signoff,"
                "bench_e2e_gpu_batch_proof,bench_e2e_routing_poc,"
                "bench_sort_scan",
            ],
        )

    def test_explicit_filter_is_preserved(self):
        explicit = {
            "benchmarks": ["bench_sql_codegen_guardrail"],
        }

        actual = server._ordinary_bench_e2e_default_filter(
            "bench-e2e",
            explicit,
            build_only=False,
            debug_report=False,
            filter_supplied=True,
            sql_guardrail_workflow=None,
        )

        self.assertIs(actual, explicit)

    def test_explicit_null_filter_is_not_reinterpreted(self):
        actual = server._ordinary_bench_e2e_default_filter(
            "bench-e2e",
            None,
            build_only=False,
            debug_report=False,
            filter_supplied=True,
            sql_guardrail_workflow=None,
        )

        self.assertIsNone(actual)

    def test_sealed_workflow_keeps_its_guardrail_owned_selection(self):
        actual = server._ordinary_bench_e2e_default_filter(
            "bench-e2e",
            None,
            build_only=False,
            debug_report=False,
            filter_supplied=False,
            sql_guardrail_workflow={
                "phase": "candidate_qualification",
            },
        )

        self.assertIsNone(actual)

    def test_other_benchmark_modes_remain_unfiltered(self):
        actual = server._ordinary_bench_e2e_default_filter(
            "bench",
            None,
            build_only=False,
            debug_report=False,
            filter_supplied=False,
            sql_guardrail_workflow=None,
        )

        self.assertIsNone(actual)

    def test_build_only_and_unfocused_debug_keep_no_runtime_filter(self):
        for build_only, debug_report in (
            (True, False),
            (False, True),
        ):
            with self.subTest(
                    build_only=build_only,
                    debug_report=debug_report):
                actual = server._ordinary_bench_e2e_default_filter(
                    "bench-e2e",
                    None,
                    build_only=build_only,
                    debug_report=debug_report,
                    filter_supplied=False,
                    sql_guardrail_workflow=None,
                )

                self.assertIsNone(actual)


class UnitIntegrationQualificationTests(unittest.TestCase):
    def test_builder_builds_and_runs_e2e_serializer_contract(self):
        mode = server.MODES["unit+integration"]

        self.assertIn("bioseq_bench_e2e_tests_exec", mode.targets)
        self.assertIn(
            "bioseq_bench_e2e_tests_exec",
            [command[0] for command in mode.commands],
        )

    def test_builder_builds_and_runs_benchmark_framework_contracts(self):
        mode = server.MODES["unit+integration"]

        self.assertIn(
            "bioseq_bench_framework_example_tests_exec", mode.targets)
        self.assertIn(
            "bioseq_bench_framework_example_tests_exec",
            [command[0] for command in mode.commands],
        )

    def test_focused_filter_allows_zero_matches_in_every_binary(self):
        test_flags, error = server._unit_test_filter_to_cli_args(
            {"tags": ["[format5]"]})

        self.assertIsNone(error)
        self.assertEqual(
            test_flags,
            ["--allow-running-no-tests", "[format5]"],
        )
        binary_dir = Path("/tmp/test-opt/bioseq")
        resolved_commands = [
            server._run_command_argv(
                "unit+integration",
                binary_dir,
                command,
                test_flags=test_flags,
            )
            for command in server.MODES["unit+integration"].commands
        ]
        self.assertEqual(len(resolved_commands), 4)
        for command in resolved_commands:
            self.assertEqual(command[-2:], test_flags)


class UnitIntegrationCacheIsolationTests(unittest.TestCase):
    def test_optional_component_mode_invariants_are_explicit(self):
        self.assertEqual(
            server.MODES["unit+integration"].required_cache_variables,
            {
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
                "BIOSEQ_BUILD_WEBSITE": "OFF",
            },
        )
        self.assertEqual(
            server.MODES["external-api"].required_cache_variables,
            {
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "ON",
                "BIOSEQ_BUILD_WEBSITE": "OFF",
                "BIOSEQ_WEBSITE_BROWSER_TESTS": "OFF",
            },
        )
        self.assertEqual(
            server.MODES["website"].required_cache_variables,
            {
                "BIOSEQ_BUILD_WEBSITE": "ON",
                "BIOSEQ_WEBSITE_BROWSER_TESTS": "ON",
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
            },
        )

    def _run_optional_transition(
            self, mode, preset, cache_entries):
        commands = []
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / preset
            build_dir.mkdir()
            (build_dir / "build.ninja").touch()
            cache = build_dir / "CMakeCache.txt"
            cache.write_text(
                "BIOSEQ_TEST_INDEXES:BOOL=OFF\n" + "".join(
                    f"{name}:BOOL={value}\n"
                    for name, value in cache_entries.items()),
                encoding="utf-8",
            )
            log_path = Path(temporary) / "worker.log"
            state = {
                mode: {
                    "started_at": time.time(),
                    "log_file": str(log_path),
                    "rt_priority": None,
                },
            }

            def run(command, *_args, **_kwargs):
                commands.append(list(command))
                return 0

            with mock.patch.multiple(
                    server,
                    _refuse_root_build_identity=mock.DEFAULT,
                    _ensure_build_worker_timesharing_policy=mock.DEFAULT,
                    _load_state=mock.DEFAULT,
                    _update_entry=mock.DEFAULT,
                    preset_for=mock.DEFAULT,
                    build_dir_for=mock.DEFAULT,
                    bin_dir_for=mock.DEFAULT,
                    _build_worker_descendant_environment=mock.DEFAULT,
                    _ensure_local_toolchain_environment=mock.DEFAULT,
                    _configured_local_build_rt_priority=mock.DEFAULT,
                    _local_clang_rt_is_enabled=mock.DEFAULT,
                    _preflight_local_clang_rt_launcher=mock.DEFAULT,
                    _stream_subprocess=mock.DEFAULT,
                    _clang_rt_cmake_cache_repairs=mock.DEFAULT,
            ) as patched:
                patched["_refuse_root_build_identity"].return_value = None
                patched["_load_state"].return_value = state
                patched["preset_for"].return_value = preset
                patched["build_dir_for"].return_value = build_dir
                patched["bin_dir_for"].return_value = build_dir / "bioseq"
                patched["_build_worker_descendant_environment"].return_value = {}
                patched["_configured_local_build_rt_priority"].return_value = None
                patched["_local_clang_rt_is_enabled"].return_value = False
                patched["_stream_subprocess"].side_effect = run
                patched["_clang_rt_cmake_cache_repairs"].return_value = ([], [])

                result = server._run_build_worker(mode, build_only=True)

            self.assertEqual(result, 0, log_path.read_text(encoding="utf-8"))
        return commands

    def test_website_to_external_transition_is_atomic(self):
        commands = self._run_optional_transition(
            "external-api",
            "shared-opt",
            {
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
                "BIOSEQ_BUILD_WEBSITE": "ON",
                "BIOSEQ_WEBSITE_BROWSER_TESTS": "ON",
            },
        )
        self.assertEqual(commands[0][:3], ["cmake", "--preset", "shared-opt"])
        self.assertEqual(
            set(commands[0][3:]),
            {
                "-DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON",
                "-DBIOSEQ_BUILD_WEBSITE=OFF",
                "-DBIOSEQ_WEBSITE_BROWSER_TESTS=OFF",
            },
        )
        self.assertEqual(commands[1][:2], ["cmake", "--build"])

    def test_external_to_website_transition_is_atomic(self):
        commands = self._run_optional_transition(
            "website",
            "shared-opt",
            {
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "ON",
                "BIOSEQ_BUILD_WEBSITE": "OFF",
                "BIOSEQ_WEBSITE_BROWSER_TESTS": "OFF",
            },
        )
        self.assertEqual(commands[0][:3], ["cmake", "--preset", "shared-opt"])
        self.assertEqual(
            set(commands[0][3:]),
            {
                "-DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=OFF",
                "-DBIOSEQ_BUILD_WEBSITE=ON",
                "-DBIOSEQ_WEBSITE_BROWSER_TESTS=ON",
            },
        )
        self.assertEqual(commands[1][:2], ["cmake", "--build"])

    def test_external_and_website_cannot_share_a_live_build_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            shared_tree = Path(temporary) / "shared-opt"
            shared_tree.mkdir()
            shared_alias = Path(temporary) / "shared-alias"
            try:
                shared_alias.symlink_to(shared_tree, target_is_directory=True)
            except OSError:
                # Windows test hosts may deny unprivileged symlink creation;
                # exact path identity still pins the scheduler contract there.
                shared_alias = shared_tree

            cases = (
                ("external-api", "website", shared_alias),
                ("website", "external-api", shared_tree),
            )
            for requested_mode, running_mode, recorded_tree in cases:
                with self.subTest(
                        requested=requested_mode, running=running_mode):
                    state = {
                        running_mode: {
                            "status": "running",
                            "build_only": True,
                            "debug": False,
                            "build_dir": str(recorded_tree),
                        },
                    }
                    with mock.patch.object(
                            server, "_refresh",
                            side_effect=lambda current, candidate:
                            current.get(candidate)):
                        conflict_mode, conflict_entry = (
                            server._running_in_same_group(
                                state, requested_mode, True, shared_tree))

                    self.assertEqual(conflict_mode, running_mode)
                    self.assertIs(conflict_entry, state[running_mode])

    def test_legacy_running_entry_uses_its_recorded_debug_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            shared_debug_tree = Path(temporary) / "shared-debug"
            state = {
                "external-api": {
                    "status": "running",
                    "build_only": True,
                    "debug": True,
                },
            }

            def legacy_build_dir(mode, debug):
                self.assertEqual((mode, debug), ("external-api", True))
                return shared_debug_tree

            with mock.patch.object(
                    server, "_refresh",
                    side_effect=lambda current, candidate:
                    current.get(candidate)), mock.patch.object(
                        server, "build_dir_for",
                        side_effect=legacy_build_dir):
                conflict_mode, conflict_entry = server._running_in_same_group(
                    state, "website", True, shared_debug_tree)

            self.assertEqual(conflict_mode, "external-api")
            self.assertIs(conflict_entry, state["external-api"])

    def test_different_optional_mode_trees_remain_independent(self):
        with tempfile.TemporaryDirectory() as temporary:
            external_tree = Path(temporary) / "external-opt"
            website_tree = Path(temporary) / "website-opt"
            state = {
                "website": {
                    "status": "running",
                    "build_only": True,
                    "debug": False,
                    "build_dir": str(website_tree),
                },
            }
            with mock.patch.object(
                    server, "_refresh",
                    side_effect=lambda current, candidate:
                    current.get(candidate)):
                conflict = server._running_in_same_group(
                    state, "external-api", True, external_tree)

            self.assertEqual(conflict, (None, None))

    def test_doubly_polluted_tree_is_repaired_in_first_configure(self):
        commands = []
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "test-opt"
            build_dir.mkdir()
            (build_dir / "build.ninja").touch()
            cache = build_dir / "CMakeCache.txt"
            cache.write_text(
                "BIOSEQ_TEST_INDEXES:BOOL=OFF\n"
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS:BOOL=ON\n"
                "BIOSEQ_BUILD_WEBSITE:BOOL=ON\n",
                encoding="utf-8",
            )
            log_path = Path(temporary) / "worker.log"
            state = {
                "unit+integration": {
                    "started_at": time.time(),
                    "log_file": str(log_path),
                    "rt_priority": None,
                },
            }

            def run(command, *_args, **_kwargs):
                command = list(command)
                commands.append(command)
                if command[:3] == ["cmake", "--preset", "test-opt"]:
                    cache.write_text(
                        "BIOSEQ_TEST_INDEXES:BOOL=ON\n"
                        "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS:BOOL=OFF\n"
                        "BIOSEQ_BUILD_WEBSITE:BOOL=OFF\n",
                        encoding="utf-8",
                    )
                return 0

            with mock.patch.multiple(
                    server,
                    _refuse_root_build_identity=mock.DEFAULT,
                    _ensure_build_worker_timesharing_policy=mock.DEFAULT,
                    _load_state=mock.DEFAULT,
                    _update_entry=mock.DEFAULT,
                    preset_for=mock.DEFAULT,
                    build_dir_for=mock.DEFAULT,
                    bin_dir_for=mock.DEFAULT,
                    _build_worker_descendant_environment=mock.DEFAULT,
                    _ensure_local_toolchain_environment=mock.DEFAULT,
                    _configured_local_build_rt_priority=mock.DEFAULT,
                    _local_clang_rt_is_enabled=mock.DEFAULT,
                    _preflight_local_clang_rt_launcher=mock.DEFAULT,
                    _initial_clang_rt_cmake_args=mock.DEFAULT,
                    _stream_subprocess=mock.DEFAULT,
                    _clang_rt_cmake_cache_repairs=mock.DEFAULT,
            ) as patched:
                patched["_refuse_root_build_identity"].return_value = None
                patched["_load_state"].return_value = state
                patched["preset_for"].return_value = "test-opt"
                patched["build_dir_for"].return_value = build_dir
                patched["bin_dir_for"].return_value = build_dir / "bioseq"
                patched["_build_worker_descendant_environment"].return_value = {}
                patched["_configured_local_build_rt_priority"].return_value = None
                patched["_local_clang_rt_is_enabled"].return_value = False
                patched["_initial_clang_rt_cmake_args"].return_value = [
                    "-DCMAKE_C_COMPILER_LAUNCHER:FILEPATH=/trusted/launcher",
                    "-DCMAKE_CXX_COMPILER_LAUNCHER:FILEPATH=/trusted/launcher",
                ]
                patched["_stream_subprocess"].side_effect = run
                patched["_clang_rt_cmake_cache_repairs"].return_value = ([], [])

                result = server._run_build_worker(
                    "unit+integration", build_only=True)

            self.assertEqual(result, 0, log_path.read_text(encoding="utf-8"))

        repair = [
            "cmake", "--preset", "test-opt",
            "-DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=OFF",
            "-DBIOSEQ_BUILD_WEBSITE=OFF",
            "-DCMAKE_C_COMPILER_LAUNCHER:FILEPATH=/trusted/launcher",
            "-DCMAKE_CXX_COMPILER_LAUNCHER:FILEPATH=/trusted/launcher",
        ]
        self.assertEqual(commands[0], repair)
        first_build = next(
            index for index, command in enumerate(commands)
            if command[:2] == ["cmake", "--build"])
        self.assertLess(0, first_build)


class RemoteRunWebsiteRejectionTests(unittest.TestCase):
    """website is a local build-tree-only target (Doxygen + private PostgreSQL
    + pinned browser); remote-run must refuse it explicitly."""

    def test_website_is_a_valid_build_mode(self):
        # It must be a real mode, otherwise the rejection below is meaningless
        # (it would be caught by the generic invalid-mode guard instead).
        self.assertIn("website", server.VALID_MODES)

    def test_website_is_registered_as_rejected(self):
        self.assertIn("website", server.REMOTE_RUN_REJECTED_MODES)

    def test_remote_run_build_rejects_website(self):
        message = server.tool_remote_run({"operation": "build", "mode": "website"})

        self.assertIn("not supported by remote-run", message)
        self.assertIn("start_build mode=website", message)

    def test_remote_run_does_not_reject_ordinary_modes(self):
        # Every other build mode stays eligible for remote-run.
        self.assertEqual(
            server.REMOTE_RUN_REJECTED_MODES & server.VALID_MODES,
            {"website"},
        )


if __name__ == "__main__":
    unittest.main()
