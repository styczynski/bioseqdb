"""Focused isolated tests for recorded-boundary terminal cleanup recovery."""
import copy
from contextlib import ExitStack
import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest import mock

from test_server import (CanonicalClangE2EReceiptPortabilityTests,
                         Q1024ServiceLifecycleTests, process_state, build_worker,
                         postgres_runtime)


class TerminalBoundaryRecoveryTests(unittest.TestCase):
    def test_global_zero_without_apple_cache_requires_exact_recorded_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            postgres = root / "postgres"
            postgres.write_bytes(b"fixture")
            selected = postgres_runtime._seal_postgresql_file_identity(
                str(postgres), "test server")
            base = Q1024ServiceLifecycleTests()._legacy_entry(root)
            boundary = {
                "runtime_identity_verified": True,
                "postgres_executable_identity": selected.__dict__,
            }
            cases = [(boundary, True), (None, False), ({}, False)]
            for key, value in (("device", -1), ("inode", -1), ("size", -1),
                               ("sha256", "0" * 64)):
                changed = copy.deepcopy(boundary)
                changed["postgres_executable_identity"][key] = value
                cases.append((changed, False))
            for value in ({}, {"extra": 1}, "malformed"):
                changed = copy.deepcopy(boundary)
                changed["postgres_executable_identity"] = value
                cases.append((changed, False))
            unverified = copy.deepcopy(boundary)
            unverified["runtime_identity_verified"] = False
            cases.append((unverified, False))
            for candidate, expected in cases:
                entry = copy.deepcopy(base)
                entry["postgres_service_boundary"] = candidate
                with self.subTest(boundary=candidate), ExitStack() as stack:
                    stack.enter_context(mock.patch.object(
                        postgres_runtime, "_pg_config_from_cache", return_value="pg_config"))
                    stack.enter_context(mock.patch.object(
                        postgres_runtime, "_postgresql_config_path", return_value=str(root)))
                    stack.enter_context(mock.patch.object(
                        postgres_runtime, "_cmake_cache_value", return_value=None))
                    snapshot = stack.enter_context(mock.patch.object(
                        postgres_runtime, "_process_snapshot", return_value={}))
                    result = postgres_runtime._legacy_q1024_global_postgresql_zero_evidence(
                        entry=entry, active_build_dir=root,
                        active_preset="native-x64-clang22-linux-e2e", deadline=time.time() + 5)
                    self.assertEqual(result["cleanup_verified"], expected)
                    if not expected:
                        snapshot.assert_not_called()

    def test_changed_claim_bindings_rejected_before_global_probe(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = (Path(temporary) / "build").resolve()
            receipt = CanonicalClangE2EReceiptPortabilityTests._receipt(
                build_dir, preset="bench-e2e-opt", system="Darwin", machine="arm64",
                compiler="/opt/homebrew/opt/llvm@22/bin/clang++", compiler_id=None,
                compiler_version=None, flags="-O3 -DNDEBUG -fno-omit-frame-pointer")
            receipt.update(build_receipt_id="br_build33", content_sha256="1" * 64)
            entry = Q1024ServiceLifecycleTests()._legacy_entry(build_dir)
            for field in ("mode", "active_guard_marker", "bench_e2e_config",
                          "postgres_service_boundary"):
                with self.subTest(field=field), ExitStack() as stack:
                    claimed = copy.deepcopy(entry)
                    claimed[field] = "changed-after-initial-validation"
                    patches = {
                        "_load_state": {"return_value": {"benchmark-e2e": entry}},
                        "_algorithm_matrix_campaign_authority_status": {
                            "return_value": ({"name": "q1024",
                                              "execution_role": "q1024_singleton"}, None)},
                        "_validate_build_receipt": {"return_value": (receipt, None)},
                        "_claim_q1024_postgresql_revalidation": {
                            "return_value": ("claim", claimed)},
                        "_legacy_q1024_global_postgresql_zero_evidence": {},
                        "_finish_q1024_postgresql_revalidation": {"return_value": False},
                    }
                    mocks = {key: stack.enter_context(mock.patch.object(
                        build_worker, key, **kwargs)) for key, kwargs in patches.items()}
                    self.assertFalse(build_worker._revalidate_legacy_q1024_postgresql_cleanup_before_admission())
                    mocks["_legacy_q1024_global_postgresql_zero_evidence"].assert_not_called()
                    evidence = mocks["_finish_q1024_postgresql_revalidation"].call_args.args[3]
                    self.assertFalse(evidence["cleanup_verified"])
                    self.assertIn("state changed", evidence["error"])

    def test_worker_admits_recorded_boundary_to_historical_global_zero_proof(self):
        original = Q1024ServiceLifecycleTests._legacy_entry

        def with_boundary(fixture, build_dir):
            entry = original(fixture, build_dir)
            entry["active_guard_marker"] = "b" * 32
            entry["postgres_service_boundary"] = {
                "schema_version": 1, "mode": "benchmark-e2e",
                "run_token": entry["q1024_run_token"],
                "application_name": entry["postgres_application_name"],
                "runtime_identity_verified": True,
                "client_process_identity": copy.deepcopy(entry["active_process_identity"]),
                "guard_marker": entry["active_guard_marker"],
            }
            return entry

        with mock.patch.object(Q1024ServiceLifecycleTests, "_legacy_entry", with_boundary):
            Q1024ServiceLifecycleTests().test_terminal_revalidation_uses_historical_receipt_only_for_cleanup()

    def exercise(self, mutation=None, live_owner=False, verified=True):
        fixture = Q1024ServiceLifecycleTests()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            entry = fixture._legacy_entry(root)
            entry["active_guard_marker"] = "b" * 32
            entry["postgres_service_boundary"] = {
                "schema_version": 1,
                "mode": "benchmark-e2e",
                "run_token": entry["q1024_run_token"],
                "application_name": entry["postgres_application_name"],
                "runtime_identity_verified": True,
                "client_process_identity": copy.deepcopy(entry["active_process_identity"]),
                "guard_marker": entry["active_guard_marker"],
            }
            def file_identity(name):
                path = root / name
                path.write_bytes(b"fixture")
                metadata = path.stat()
                return {"path": str(path), "device": metadata.st_dev,
                        "inode": metadata.st_ino, "size": metadata.st_size,
                        "sha256": "f" * 64}

            metadata = root.stat()
            entry["postgres_service_boundary"].update({
                "verified_at": time.time(),
                "postmaster_process_identity": fixture._identity(
                    104, 1, 104, 104, "postgres", "postgres").__dict__,
                "data_directory": {"path": str(root), "device": metadata.st_dev,
                                   "inode": metadata.st_ino},
                "postgres_executable_identity": file_identity("postgres"),
                "bioseq_dso_identity": file_identity("bioseq.so"),
            })
            if mutation:
                mutation(entry["postgres_service_boundary"])
            before = copy.deepcopy(entry)
            state_path = root / "state.json"
            state_path.write_text(json.dumps({"benchmark-e2e": entry}))
            claimant = fixture._identity(900, 1, 900, 900, "server", "server.py")
            snapshot = {900: claimant}
            if live_owner:
                owner = process_state._ProcessIdentity(**entry["active_process_identity"])
                snapshot[owner.pid] = owner
            with mock.patch.object(process_state, "STATE_FILE", state_path), \
                    mock.patch.object(process_state, "LOCK_FILE", root / "lock"), \
                    mock.patch.object(process_state.os, "getpid", return_value=900), \
                    mock.patch.object(process_state, "_process_snapshot", return_value=snapshot):
                claim = process_state._claim_q1024_postgresql_revalidation(
                    "benchmark-e2e", entry["q1024_run_token"], deadline=time.time() + 5)
                if claim:
                    result = process_state._finish_q1024_postgresql_revalidation(
                        "benchmark-e2e", entry["q1024_run_token"], claim[0],
                        {"cleanup_verified": verified})
                    self.assertEqual(result, verified)
                after = process_state._load_state()["benchmark-e2e"]
            for field in ("status", "exit_code", "report_files", "postgres_service_boundary"):
                self.assertEqual(after[field], before[field])
            return claim, after

    def test_valid_boundary_is_preserved(self):
        claim, after = self.exercise()
        self.assertIsNotNone(claim)
        self.assertFalse(after["postgres_cleanup_unverified"])
        self.assertTrue(after["cleanup_unverified"])

    def test_mismatched_boundaries_rejected(self):
        for key, value in (
            ("mode", "unit-integration"), ("run_token", "c" * 32),
            ("application_name", "other"), ("runtime_identity_verified", False),
            ("schema_version", 2), ("client_process_identity", {}),
            ("guard_marker", "c" * 32), ("guard_marker", None),
        ):
            with self.subTest(field=key, value=value):
                claim, after = self.exercise(lambda boundary: boundary.update({key: value}))
                self.assertIsNone(claim)
                self.assertTrue(after["postgres_cleanup_unverified"])

    def test_live_owner_rejected(self):
        claim, after = self.exercise(live_owner=True)
        self.assertIsNone(claim)
        self.assertTrue(after["postgres_cleanup_unverified"])

    def test_malformed_or_missing_full_boundary_evidence_rejected(self):
        changes = [lambda b: b.update(extra="not-allowed")]
        for key in ("verified_at", "postmaster_process_identity", "data_directory",
                    "postgres_executable_identity", "bioseq_dso_identity"):
            changes.append(lambda b, key=key: b.pop(key))
            changes.append(lambda b, key=key: b.update({key: "malformed"}))
        for change in changes:
            claim, after = self.exercise(change)
            self.assertIsNone(claim)
            self.assertTrue(after["postgres_cleanup_unverified"])

    def test_failed_zero_proof_does_not_clear_uncertainty(self):
        claim, after = self.exercise(verified=False)
        self.assertIsNotNone(claim)
        self.assertTrue(after["postgres_cleanup_unverified"])


if __name__ == "__main__":
    unittest.main()
