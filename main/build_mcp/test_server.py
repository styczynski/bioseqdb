"""Focused unit tests for the safe local bench-e2e config override."""

from pathlib import Path
import hashlib
import json
import os
import socket
import subprocess
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

import server
from docs import qmd_manager
from docs.qmd_manager import QmdManager


class DocumentActivityContractTests(unittest.TestCase):
    def test_qmd_activity_snapshot_is_closed_and_phase_aware(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with mock.patch.object(
                qmd_manager, "ACTIVITY_REGISTRY", root / "activity.json",
            ), mock.patch.object(
                qmd_manager,
                "ACTIVITY_REGISTRY_LOCK",
                root / "activity.lock",
            ), mock.patch.object(
                qmd_manager,
                "QMD_OPERATION_LOCK",
                root / "operation.lock",
            ):
                manager = QmdManager()
                manager.client.health = mock.Mock(return_value=True)
                self.assertEqual(manager.activity_snapshot(), {
                    "document_update": {
                        "queued": 0, "running": 0, "state": "absent"},
                    "document_embed": {
                        "queued": 0, "running": 0, "state": "absent"},
                    "qmd_server": {"persistent": True, "state": "idle"},
                })

                manager.begin_external_update()
                manager._queue_index()
                snapshot = manager.activity_snapshot()
                self.assertEqual(
                    snapshot["document_update"],
                    {"queued": 1, "running": 1, "state": "running"},
                )
                self.assertEqual(
                    snapshot["document_embed"],
                    {"queued": 1, "running": 0, "state": "queued"},
                )
                self.assertEqual(
                    snapshot["qmd_server"],
                    {"persistent": True, "state": "busy"},
                )
                with manager._activity_lock:
                    manager._dirty_pending = False
                    manager._dirty.clear()
                    manager._external_update_running = 0
                    manager._ready = True
                    manager._publish_process_activity_locked()
                idle = manager.activity_snapshot()
                self.assertEqual(
                    idle["document_update"],
                    {"queued": 0, "running": 0, "state": "idle"},
                )
                self.assertEqual(
                    idle["document_embed"],
                    {"queued": 0, "running": 0, "state": "idle"},
                )

    def test_qmd_readiness_balances_activity_when_lock_setup_fails(self):
        manager = QmdManager()
        with mock.patch.object(
            manager, "begin_external_update",
        ) as begin, mock.patch.object(
            manager, "end_external_update",
        ) as end, mock.patch.object(
            manager, "_lock_file", side_effect=OSError("lock failed"),
        ), self.assertRaisesRegex(OSError, "lock failed"):
            manager.ensure_ready()
        begin.assert_called_once_with()
        end.assert_called_once_with()

    def test_qmd_readiness_balances_activity_when_unlock_fails(self):
        manager = QmdManager()
        handle = object()
        with mock.patch.object(
            manager, "begin_external_update",
        ), mock.patch.object(
            manager, "end_external_update",
        ) as end, mock.patch.object(
            manager, "_lock_file", return_value=handle,
        ), mock.patch.object(
            manager, "_unlock_file", side_effect=OSError("unlock failed"),
        ), mock.patch.object(
            manager, "_ensure_collections",
        ), mock.patch.object(
            manager, "_ensure_server",
        ), mock.patch.object(
            manager, "_start_worker",
        ), mock.patch.object(
            manager, "_queue_index",
        ), self.assertRaisesRegex(OSError, "unlock failed"):
            manager.ensure_ready()
        end.assert_called_once_with()

    def test_qmd_dead_process_presence_is_unknown(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            activity_path = root / "activity.json"
            activity_path.write_text(json.dumps({
                "99999999": {
                    "pid": 99999999,
                    "initialized": True,
                    "update_queued": 0,
                    "update_running": 0,
                    "embed_queued": 0,
                    "embed_running": 0,
                },
            }), encoding="utf-8")
            with mock.patch.object(
                qmd_manager, "ACTIVITY_REGISTRY", activity_path,
            ), mock.patch.object(
                qmd_manager,
                "ACTIVITY_REGISTRY_LOCK",
                root / "activity.lock",
            ), mock.patch.object(
                QmdManager, "_pid_alive", return_value=False,
            ):
                manager = QmdManager()
                manager.client.health = mock.Mock(return_value=True)
                snapshot = manager.activity_snapshot()
        self.assertEqual(snapshot["document_update"]["state"], "unknown")
        self.assertEqual(snapshot["document_embed"]["state"], "unknown")
        self.assertEqual(snapshot["qmd_server"]["state"], "unknown")


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
    def test_builder_co_schedules_independent_compile_graphs(self):
        mode = server.MODES["unit+integration"]

        self.assertTrue(mode.build_targets_together)
        self.assertEqual(
            server._build_target_groups_for("unit+integration"),
            (tuple(mode.targets),),
        )
        self.assertEqual(
            server._cmake_build_argv(
                Path("/tmp/test-opt"), tuple(mode.targets)),
            [
                "cmake", "--build", "/tmp/test-opt", "--target",
                *mode.targets, "--parallel", str(server.CMAKE_PARALLEL_JOBS),
            ],
        )
        self.assertFalse(server.MODES["bench"].build_targets_together)
        self.assertEqual(
            server._build_target_groups_for("bench"),
            tuple((target,) for target in server.MODES["bench"].targets),
        )

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
            configure_stamp = {
                "artifact_kind": "test-configure-stamp",
                "schema_version": 1,
                "inputs": {"preset": preset},
                "inputs_sha256": "0" * 64,
            }
            (build_dir / server._CMAKE_CONFIGURE_STAMP_NAME).write_text(
                json.dumps(configure_stamp),
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
                    _cmake_preset_cache_contract=mock.DEFAULT,
                    _cmake_preset_cache_repairs=mock.DEFAULT,
                    _cmake_configure_stamp_document=mock.DEFAULT,
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
                patched["_cmake_preset_cache_contract"].return_value = {}
                patched["_cmake_preset_cache_repairs"].return_value = {}
                patched["_cmake_configure_stamp_document"].return_value = (
                    configure_stamp)

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
                    _cmake_preset_cache_contract=mock.DEFAULT,
                    _cmake_preset_cache_repairs=mock.DEFAULT,
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
                patched["_cmake_preset_cache_contract"].return_value = {}
                patched["_cmake_preset_cache_repairs"].return_value = {}

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
        self.assertNotIn("unit+integration", server.REMOTE_RUN_REJECTED_MODES)
        self.assertIn("unit+integration", server.REMOTE_RUN_BUILD_MODES)


class RemoteActivityReservationTests(unittest.TestCase):
    def test_remote_slot_is_reserved_before_worker_spawn(self):
        process = SimpleNamespace(pid=43210)
        saved = []
        running = {
            "op_id": "observed",
            "kind": "status",
            "label": "status",
            "status": "running",
            "step": "running",
            "started_at": time.time(),
            "pid": process.pid,
            "log_file": "/tmp/remote.log",
        }
        with mock.patch.object(
            server, "_remote_global_lock",
        ), mock.patch.object(
            server, "_load_remote_state", return_value={},
        ), mock.patch.object(
            server, "_refresh_remote", side_effect=lambda value: value,
        ), mock.patch.object(
            server, "_save_remote_state",
            side_effect=lambda value: saved.append(dict(value)),
        ), mock.patch.object(
            server.subprocess, "Popen", return_value=process,
        ), mock.patch.object(
            server, "_remote_grace_wait", return_value=running,
        ):
            server._start_or_poll_remote(
                "status", "status", argv=["provisioner", "status"])

        self.assertEqual(saved[0]["pid"], os.getpid())
        self.assertEqual(saved[0]["step"], "spawning")
        self.assertEqual(saved[1]["pid"], process.pid)

    def test_remote_worker_waits_for_exact_pid_handoff(self):
        op_id = "status-test"
        started = time.time()
        with tempfile.TemporaryDirectory() as temporary:
            log_file = str(Path(temporary) / "remote.log")
            scheduler_state = {
                "op_id": op_id,
                "status": "running",
                "pid": os.getpid() + 100000,
            }
            worker_state = {
                "op_id": op_id,
                "kind": "status",
                "label": "status",
                "status": "running",
                "step": "spawning",
                "started_at": started,
                "pid": os.getpid(),
                "log_file": log_file,
                "timeout_sec": 60,
                "argv": ["provisioner", "status"],
            }
            with mock.patch.object(
                server,
                "_load_remote_state",
                side_effect=(scheduler_state, worker_state),
            ) as load, mock.patch.object(
                server.time, "sleep",
            ) as sleeper, mock.patch.object(
                server, "_update_remote_entry",
            ), mock.patch.object(
                server, "_remote_exec", return_value=0,
            ):
                self.assertEqual(server._run_remote_worker(op_id), 0)

        self.assertEqual(load.call_count, 2)
        sleeper.assert_called_once_with(0.1)


class BuildReceiptWorkflowTests(unittest.TestCase):
    @staticmethod
    def _write_receipted_bench_command(build_dir: Path) -> Path:
        bin_dir = build_dir / "bioseq"
        bin_dir.mkdir(parents=True, exist_ok=True)
        command = bin_dir / "bioseq_bench"
        command.write_bytes(b"test benchmark executable\n")
        command.chmod(0o755)
        return command

    def test_public_schemas_expose_guard_and_receipt_controls(self):
        start_schema = next(
            item for item in server.TOOL_DEFINITIONS
            if item["name"] == "start_build"
        )["inputSchema"]
        inspect_schema = next(
            item for item in server.TOOL_DEFINITIONS
            if item["name"] == "inspect"
        )["inputSchema"]
        self.assertTrue({
            "fresh",
            "build_timeout_sec",
            "baseline_ref",
            "run_only",
            "reuse_build_receipt",
        }.issubset(start_schema["properties"]))
        self.assertEqual(
            start_schema["properties"]["build_timeout_sec"]["maximum"],
            3300,
        )
        self.assertEqual(
            start_schema["properties"]["baseline_ref"]["const"],
            "refs/heads/main",
        )
        self.assertIn(
            "reuse_build_receipt", inspect_schema["properties"])

    def test_status_schema_is_closed_and_accepts_empty_aggregate_request(self):
        status_schema = next(
            item for item in server.TOOL_DEFINITIONS
            if item["name"] == "status"
        )["inputSchema"]
        self.assertEqual(len(status_schema["oneOf"]), 2)
        aggregate, per_mode = status_schema["oneOf"]
        self.assertFalse(aggregate["additionalProperties"])
        self.assertEqual(aggregate["properties"], {})
        self.assertFalse(per_mode["additionalProperties"])
        self.assertEqual(per_mode["required"], ["mode"])
        self.assertEqual(
            per_mode["properties"]["mode"]["enum"],
            sorted(server.VALID_MODES),
        )

    def test_status_aggregate_has_exact_path_free_closed_shape(self):
        document = {
            "document_update": {"queued": 0, "running": 0, "state": "idle"},
            "document_embed": {"queued": 0, "running": 0, "state": "idle"},
            "qmd_server": {"persistent": True, "state": "idle"},
        }
        registries = (
            ({"bench": {"status": "completed"}}, True),
            ({}, False),
            ({}, False),
        )
        with mock.patch.object(
            server, "_read_activity_registry", side_effect=registries,
        ), mock.patch.object(
            server, "_document_activity", return_value=document,
        ), mock.patch.object(
            server.shutil, "disk_usage", return_value=SimpleNamespace(free=12345),
        ):
            response = json.loads(server.tool_status())

        self.assertEqual(set(response), {"local_activity"})
        activity = response["local_activity"]
        self.assertEqual(
            set(activity), {"quiescent", "free_disk_bytes", "workers"})
        self.assertTrue(activity["quiescent"])
        self.assertEqual(activity["free_disk_bytes"], 12345)
        self.assertEqual(
            set(activity["workers"]),
            {
                "build", "document_update", "document_embed",
                "mcp_subagent", "papers", "remote_operation",
                "other_detached", "qmd_server",
            },
        )
        for name in set(activity["workers"]) - {"qmd_server"}:
            self.assertEqual(
                set(activity["workers"][name]), {"queued", "running", "state"})
        self.assertEqual(
            set(activity["workers"]["qmd_server"]), {"persistent", "state"})
        self.assertNotIn("/", json.dumps(response, sort_keys=True))

    def test_status_aggregate_fails_closed_on_malformed_registry(self):
        document = {
            "document_update": {"queued": 0, "running": 0, "state": "absent"},
            "document_embed": {"queued": 0, "running": 0, "state": "absent"},
            "qmd_server": {"persistent": False, "state": "absent"},
        }
        with mock.patch.object(
            server,
            "_read_activity_registry",
            side_effect=((None, True), ({}, False), ({}, False)),
        ), mock.patch.object(
            server, "_document_activity", return_value=document,
        ), mock.patch.object(
            server.shutil, "disk_usage", return_value=SimpleNamespace(free=1),
        ):
            activity = json.loads(server.tool_status())["local_activity"]
        self.assertFalse(activity["quiescent"])
        self.assertEqual(activity["workers"]["build"]["state"], "unknown")
        self.assertEqual(activity["workers"]["papers"]["state"], "unknown")
        self.assertEqual(
            activity["workers"]["other_detached"]["state"], "unknown")

    def test_status_dispatch_rejects_unknown_arguments(self):
        with self.assertRaisesRegex(RuntimeError, "unknown argument"):
            server._handle_tools_call({
                "name": "status",
                "arguments": {"unexpected": True},
            })

    def test_status_dispatch_enforces_the_closed_schema(self):
        for arguments in (None, [], False, "", {"mode": None}):
            with self.subTest(arguments=arguments), self.assertRaisesRegex(
                RuntimeError, "arguments must be an object|mode must be a string"
            ):
                server._handle_tools_call({
                    "name": "status",
                    "arguments": arguments,
                })
        with self.assertRaisesRegex(RuntimeError, "supported build mode"):
            server._handle_tools_call({
                "name": "status",
                "arguments": {"mode": "not-a-mode"},
            })

    def test_activity_marks_unverified_terminal_cleanup_unknown(self):
        for entry in (
            {"status": "aborted"},
            {"status": "killed_timeout"},
            {"status": "completed", "cleanup_unverified": True},
            {
                "status": "failed",
                "termination_evidence": {"cleanup_verified": False},
            },
            {
                "status": "completed",
                "termination_evidence": {
                    "cleanup_verified": True,
                    "survivor_pids": [123],
                },
            },
        ):
            with self.subTest(entry=entry):
                self.assertEqual(
                    server._transient_activity([entry])["state"], "unknown")
        verified_timeout = {
            "status": "killed_timeout",
            "termination_evidence": {"cleanup_verified": True},
            "cleanup_unverified": False,
        }
        self.assertEqual(
            server._transient_activity([verified_timeout])["state"], "idle")

    def test_public_input_matrix_fails_closed_before_scheduling(self):
        invalid = (
            ({"fresh": True}, "requires build_only"),
            ({"build_only": True, "build_timeout_sec": 3301}, "between 60"),
            ({"build_only": True, "build_timeout_sec": 3300},
             "requires fresh=true"),
            ({"run_only": True}, "requires reuse_build_receipt"),
            ({"build_only": True, "run_only": True}, "mutually exclusive"),
            ({"reuse_build_receipt": "br_x"}, "only with run_only"),
            ({
                "build_only": True,
                "baseline_ref": "refs/heads/main",
            }, "requires build_only=true and fresh=true"),
            ({
                "build_only": True,
                "fresh": True,
                "baseline_ref": "refs/heads/main",
            }, "only for benchmark modes"),
        )
        with mock.patch.object(
            server, "_refuse_root_build_identity", return_value=None,
        ), mock.patch.object(
            server, "_ensure_build_worker_timesharing_policy",
        ):
            for request, expected in invalid:
                with self.subTest(request=request):
                    message = server.tool_start_build(
                        "unit+integration", **request)
                    self.assertIn(expected, message)

    def test_guard_terms_at_54_minutes_and_kills_by_55(self):
        soft, hard = server._build_phase_deadlines(100.0, 3300)
        self.assertEqual(soft, 100.0 + 54 * 60)
        self.assertEqual(hard, 100.0 + 55 * 60)

    def test_start_rechecks_cleanup_after_running_entry_refresh(self):
        with mock.patch.object(
            server, "_refuse_root_build_identity", return_value=None,
        ), mock.patch.object(
            server, "_ensure_build_worker_timesharing_policy",
        ), mock.patch.object(
            server, "_git_commit", return_value="a" * 40,
        ), mock.patch.object(
            server, "_source_graph_fingerprint", return_value="fingerprint",
        ), mock.patch.object(
            server, "_load_state", return_value={},
        ), mock.patch.object(
            server,
            "_unverified_guard_cleanup",
            side_effect=(None, "newly discovered unverified cleanup"),
        ) as cleanup, mock.patch.object(
            server, "_running_in_same_group", return_value=(None, None),
        ) as refresh, mock.patch.object(server.subprocess, "Popen") as popen:
            message = server.tool_start_build(
                "unit+integration",
                build_only=True,
                fresh=True,
                build_timeout_sec=3300,
            )

        self.assertIn("newly discovered unverified cleanup", message)
        self.assertEqual(cleanup.call_count, 2)
        refresh.assert_called_once()
        popen.assert_not_called()

    def test_source_fingerprint_ignores_docs_build_and_reports(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "main/DOCS/CURRENT").mkdir(parents=True)
            (root / "main/build/x").mkdir(parents=True)
            (root / "main/reporting").mkdir(parents=True)
            (root / "main/bioseq").mkdir(parents=True)
            (root / "main/bioseq/source.cppm").write_text(
                "export module source;\n", encoding="utf-8")
            (root / "main/DOCS/CURRENT/log.md").write_text(
                "one\n", encoding="utf-8")
            (root / "main/build/x/output").write_text(
                "one\n", encoding="utf-8")
            (root / "main/reporting/report.json").write_text(
                "{}\n", encoding="utf-8")
            self.assertEqual(
                subprocess.run(
                    ["git", "init", "-q", str(root)], check=False,
                ).returncode,
                0,
            )
            first = server._source_graph_fingerprint(root)
            (root / "main/DOCS/CURRENT/log.md").write_text(
                "two\n", encoding="utf-8")
            (root / "main/build/x/output").write_text(
                "two\n", encoding="utf-8")
            (root / "main/reporting/report.json").write_text(
                '{"changed":true}\n', encoding="utf-8")
            self.assertEqual(first, server._source_graph_fingerprint(root))
            (root / "main/bioseq/source.cppm").write_text(
                "export module changed;\n", encoding="utf-8")
            self.assertNotEqual(first, server._source_graph_fingerprint(root))

    def test_source_fingerprint_excludes_papers_in_git_argv(self):
        completed = subprocess.CompletedProcess([], 0, b"", b"")
        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(
                server.subprocess, "run", return_value=completed) as run:
            server._source_graph_fingerprint(Path(temporary))
        argv = run.call_args.args[0]
        self.assertEqual(argv[-6:], [
            "--",
            ".",
            ":(top,exclude,literal)main/papers",
            ":(top,exclude,glob)main/papers/**",
            ":(top,exclude,literal)papers",
            ":(top,exclude,glob)papers/**",
        ])

    def test_baseline_worktree_is_sparse_before_checkout(self):
        commit = "a" * 40
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline_root = root / "baselines"
            expected_worktree = baseline_root / commit[:16] / "run"
            calls = []

            def run(command, **kwargs):
                calls.append(command)
                if command[3:5] == ["worktree", "add"]:
                    presets = expected_worktree / "main/CMakePresets.json"
                    presets.parent.mkdir(parents=True)
                    presets.write_text("{}\n", encoding="utf-8")
                return subprocess.CompletedProcess(command, 0, "", "")

            with mock.patch.object(
                server, "WORKSPACE_ROOT", root,
            ), mock.patch.object(
                server, "BASELINE_WORKTREE_ROOT", baseline_root,
            ), mock.patch.object(
                server, "_git_commit", return_value=commit,
            ), mock.patch.object(server.subprocess, "run", side_effect=run):
                project_root, observed = server._create_baseline_worktree(
                    "refs/heads/main", "run")

        self.assertEqual(observed, commit)
        self.assertEqual(project_root, expected_worktree / "main")
        self.assertIn("--no-checkout", calls[0])
        self.assertEqual(calls[1][-4:], [
            "--no-cone", "/*", "!/main/papers/", "!/papers/",
        ])
        self.assertEqual(calls[2][-3:], ["--detach", commit])
        self.assertEqual(calls[3][-5:], [
            ".",
            ":(top,exclude,literal)main/papers",
            ":(top,exclude,glob)main/papers/**",
            ":(top,exclude,literal)papers",
            ":(top,exclude,glob)papers/**",
        ])

    def test_failed_sparse_baseline_uses_exact_worktree_cleanup(self):
        commit = "b" * 40
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline_root = root / "baselines"
            expected = baseline_root / commit[:16] / "run"
            results = (
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 1, "", "sparse failed"),
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess(
                    [], 0, f"worktree {root}\n", ""),
            )
            with mock.patch.object(
                server, "WORKSPACE_ROOT", root,
            ), mock.patch.object(
                server, "BASELINE_WORKTREE_ROOT", baseline_root,
            ), mock.patch.object(
                server, "_git_commit", return_value=commit,
            ), mock.patch.object(
                server.subprocess, "run", side_effect=results,
            ) as run, self.assertRaisesRegex(
                RuntimeError, "papers-excluding baseline projection"):
                server._create_baseline_worktree("refs/heads/main", "run")

        remove = run.call_args_list[2].args[0]
        prune = run.call_args_list[3].args[0]
        self.assertEqual(remove[-3:], ["remove", "--force", str(expected)])
        self.assertEqual(prune[-2:], ["worktree", "prune"])

    def test_failed_worktree_add_still_verifies_exact_cleanup(self):
        commit = "c" * 40
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline_root = root / "baselines"
            expected = baseline_root / commit[:16] / "run"
            results = (
                subprocess.CompletedProcess([], 1, "", "add failed"),
                subprocess.CompletedProcess([], 1, "", "not registered"),
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess(
                    [], 0, f"worktree {root}\n", ""),
            )
            with mock.patch.object(
                server, "WORKSPACE_ROOT", root,
            ), mock.patch.object(
                server, "BASELINE_WORKTREE_ROOT", baseline_root,
            ), mock.patch.object(
                server, "_git_commit", return_value=commit,
            ), mock.patch.object(
                server.subprocess, "run", side_effect=results,
            ) as run, self.assertRaisesRegex(
                RuntimeError, "could not create detached baseline"):
                server._create_baseline_worktree(
                    "refs/heads/main", "run")

        self.assertEqual(
            run.call_args_list[1].args[0][-3:],
            ["remove", "--force", str(expected)],
        )
        self.assertEqual(
            run.call_args_list[-1].args[0][-3:],
            ["worktree", "list", "--porcelain"],
        )

    def test_timed_out_worktree_add_still_verifies_exact_cleanup(self):
        commit = "e" * 40
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline_root = root / "baselines"
            results = iter((
                subprocess.TimeoutExpired(["git", "worktree", "add"], 120),
                subprocess.CompletedProcess([], 1, "", "not registered"),
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess(
                    [], 0, f"worktree {root}\n", ""),
            ))

            def run(*_args, **_kwargs):
                value = next(results)
                if isinstance(value, Exception):
                    raise value
                return value

            with mock.patch.object(
                server, "WORKSPACE_ROOT", root,
            ), mock.patch.object(
                server, "BASELINE_WORKTREE_ROOT", baseline_root,
            ), mock.patch.object(
                server, "_git_commit", return_value=commit,
            ), mock.patch.object(
                server.subprocess, "run", side_effect=run,
            ), self.assertRaises(subprocess.TimeoutExpired):
                server._create_baseline_worktree(
                    "refs/heads/main", "run")

    def test_unverified_worktree_cleanup_surfaces_combined_failure(self):
        commit = "d" * 40
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline_root = root / "baselines"
            expected = baseline_root / commit[:16] / "run"
            listed = f"worktree {expected}\n"
            for remove_result in (
                subprocess.CompletedProcess([], 1, "", "remove failed"),
                subprocess.TimeoutExpired(["git", "worktree", "remove"], 120),
            ):
                results = iter((
                    subprocess.CompletedProcess([], 1, "", "add failed"),
                    remove_result,
                    subprocess.CompletedProcess([], 0, "", ""),
                    subprocess.CompletedProcess([], 0, listed, ""),
                ))

                def run(*_args, **_kwargs):
                    value = next(results)
                    if isinstance(value, Exception):
                        raise value
                    return value

                with self.subTest(remove=type(remove_result).__name__), mock.patch.object(
                    server, "WORKSPACE_ROOT", root,
                ), mock.patch.object(
                    server, "BASELINE_WORKTREE_ROOT", baseline_root,
                ), mock.patch.object(
                    server, "_git_commit", return_value=commit,
                ), mock.patch.object(
                    server.subprocess, "run", side_effect=run,
                ), self.assertRaisesRegex(
                    RuntimeError, "cleanup was not verified"):
                    server._create_baseline_worktree(
                        "refs/heads/main", "run")

    def test_descendant_capture_includes_new_sessions(self):
        root = server._ProcessIdentity(10, 1, 10, 10, "start-root", "cmake")
        child = server._ProcessIdentity(11, 10, 11, 11, "start-child", "ninja")
        compiler = server._ProcessIdentity(
            12, 11, 12, 12, "start-compiler", "clang")
        with mock.patch.object(
            server,
            "_process_snapshot",
            return_value={10: root, 11: child, 12: compiler},
        ):
            groups, evidence = server._validated_process_tree(root)
        self.assertEqual(set(groups), {10, 11, 12})
        self.assertEqual(evidence["descendant_pids"], [10, 11, 12])

    def test_receipt_round_trip_and_content_tamper(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project = root / "main"
            build_dir = project / "build/mcp/run/test-opt"
            build_dir.mkdir(parents=True)
            receipt_path = root / "receipts.json"
            host = {
                "node": server.host_platform.node(),
                "system": server.host_platform.system(),
                "machine": server.host_platform.machine(),
                "release": server.host_platform.release(),
            }
            receipt = {
                "kind": "bioseq-build-receipt-v1",
                "schema_version": 1,
                "build_receipt_id": "br_round_trip",
                "status": "completed",
                "exit_code": 0,
                "created_at": 1.0,
                "mode": "unit+integration",
                "debug": False,
                "fresh": True,
                "preset": "test-opt",
                "preset_family": "test",
                "binary_subdir": "bioseq",
                "targets": list(
                    server.MODES["unit+integration"].targets),
                "source_project_root": str(project),
                "source_workspace_root": str(root),
                "source_commit": "a" * 40,
                "source_graph_fingerprint": "source-digest",
                "baseline_ref": None,
                "baseline_commit": None,
                "build_dir": str(build_dir),
                "build_timeout_sec": 3300,
                "build_phase_started_at": 1.0,
                "configure_completed_at": 2.0,
                "configure_duration_sec": 1.0,
                "build_phase_completed_at": 3.0,
                "build_phase_duration_sec": 2.0,
                "target_timings": [],
                "termination_evidence": None,
                "configuration": {"sealed": True},
                "runtime_artifacts": [],
                "authorized_artifacts": [{"sealed": True}],
                "artifact_counts": {"sealed": 1},
                "host": host,
            }
            receipt["content_sha256"] = server._canonical_json_sha256(
                receipt)
            with mock.patch.object(
                server, "BUILD_RECEIPT_FILE", receipt_path,
            ), mock.patch.object(
                server, "_source_graph_fingerprint",
                return_value="source-digest",
            ), mock.patch.object(
                server, "_git_commit", return_value="a" * 40,
            ), mock.patch.object(
                server, "_build_configuration_evidence",
                return_value={"sealed": True},
            ), mock.patch.object(
                server, "_runtime_artifact_evidence", return_value=[],
            ), mock.patch.object(
                server,
                "_receipt_authorized_artifact_evidence",
                return_value=[{"sealed": True}],
            ), mock.patch.object(
                server, "_build_artifact_counts",
                return_value={"sealed": 1},
            ):
                server._store_build_receipt(receipt)
                observed, error = server._validate_build_receipt(
                    "br_round_trip", expected_mode="unit+integration")
                self.assertIsNone(error)
                self.assertEqual(
                    observed["build_receipt_id"], "br_round_trip")
                ledger = json.loads(receipt_path.read_text(encoding="utf-8"))
                ledger["br_round_trip"]["mode"] = "bench"
                receipt_path.write_text(
                    json.dumps(ledger), encoding="utf-8")
                observed, error = server._validate_build_receipt(
                    "br_round_trip")
                self.assertIsNone(observed)
                self.assertIn("content seal", error)

    def test_reparented_survivor_is_still_killed_by_start_identity(self):
        root = server._ProcessIdentity(20, 1, 20, 20, "root-start", "cmake")
        child = server._ProcessIdentity(21, 20, 21, 21, "child-start", "clang")
        reparented = server._ProcessIdentity(
            21, 1, 21, 21, "child-start", "clang --still-running")
        snapshots = [
            {20: root, 21: child},
            {21: reparented},
            {21: reparented},
        ]
        clock = iter((0.0, 0.0, 10.0, 10.0, 10.0, 10.0))
        killed = []
        with mock.patch.object(
            server, "_process_snapshot", side_effect=snapshots,
        ), mock.patch.object(
            server.os, "killpg", side_effect=lambda pgid, sig: killed.append(
                (pgid, sig)),
        ), mock.patch.object(
            server.time, "time", side_effect=lambda: next(clock),
        ), mock.patch.object(server.time, "sleep"):
            evidence = server._terminate_validated_process_tree(
                root, hard_kill_deadline=10.0)
        self.assertIn((21, server.signal.SIGKILL), killed)
        self.assertIn(21, evidence["survivor_pids"])

    def test_artifact_authority_detects_extension_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            bin_dir = build_dir / "bioseq"
            bin_dir.mkdir()
            for command in server.MODES["unit+integration"].commands:
                path = bin_dir / command[0]
                path.write_bytes(b"test executable\n")
                path.chmod(0o755)
            suffix = server._native_shared_library_suffix(
                server.host_platform.system())
            (build_dir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")
            core = bin_dir / f"bioseq{suffix}"
            cache = (
                build_dir
                / "bioseq_caching/artifacts/cache_production/Release"
                / f"bioseq_cache{suffix}"
            )
            cache.parent.mkdir(parents=True)
            core.write_bytes(b"core dso\n")
            cache.write_bytes(b"cache dso\n")
            (build_dir / "cmake_install.cmake").write_text(
                "# install plan\n", encoding="utf-8")
            (bin_dir / "bioseq.control").write_text(
                "default_version='1.0'\n", encoding="utf-8")
            (bin_dir / "bioseq--1.0.sql").write_text(
                "SELECT 1;\n", encoding="utf-8")

            before = server._receipt_authorized_artifact_evidence(
                "unit+integration", build_dir, debug=False)
            core.write_bytes(b"mutated core dso\n")
            after = server._receipt_authorized_artifact_evidence(
                "unit+integration", build_dir, debug=False)

        self.assertNotEqual(before, after)
        relative_paths = {item["relative_path"] for item in before}
        self.assertIn(str(core.relative_to(build_dir)), relative_paths)
        self.assertIn(str(cache.relative_to(build_dir)), relative_paths)
        self.assertIn("cmake_install.cmake", relative_paths)

    def test_artifact_authority_seals_vcpkg_library_alias_chains(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            self._write_receipted_bench_command(build_dir)
            expected = {}
            for relative_library_dir in (
                Path("vcpkg_installed/test-triplet/lib"),
                Path("vcpkg_installed/test-triplet/debug/lib"),
            ):
                library_dir = build_dir / relative_library_dir
                library_dir.mkdir(parents=True)
                terminal = library_dir / "libvulkan.1.4.341.dylib"
                terminal.write_bytes(
                    b"debug vulkan\n"
                    if "debug" in relative_library_dir.parts
                    else b"release vulkan\n")
                middle = library_dir / "libvulkan.1.dylib"
                entry = library_dir / "libvulkan.dylib"
                middle.symlink_to(terminal.name)
                entry.symlink_to(middle.name)
                expected[str(entry.relative_to(build_dir))] = (
                    middle.name,
                    str(terminal.relative_to(build_dir)),
                    hashlib.sha256(terminal.read_bytes()).hexdigest(),
                )

            evidence = server._receipt_authorized_artifact_evidence(
                "bench", build_dir, debug=False)

        by_path = {item["relative_path"]: item for item in evidence}
        for alias_path, (direct_target, terminal_path, digest) in expected.items():
            alias = by_path[alias_path]
            self.assertEqual(
                alias["artifact_kind"], "vcpkg-shared-library-alias")
            self.assertEqual(alias["link_target"], direct_target)
            self.assertEqual(len(alias["symlink_chain"]), 2)
            self.assertEqual(alias["target_relative_path"], terminal_path)
            self.assertEqual(alias["target_sha256"], digest)
            self.assertEqual(by_path[terminal_path]["sha256"], digest)
            middle_path = str(Path(alias_path).parent / direct_target)
            self.assertEqual(
                by_path[middle_path]["artifact_kind"],
                "vcpkg-shared-library-alias",
            )
            self.assertEqual(len(by_path[middle_path]["symlink_chain"]), 1)
        self.assertEqual(
            [item["relative_path"] for item in evidence],
            sorted(item["relative_path"] for item in evidence),
        )

    def test_artifact_authority_seals_same_content_alias_retarget(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            self._write_receipted_bench_command(build_dir)
            library_dir = (
                build_dir / "vcpkg_installed/test-triplet/lib")
            library_dir.mkdir(parents=True)
            first = library_dir / "libchoice.1.dylib"
            second = library_dir / "libchoice.2.dylib"
            first.write_bytes(b"same library bytes\n")
            second.write_bytes(b"same library bytes\n")
            alias = library_dir / "libchoice.dylib"
            alias.symlink_to(first.name)
            before = server._receipt_authorized_artifact_evidence(
                "bench", build_dir, debug=False)
            alias.unlink()
            alias.symlink_to(second.name)
            after = server._receipt_authorized_artifact_evidence(
                "bench", build_dir, debug=False)

        before_regular = [
            item for item in before if "artifact_kind" not in item]
        after_regular = [
            item for item in after if "artifact_kind" not in item]
        before_alias = next(
            item for item in before
            if item["relative_path"].endswith("libchoice.dylib"))
        after_alias = next(
            item for item in after
            if item["relative_path"].endswith("libchoice.dylib"))
        self.assertEqual(before_regular, after_regular)
        self.assertNotEqual(before_alias, after_alias)
        self.assertEqual(before_alias["link_target"], first.name)
        self.assertEqual(after_alias["link_target"], second.name)

    def test_artifact_authority_rejects_alias_replacement_during_capture(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            self._write_receipted_bench_command(build_dir)
            library_dir = (
                build_dir / "vcpkg_installed/test-triplet/lib")
            library_dir.mkdir(parents=True)
            first = library_dir / "libchoice.1.dylib"
            second = library_dir / "libchoice.2.dylib"
            first.write_bytes(b"first library\n")
            second.write_bytes(b"second library\n")
            alias = library_dir / "libchoice.dylib"
            alias.symlink_to(first.name)
            original_readlink = server.os.readlink
            replaced = False

            def replace_after_readlink(path):
                nonlocal replaced
                target = original_readlink(path)
                if Path(path) == alias and not replaced:
                    replaced = True
                    alias.unlink()
                    alias.symlink_to(second.name)
                return target

            with mock.patch.object(
                    server.os, "readlink", side_effect=replace_after_readlink):
                with self.assertRaisesRegex(
                        RuntimeError, "changed while inspected"):
                    server._receipt_authorized_artifact_evidence(
                        "bench", build_dir, debug=False)

    def test_artifact_authority_seals_alias_terminal_content(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            self._write_receipted_bench_command(build_dir)
            library_dir = (
                build_dir / "vcpkg_installed/test-triplet/lib")
            library_dir.mkdir(parents=True)
            terminal = library_dir / "libcontent.1.dylib"
            terminal.write_bytes(b"content A\n")
            alias = library_dir / "libcontent.dylib"
            alias.symlink_to(terminal.name)
            before = server._receipt_authorized_artifact_evidence(
                "bench", build_dir, debug=False)
            terminal.write_bytes(b"content B\n")
            after = server._receipt_authorized_artifact_evidence(
                "bench", build_dir, debug=False)

        before_by_path = {
            item["relative_path"]: item for item in before}
        after_by_path = {
            item["relative_path"]: item for item in after}
        alias_path = str(alias.relative_to(build_dir))
        terminal_path = str(terminal.relative_to(build_dir))
        self.assertNotEqual(
            before_by_path[alias_path]["target_sha256"],
            after_by_path[alias_path]["target_sha256"],
        )
        self.assertNotEqual(
            before_by_path[terminal_path]["sha256"],
            after_by_path[terminal_path]["sha256"],
        )

    def test_artifact_authority_rejects_unsafe_vcpkg_library_aliases(self):
        cases = {
            "absolute": "/tmp/liboutside.dylib",
            "parent": "../liboutside.dylib",
            "nested": "nested/libtarget.dylib",
            "broken": "libmissing.dylib",
            "non-library": "payload.txt",
            "non-regular": "libdirectory.dylib",
        }
        for label, target in cases.items():
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                build_dir = Path(temporary).resolve()
                self._write_receipted_bench_command(build_dir)
                library_dir = (
                    build_dir / "vcpkg_installed/test-triplet/lib")
                library_dir.mkdir(parents=True)
                if label == "nested":
                    (library_dir / "nested").mkdir()
                    (library_dir / target).write_bytes(b"nested library\n")
                elif label == "non-library":
                    (library_dir / target).write_bytes(b"not a library\n")
                elif label == "non-regular":
                    (library_dir / target).mkdir()
                alias = library_dir / "libunsafe.dylib"
                alias.symlink_to(target)
                with self.assertRaises(RuntimeError):
                    server._receipt_authorized_artifact_evidence(
                        "bench", build_dir, debug=False)

        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            self._write_receipted_bench_command(build_dir)
            library_dir = (
                build_dir / "vcpkg_installed/test-triplet/lib")
            library_dir.mkdir(parents=True)
            first = library_dir / "libcycle.dylib"
            second = library_dir / "libcycle.1.dylib"
            first.symlink_to(second.name)
            second.symlink_to(first.name)
            with self.assertRaisesRegex(RuntimeError, "cycle"):
                server._receipt_authorized_artifact_evidence(
                    "bench", build_dir, debug=False)

    def test_artifact_authority_rejects_symlinked_vcpkg_library_parents(self):
        parent_paths = (
            Path("vcpkg_installed"),
            Path("vcpkg_installed/test-triplet"),
            Path("vcpkg_installed/test-triplet/lib"),
            Path("vcpkg_installed/test-triplet/debug"),
            Path("vcpkg_installed/test-triplet/debug/lib"),
        )
        for relative_parent in parent_paths:
            with self.subTest(relative_parent=relative_parent), \
                    tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                build_dir = root / "build"
                build_dir.mkdir()
                self._write_receipted_bench_command(build_dir)
                external = root / "external"
                external.mkdir()
                symlink = build_dir / relative_parent
                symlink.parent.mkdir(parents=True, exist_ok=True)
                symlink.symlink_to(external, target_is_directory=True)
                with self.assertRaisesRegex(
                        RuntimeError, "parent must be a real non-symlink"):
                    server._receipt_authorized_artifact_evidence(
                        "bench", build_dir, debug=False)

    def test_artifact_authority_keeps_non_vcpkg_symlinks_strict(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            target = build_dir / "real-bench"
            target.write_bytes(b"test executable\n")
            target.chmod(0o755)
            bin_dir = build_dir / "bioseq"
            bin_dir.mkdir()
            (bin_dir / "bioseq_bench").symlink_to(target)
            with self.assertRaisesRegex(RuntimeError, "regular non-symlink"):
                server._receipt_authorized_artifact_evidence(
                    "bench", build_dir, debug=False)

        for authority_name in ("cmake_install.cmake", "install.sql"):
            with self.subTest(authority_name=authority_name), \
                    tempfile.TemporaryDirectory() as temporary:
                build_dir = Path(temporary).resolve()
                self._write_receipted_bench_command(build_dir)
                payload = build_dir / "payload"
                payload.write_bytes(b"install payload\n")
                (build_dir / authority_name).symlink_to(payload)
                with self.assertRaisesRegex(RuntimeError, "must not be a symlink"):
                    server._receipt_authorized_artifact_evidence(
                        "bench", build_dir, debug=False)

        for authority_name, debug in (
            ("install.so.sql", False),
            ("install.so.control", False),
            ("listing.so.s", True),
            ("bitcode.so.bc", True),
        ):
            with self.subTest(overlapping_name=authority_name), \
                    tempfile.TemporaryDirectory() as temporary:
                build_dir = Path(temporary).resolve()
                self._write_receipted_bench_command(build_dir)
                library_dir = (
                    build_dir / "vcpkg_installed/test-triplet/lib")
                library_dir.mkdir(parents=True)
                payload = library_dir / "libpayload.so.1"
                payload.write_bytes(b"library payload\n")
                (library_dir / authority_name).symlink_to(payload.name)
                with self.assertRaisesRegex(RuntimeError, "must not be a symlink"):
                    server._receipt_authorized_artifact_evidence(
                        "bench", build_dir, debug=debug)

        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            self._write_receipted_bench_command(build_dir)
            dependency_dir = build_dir / "linked"
            dependency_dir.mkdir()
            target = dependency_dir / "liboutside.1.dylib"
            target.write_bytes(b"library\n")
            (dependency_dir / "liboutside.dylib").symlink_to(target.name)
            with self.assertRaisesRegex(RuntimeError, "must not be a symlink"):
                server._receipt_authorized_artifact_evidence(
                    "bench", build_dir, debug=False)

    def test_artifact_authority_keeps_extension_dsos_non_symlink_strict(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            bin_dir = build_dir / "bioseq"
            bin_dir.mkdir()
            for command in server.MODES["unit+integration"].commands:
                path = bin_dir / command[0]
                path.write_bytes(b"test executable\n")
                path.chmod(0o755)
            suffix = server._native_shared_library_suffix(
                server.host_platform.system())
            (build_dir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")
            real_core = bin_dir / f"real-bioseq{suffix}"
            real_core.write_bytes(b"core dso\n")
            (bin_dir / f"bioseq{suffix}").symlink_to(real_core.name)
            cache = (
                build_dir
                / "bioseq_caching/artifacts/cache_production/Release"
                / f"bioseq_cache{suffix}"
            )
            cache.parent.mkdir(parents=True)
            cache.write_bytes(b"cache dso\n")

            with self.assertRaisesRegex(RuntimeError, "has no native"):
                server._receipt_authorized_artifact_evidence(
                    "unit+integration", build_dir, debug=False)

    def test_artifact_authority_rejects_dependency_dso_name_substitution(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            bin_dir = build_dir / "bioseq"
            bin_dir.mkdir()
            for command in server.MODES["unit+integration"].commands:
                path = bin_dir / command[0]
                path.write_bytes(b"test executable\n")
                path.chmod(0o755)
            suffix = server._native_shared_library_suffix(
                server.host_platform.system())
            (build_dir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")
            dependency_dir = build_dir / "vcpkg_installed/test-triplet/lib"
            dependency_dir.mkdir(parents=True)
            (dependency_dir / f"bioseq{suffix}").write_bytes(
                b"unrelated core basename\n")
            (dependency_dir / f"bioseq_cache{suffix}").write_bytes(
                b"unrelated cache basename\n")
            stale_cache = (
                build_dir
                / "bioseq_caching/artifacts/cache_production/OldConfiguration"
                / f"bioseq_cache{suffix}"
            )
            stale_cache.parent.mkdir(parents=True)
            stale_cache.write_bytes(b"stale cache dso\n")

            with self.assertRaisesRegex(RuntimeError, "has no native"):
                server._receipt_authorized_artifact_evidence(
                    "unit+integration", build_dir, debug=False)
            real_cache = (
                build_dir
                / "bioseq_caching/artifacts/cache_production/Release"
                / f"bioseq_cache{suffix}"
            )
            real_cache.parent.mkdir(parents=True)
            real_cache.write_bytes(b"real cache dso\n")
            with self.assertRaisesRegex(RuntimeError, "has no native"):
                server._receipt_authorized_artifact_evidence(
                    "unit+integration", build_dir, debug=False)

    def test_artifact_authority_uses_core_target_path_across_modes(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            suffix = server._native_shared_library_suffix(
                server.host_platform.system())
            core = build_dir / "bioseq" / f"bioseq{suffix}"
            core.parent.mkdir()
            core.write_bytes(b"website-owned core dso\n")

            evidence = server._receipt_authorized_artifact_evidence(
                "website", build_dir, debug=False)

        relative_paths = {item["relative_path"] for item in evidence}
        self.assertIn(str(core.relative_to(build_dir)), relative_paths)

    def test_runtime_artifact_must_be_executable(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            bin_dir = build_dir / "bioseq"
            bin_dir.mkdir()
            (bin_dir / "bioseq_bench").write_text(
                "not executable\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "not executable"):
                server._runtime_artifact_evidence("bench", build_dir)

    def test_artifact_counts_report_actual_compiled_and_linked_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary).resolve()
            generated = build_dir / "bioseq/generated/highway/target"
            generated.mkdir(parents=True)
            (generated / "owner.cppm").write_text(
                "export module owner;\n", encoding="utf-8")
            objects = build_dir / "objects"
            objects.mkdir()
            (objects / "one.o").write_bytes(b"one")
            (objects / "two.o").write_bytes(b"two")
            bin_dir = build_dir / "bioseq"
            bin_dir.mkdir(exist_ok=True)
            executable = bin_dir / "bioseq_bench"
            executable.write_bytes(b"binary")
            executable.chmod(0o755)
            (bin_dir / f"provider{server._native_shared_library_suffix(server.host_platform.system())}").write_bytes(b"dso")
            (build_dir / "compile_commands.json").write_text(
                json.dumps([{}, {}, {}]), encoding="utf-8")
            (build_dir / "cmake_install.cmake").write_text(
                "# plan\n", encoding="utf-8")
            (objects / "ignored.o.link").symlink_to(objects / "one.o")

            counts = server._build_artifact_counts(build_dir, "bench")

        self.assertEqual(counts["compile_commands"], 3)
        self.assertEqual(counts["generated_highway_sources"], 1)
        self.assertEqual(counts["compiled_objects"], 2)
        self.assertEqual(counts["linked_artifacts"], 2)
        self.assertEqual(counts["runtime_artifacts"], 1)
        self.assertEqual(counts["install_scripts"], 1)

    def test_baseline_ref_rejects_arbitrary_commit_before_git(self):
        with mock.patch.object(server, "_git_commit") as git_commit:
            with self.assertRaisesRegex(RuntimeError, "exact local ref"):
                server._create_baseline_worktree("a" * 40, "run")
        git_commit.assert_not_called()

    def test_special_postgres_worker_does_not_promise_ordinary_receipt(self):
        self.assertFalse(server._build_receipt_required(
            server.POSTGRES_ERROR_SAFETY_MODE, True))
        self.assertTrue(server._build_receipt_required("bench", True))

    def test_io_inspect_target_is_owned_by_io_mode(self):
        self.assertEqual(
            server.INSPECT_TARGETS["bioseq_io_bench"].mode,
            "bench-io",
        )

    def test_status_surfaces_artifact_counts(self):
        entry = {
            "status": "completed",
            "started_at": 1.0,
            "completed_at": 2.0,
            "build_only": True,
            "debug": False,
            "preset": "bench-opt",
            "log_file": "/tmp/test.log",
            "artifact_counts": {
                "compiled_objects": 2,
                "linked_artifacts": 1,
            },
        }
        with mock.patch.object(
            server, "_load_state", return_value={"bench": entry},
        ), mock.patch.object(server, "_refresh", return_value=entry):
            message = server.tool_status("bench")
        self.assertIn("Artifact counts", message)
        self.assertIn('"compiled_objects": 2', message)

        failed = dict(entry)
        failed.update({
            "status": "failed",
            "notes": "guard cleanup failed",
            "build_timeout_sec": 3300,
            "timeout_guard_armed": True,
            "build_soft_deadline": 3240.0,
            "build_hard_deadline": 3300.0,
            "active_edge": "[2/10] clang++ owner.cppm",
            "termination_evidence": {"cleanup_verified": False},
            "cleanup_unverified": True,
        })
        with mock.patch.object(
            server, "_load_state", return_value={"bench": failed},
        ), mock.patch.object(server, "_refresh", return_value=failed):
            failed_message = server.tool_status("bench")
        self.assertIn("[2/10] clang++ owner.cppm", failed_message)
        self.assertIn("Termination evidence", failed_message)
        self.assertIn("cleanup_unverified=True", failed_message)

    def test_run_only_helper_never_invokes_build(self):
        mode = "bench-fast"
        receipt = {"build_receipt_id": "br_test", "debug": False}
        entry = {"started_at": time.time()}
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary)
            log_path = build_dir / "run.log"
            with log_path.open("w", encoding="utf-8") as log_handle, \
                    mock.patch.object(
                        server,
                        "_build_worker_descendant_environment",
                        return_value={},
                    ), mock.patch.object(
                        server, "_ensure_local_toolchain_environment",
                    ), mock.patch.object(
                        server,
                        "_run_mode_commands",
                        return_value=(None, [], [], []),
                    ) as run_commands, mock.patch.object(
                        server, "_stream_subprocess",
                    ) as subprocess_call, mock.patch.object(
                        server, "_update_entry",
                    ), mock.patch.object(
                        server,
                        "_validate_build_receipt",
                        return_value=(receipt, None),
                    ), mock.patch.object(
                        server,
                        "_seal_run_receipt",
                        return_value={"build_receipt_id": "rr_test"},
                    ):
                result = server._run_only_from_build_receipt(
                    mode=mode,
                    entry=entry,
                    receipt=receipt,
                    active_build_dir=build_dir,
                    active_bin_dir=build_dir / "bioseq",
                    active_preset="bench-opt",
                    bench_flags=[],
                    test_flags=[],
                    bench_e2e_config=None,
                    log_handle=log_handle,
                    deadline=time.time() + 60,
                )
        self.assertEqual(result, 0)
        run_commands.assert_called_once()
        subprocess_call.assert_not_called()

    def test_inspect_uses_exact_debug_receipt_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "debug-tree"
            build_dir.mkdir()
            receipt = {
                "build_receipt_id": "br_debug",
                "debug": True,
                "preset": "test-opt-debug",
                "build_dir": str(build_dir),
                "source_graph_fingerprint": "source-digest",
                "baseline_ref": None,
            }
            captured = {}

            def inspect_impl(**arguments):
                captured.update(arguments)
                return {"summary": {}}

            with mock.patch.object(
                server,
                "_validate_build_receipt",
                return_value=(receipt, None),
            ) as validate, mock.patch.object(
                server,
                "_inspect_artifact_candidates",
                return_value=("bioseq_aligner_tests_exec",),
            ), mock.patch.object(
                server, "_run_inspect_impl", side_effect=inspect_impl,
            ):
                result = json.loads(server.tool_inspect({
                    "function_name": "ksw",
                    "target": "bioseq_aligner_tests_exec",
                    "reuse_build_receipt": "br_debug",
                }))
        validate.assert_called_once_with(
            "br_debug",
            expected_mode="unit+integration",
            expected_debug=None,
        )
        self.assertEqual(captured["build_dir"], build_dir)
        self.assertEqual(
            result["build_metadata"]["build_receipt_id"], "br_debug")


class GuardedProcessTrackerTests(unittest.TestCase):
    @staticmethod
    def _identity(pid, ppid, pgid, session, started, command):
        return server._ProcessIdentity(
            pid, ppid, pgid, session, started, command)

    def test_marker_finds_already_reparented_new_session(self):
        root = self._identity(10, 1, 10, 10, "root-start", "cmake")
        escaped = self._identity(
            11, 1, 11, 11, "child-start", "clang")
        tracker = server._GuardedProcessTracker(root, "marker")
        with mock.patch.object(
            server,
            "_guarded_process_snapshot",
            return_value=({10: root, 11: escaped}, {11}),
        ):
            live = tracker.refresh()
        self.assertEqual(
            {process.pid for process in live.values()}, {10, 11})

    def test_host_snapshot_finds_process_by_isolated_build_cwd(self):
        if not (
            server.sys.platform == "darwin"
            or server.sys.platform.startswith("linux")
        ):
            self.skipTest("cwd ownership probe is implemented on macOS/Linux")
        with tempfile.TemporaryDirectory() as temporary:
            ownership_root = Path(temporary).resolve()
            marker = "host-cwd-probe-marker"
            environment = dict(server.os.environ)
            environment[server.BUILD_GUARD_ENV_KEY] = marker
            process = subprocess.Popen(
                [server.sys.executable, "-c", "import time; time.sleep(2)"],
                cwd=ownership_root,
                env=environment,
                start_new_session=True,
            )
            try:
                snapshot, owned = server._guarded_process_snapshot(
                    marker, ownership_root)
                self.assertIn(process.pid, snapshot)
                self.assertIn(process.pid, owned)
                self.assertEqual(snapshot[process.pid].session, process.pid)
            finally:
                server._terminate_unarmed_popen_group(
                    process,
                    guard_marker=marker,
                    ownership_root=ownership_root,
                )

    def test_retains_child_after_reparent_and_marker_clear(self):
        root = self._identity(20, 1, 20, 20, "root-start", "cmake")
        child = self._identity(21, 20, 21, 21, "child-start", "clang")
        reparented = self._identity(
            21, 1, 31, 31, "child-start", "clang")
        tracker = server._GuardedProcessTracker(root, "marker")
        with mock.patch.object(
            server,
            "_guarded_process_snapshot",
            side_effect=[
                ({20: root, 21: child}, {20, 21}),
                ({20: root, 21: reparented}, set()),
            ],
        ):
            tracker.refresh()
            live = tracker.refresh()
        observed = next(
            process for process in live.values() if process.pid == 21)
        self.assertEqual((observed.ppid, observed.pgid, observed.session),
                         (1, 31, 31))

    def test_pid_reuse_is_not_retained_without_marker_or_ancestry(self):
        root = self._identity(30, 1, 30, 30, "root-start", "cmake")
        child = self._identity(31, 30, 31, 31, "old-start", "clang")
        reused = self._identity(31, 1, 31, 31, "new-start", "unrelated")
        tracker = server._GuardedProcessTracker(root, "marker")
        with mock.patch.object(
            server,
            "_guarded_process_snapshot",
            side_effect=[
                ({30: root, 31: child}, {30, 31}),
                ({30: root, 31: reused}, set()),
            ],
        ):
            tracker.refresh()
            live = tracker.refresh()
        self.assertEqual({process.pid for process in live.values()}, {30})

    def test_termination_discovers_group_created_after_term_snapshot(self):
        root = self._identity(40, 1, 40, 40, "root-start", "cmake")
        child = self._identity(41, 1, 41, 41, "child-start", "clang")
        tracker = server._GuardedProcessTracker(root, "marker")
        snapshots = iter([
            ({40: root}, {40}),
            ({40: root, 41: child}, {40, 41}),
            ({40: root, 41: child}, {40, 41}),
            ({40: root, 41: child}, {40, 41}),
            ({}, set()),
        ])
        now = [0.0]
        signals = []
        direct_signals = []

        def sleep(duration):
            now[0] += max(duration, 0.1)

        with mock.patch.object(
            server,
            "_guarded_process_snapshot",
            side_effect=lambda marker, ownership_root=None: next(snapshots),
        ), mock.patch.object(
            server,
            "_guarded_worker_validation",
            return_value=({}, None),
        ), mock.patch.object(
            server.os,
            "killpg",
            side_effect=lambda pgid, sig: signals.append((pgid, sig)),
        ), mock.patch.object(
            server.os,
            "kill",
            side_effect=lambda pid, sig: direct_signals.append((pid, sig)),
        ), mock.patch.object(
            server,
            "BUILD_TIMEOUT_TERM_GRACE_SEC",
            0.2,
        ):
            evidence = server._terminate_guarded_process_tree(
                tracker,
                hard_kill_deadline=2.0,
                state_mode="unit+integration",
                clock=lambda: now[0],
                sleeper=sleep,
            )
        self.assertIn((41, server.signal.SIGKILL), signals)
        self.assertIn((41, server.signal.SIGKILL), direct_signals)
        self.assertEqual(evidence["survivor_pids"], [])

    def test_persistent_snapshot_failure_is_not_reported_as_cleanup(self):
        root = self._identity(50, 1, 50, 50, "root-start", "cmake")
        tracker = server._GuardedProcessTracker(root, "marker")
        tracker.last_live = {(root.pid, root.started): root}
        now = [0.0]

        def sleep(duration):
            now[0] += max(duration, 0.1)

        with mock.patch.object(
            server,
            "_guarded_process_snapshot",
            side_effect=RuntimeError("ps unavailable"),
        ), mock.patch.object(
            server,
            "_guarded_worker_validation",
            return_value=({}, None),
        ), mock.patch.object(server.os, "killpg"), mock.patch.object(
            server.os, "kill",
        ), mock.patch.object(
            server, "BUILD_TIMEOUT_TERM_GRACE_SEC", 0.1,
        ):
            with self.assertRaises(
                server._GuardedTerminationError,
            ) as raised:
                server._terminate_guarded_process_tree(
                    tracker,
                    hard_kill_deadline=0.5,
                    state_mode="unit+integration",
                    clock=lambda: now[0],
                    sleeper=sleep,
                )
        self.assertFalse(raised.exception.evidence["cleanup_verified"])
        self.assertEqual(
            raised.exception.evidence["survivor_pids"], [50])

    def test_expired_deadline_does_not_spawn(self):
        with tempfile.NamedTemporaryFile(mode="w+") as log_handle, \
                mock.patch.object(server.subprocess, "Popen") as popen:
            with self.assertRaises(TimeoutError):
                server._stream_subprocess(
                    ["never-run"],
                    log_handle,
                    deadline=10.0,
                    hard_kill_deadline=11.0,
                    require_verified_termination=True,
                    deadline_clock=lambda: 10.0,
                )
        popen.assert_not_called()

    def test_guard_preflight_cannot_launch_after_deadline(self):
        now = [9.0]

        def preflight(marker, ownership_root=None):
            now[0] = 10.0
            return {}, set()

        with tempfile.NamedTemporaryFile(mode="w+") as log_handle, \
                mock.patch.object(
                    server, "_guarded_process_snapshot", side_effect=preflight,
                ), mock.patch.object(
                    server,
                    "_load_state",
                    return_value={
                        "unit+integration": {"build_dir": "/tmp/guarded"},
                    },
                ), mock.patch.object(server.subprocess, "Popen") as popen:
            with self.assertRaises(TimeoutError):
                server._stream_subprocess(
                    ["never-run"],
                    log_handle,
                    deadline=10.0,
                    hard_kill_deadline=11.0,
                    require_verified_termination=True,
                    state_mode="unit+integration",
                    deadline_clock=lambda: now[0],
                )
        popen.assert_not_called()

    def test_parent_accepts_only_new_session_child_topology(self):
        exact = self._identity(
            70, 7, 70, 70, "worker-start", "python server.py")
        wrong_parent = self._identity(
            70, 8, 70, 70, "worker-start", "python server.py")
        wrong_group = self._identity(
            70, 7, 71, 70, "worker-start", "python server.py")
        self.assertTrue(server._valid_detached_worker_identity(
            exact, child_pid=70, parent_pid=7))
        self.assertFalse(server._valid_detached_worker_identity(
            wrong_parent, child_pid=70, parent_pid=7))
        self.assertFalse(server._valid_detached_worker_identity(
            wrong_group, child_pid=70, parent_pid=7))

    def test_guarded_worker_arm_requires_exact_pid_start_and_session(self):
        identity = self._identity(
            77, 1, 77, 77, "worker-start", "python server.py")
        entry = {
            "status": "running",
            "step": "armed",
            "pid": 77,
            "worker_process_identity": identity.__dict__,
        }
        with mock.patch.object(
            server, "_load_state", return_value={"unit+integration": entry},
        ), mock.patch.object(server.os, "getpid", return_value=77), \
                mock.patch.object(
                    server, "_capture_process_identity", return_value=identity):
            observed = server._await_guarded_worker_arm("unit+integration")
        self.assertEqual(observed["pid"], 77)

    def test_parent_refuses_to_arm_when_identity_capture_fails(self):
        fake_process = mock.Mock(pid=4321, returncode=1)
        fake_process.wait.return_value = 1
        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(
            server, "STATE_FILE", Path(temporary) / "state.json",
        ), mock.patch.object(
            server, "LOCK_FILE", Path(temporary) / "state.lock",
        ), mock.patch.object(
            server, "_refuse_root_build_identity", return_value=None,
        ), mock.patch.object(
            server, "_ensure_build_worker_timesharing_policy",
        ), mock.patch.object(
            server, "_source_graph_fingerprint", return_value="fingerprint",
        ), mock.patch.object(
            server, "_git_commit", return_value="a" * 40,
        ), mock.patch.object(
            server, "_configured_local_build_rt_priority", return_value=None,
        ), mock.patch.object(
            server, "_preflight_local_clang_rt_launcher",
        ), mock.patch.object(
            server, "_local_clang_rt_is_enabled", return_value=False,
        ), mock.patch.object(
            server.subprocess, "Popen", return_value=fake_process,
        ), mock.patch.object(
            server, "_capture_process_identity", return_value=None,
        ), mock.patch.object(
            server,
            "_terminate_unarmed_popen_group",
            return_value={"cleanup_verified": True},
        ) as terminate_unarmed:
            message = server.tool_start_build(
                "unit+integration",
                build_only=True,
                fresh=True,
                build_timeout_sec=3300,
            )
            state = server._load_state()["unit+integration"]
        self.assertIn("identity error", message.lower())
        self.assertEqual(state["step"], "worker_identity_failed")
        terminate_unarmed.assert_called_once_with(fake_process)

    def test_initial_tracker_failure_persists_cleanup_block(self):
        root = self._identity(
            90, 1, 90, 90, "root-start", "cmake --build")
        fake_process = mock.Mock(pid=90)
        fake_process.stdout = mock.Mock()
        cleanup_error = server._GuardedTerminationError(
            "cleanup unverified",
            {
                "survivor_identities": [root.__dict__],
                "cleanup_verified": False,
            },
        )
        with tempfile.NamedTemporaryFile(mode="w+") as log_handle, \
                mock.patch.object(
                    server,
                    "_load_state",
                    return_value={
                        "unit+integration": {"build_dir": "/tmp/guarded"},
                    },
                ), mock.patch.object(
                    server, "_guarded_process_snapshot", return_value=({}, set()),
                ), mock.patch.object(
                    server.subprocess, "Popen", return_value=fake_process,
                ), mock.patch.object(
                    server, "_capture_process_identity", return_value=root,
                ), mock.patch.object(
                    server._GuardedProcessTracker,
                    "refresh",
                    side_effect=RuntimeError("snapshot failed"),
                ), mock.patch.object(
                    server,
                    "_terminate_guarded_process_tree",
                    side_effect=cleanup_error,
                ), mock.patch.object(server, "_update_entry") as update:
            with self.assertRaises(server._GuardedTerminationError):
                server._stream_subprocess(
                    ["cmake", "--build", "/tmp/guarded"],
                    log_handle,
                    deadline=10.0,
                    hard_kill_deadline=11.0,
                    state_mode="unit+integration",
                    require_verified_termination=True,
                    deadline_clock=lambda: 0.0,
                )
        self.assertTrue(any(
            call.kwargs.get("cleanup_unverified") is True
            for call in update.call_args_list
        ))

    def test_dead_guarded_worker_runs_orphan_recovery(self):
        worker = self._identity(
            101, 1, 101, 101, "worker-start", "python server.py")
        entry = {
            "status": "running",
            "pid": 101,
            "worker_process_identity": worker.__dict__,
            "build_timeout_sec": 3300,
            "build_dir": "/tmp/guarded",
            "started_at": 1.0,
        }
        state = {"unit+integration": entry}
        evidence = {"cleanup_verified": False, "survivor_identities": []}
        with mock.patch.object(
            server, "_capture_process_identity", return_value=None,
        ), mock.patch.object(
            server, "_is_pid_alive", return_value=False,
        ), mock.patch.object(
            server,
            "_recover_guarded_worker_descendants",
            return_value=(evidence, True),
        ) as recover, mock.patch.object(server, "_save_state"):
            observed = server._refresh(state, "unit+integration")
        recover.assert_called_once()
        self.assertEqual(observed["status"], "aborted")
        self.assertTrue(observed["cleanup_unverified"])
        self.assertEqual(observed["termination_evidence"], evidence)

    def test_unverified_cleanup_requires_successful_empty_ownership_sweep(self):
        entry = {
            "cleanup_unverified": True,
            "build_dir": "/tmp/guarded",
            "termination_evidence": {"survivor_identities": []},
        }
        child = self._identity(
            111, 1, 111, 111, "child-start", "clang")
        with mock.patch.object(
            server,
            "_guarded_process_snapshot",
            return_value=({111: child}, {111}),
        ):
            blocked = server._unverified_guard_cleanup({"bench": entry})
        self.assertIn("remain live", blocked)
        with mock.patch.object(
            server, "_guarded_process_snapshot", return_value=({}, set()),
        ):
            cleared = server._unverified_guard_cleanup({"bench": entry})
        self.assertIsNone(cleared)


if __name__ == "__main__":
    unittest.main()
