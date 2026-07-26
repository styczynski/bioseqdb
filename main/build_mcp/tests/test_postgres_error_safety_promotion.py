from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest


BUILD_MCP_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BUILD_MCP_DIR))

import postgres_error_safety_promotion as promotion


def _seal(value: dict[str, object]) -> dict[str, object]:
    result = dict(value)
    result["content_sha256"] = hashlib.sha256(json.dumps(
        result,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")).hexdigest()
    return result


class PostgresErrorSafetyPromotionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.workspace = Path(self.temporary.name).resolve()
        self.candidate = self.workspace / "candidate"
        self.candidate.mkdir()
        self.bodies = {
            "production-ledger.json": b'{"profile":"production"}\n',
            "runtime-matrix.json": b'{"cases":[]}\n',
            "test-indexes-ledger.json":
                b'{"profile":"test_indexes"}\n',
        }
        for name, body in self.bodies.items():
            (self.candidate / name).write_bytes(body)
        stamp = {
            "schema_version": 1,
            "production_ledger_sha256": self._sha(
                self.bodies["production-ledger.json"]),
            "runtime_matrix_sha256": self._sha(
                self.bodies["runtime-matrix.json"]),
            "test_indexes_ledger_sha256": self._sha(
                self.bodies["test-indexes-ledger.json"]),
        }
        (self.candidate / "candidate.stamp").write_text(
            json.dumps(stamp, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        self.waiver_path = self.workspace / "review-waiver.json"
        self.waiver = _seal({
            "schema_version": 1,
            "artifact_kind": promotion.REVIEW_WAIVER_KIND,
            "approval_reference":
                "User instruction dated 2026-07-26: skip any reviews.",
            "waiver_reason":
                "User explicitly waived independent review for DOC0388.",
            "independent_review_performed": False,
        })
        self.waiver_path.write_text(
            json.dumps(self.waiver, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        self.snapshot = promotion.load_review_waiver(
            str(self.waiver_path),
            self.workspace,
        )

    @staticmethod
    def _sha(body: bytes) -> str:
        return hashlib.sha256(body).hexdigest()

    def test_waiver_snapshot_is_exact_and_cannot_claim_review(self) -> None:
        self.assertEqual(
            set(self.snapshot),
            promotion.REVIEW_WAIVER_SNAPSHOT_FIELDS,
        )
        self.assertIs(
            self.snapshot["independent_review_performed"],
            False,
        )
        self.assertEqual(
            self.snapshot["waiver_request_file_sha256"],
            self._sha(self.waiver_path.read_bytes()),
        )

        invalid = dict(self.waiver)
        invalid["independent_review_performed"] = True
        unsealed = dict(invalid)
        del unsealed["content_sha256"]
        invalid = _seal(unsealed)
        self.waiver_path.write_text(
            json.dumps(invalid),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
                promotion.PromotionError,
                "must set independent_review_performed to false"):
            promotion.load_review_waiver(
                str(self.waiver_path),
                self.workspace,
            )

    def test_waiver_rejects_symlinks_hardlinks_and_outside_workspace(
            self) -> None:
        symlink = self.workspace / "waiver-symlink.json"
        symlink.symlink_to(self.waiver_path)
        with self.assertRaisesRegex(
                promotion.PromotionError,
                "must not be a symlink"):
            promotion.load_review_waiver(str(symlink), self.workspace)

        hardlink = self.workspace / "waiver-hardlink.json"
        os.link(self.waiver_path, hardlink)
        with self.assertRaisesRegex(
                promotion.PromotionError,
                "regular single-link"):
            promotion.load_review_waiver(
                str(self.waiver_path),
                self.workspace,
            )

        with tempfile.NamedTemporaryFile(suffix=".json") as outside:
            outside.write(b"{}")
            outside.flush()
            with self.assertRaisesRegex(
                    promotion.PromotionError,
                    "inside the workspace"):
                promotion.load_review_waiver(
                    outside.name,
                    self.workspace,
                )

    def test_promotion_is_atomic_independent_and_idempotent(self) -> None:
        receipt_path, receipt = promotion.promote_reviewed_evidence(
            self.candidate,
            self.snapshot,
            self.workspace,
        )
        receipt_body = receipt_path.read_bytes()
        reviewed = self.candidate / "reviewed"
        self.assertEqual(
            stat.S_IMODE(reviewed.lstat().st_mode),
            0o700,
        )
        self.assertIs(receipt["independent_review_performed"], False)
        self.assertNotIn("reviewer", receipt)
        unsealed = dict(receipt)
        seal = unsealed.pop("content_sha256")
        self.assertEqual(
            seal,
            promotion._canonical_json_sha256(unsealed),
        )
        self.assertEqual(
            [record["name"] for record in receipt["artifacts"]],
            list(promotion.PROMOTED_ARTIFACT_NAMES),
        )
        for name, candidate_body in self.bodies.items():
            candidate_path = self.candidate / name
            reviewed_path = reviewed / name
            self.assertEqual(reviewed_path.read_bytes(), candidate_body)
            reviewed_metadata = reviewed_path.lstat()
            self.assertEqual(
                stat.S_IMODE(reviewed_metadata.st_mode),
                0o600,
            )
            self.assertEqual(reviewed_metadata.st_nlink, 1)
            self.assertNotEqual(
                candidate_path.lstat().st_ino,
                reviewed_metadata.st_ino,
            )

        repeated_path, repeated_receipt = (
            promotion.promote_reviewed_evidence(
                self.candidate,
                self.snapshot,
                self.workspace,
            )
        )
        self.assertEqual(repeated_path, receipt_path)
        self.assertEqual(repeated_receipt, receipt)
        self.assertEqual(receipt_path.read_bytes(), receipt_body)

    def test_mismatched_reviewed_file_or_receipt_fails_closed(self) -> None:
        receipt_path, _ = promotion.promote_reviewed_evidence(
            self.candidate,
            self.snapshot,
            self.workspace,
        )
        reviewed = self.candidate / "reviewed"
        ledger = reviewed / "production-ledger.json"
        ledger.write_bytes(b"drift\n")
        os.chmod(ledger, 0o600)
        with self.assertRaisesRegex(
                promotion.PromotionError,
                "differs from candidate"):
            promotion.promote_reviewed_evidence(
                self.candidate,
                self.snapshot,
                self.workspace,
            )

        ledger.write_bytes(self.bodies["production-ledger.json"])
        os.chmod(ledger, 0o600)
        receipt_path.write_bytes(b"{}\n")
        os.chmod(receipt_path, 0o600)
        with self.assertRaisesRegex(
                promotion.PromotionError,
                "does not bind this exact waiver"):
            promotion.promote_reviewed_evidence(
                self.candidate,
                self.snapshot,
                self.workspace,
            )

    def test_candidate_stamp_and_link_identity_fail_closed(self) -> None:
        stamp = self.candidate / "candidate.stamp"
        stamp.write_text('{"schema_version":1}\n', encoding="utf-8")
        with self.assertRaisesRegex(
                promotion.PromotionError,
                "candidate stamp does not bind"):
            promotion.promote_reviewed_evidence(
                self.candidate,
                self.snapshot,
                self.workspace,
            )

        stamp.write_text(
            json.dumps({
                "schema_version": 1,
                "production_ledger_sha256": self._sha(
                    self.bodies["production-ledger.json"]),
                "runtime_matrix_sha256": self._sha(
                    self.bodies["runtime-matrix.json"]),
                "test_indexes_ledger_sha256": self._sha(
                    self.bodies["test-indexes-ledger.json"]),
            }),
            encoding="utf-8",
        )
        reviewed = self.candidate / "reviewed"
        reviewed.mkdir(mode=0o700)
        os.link(
            self.candidate / "production-ledger.json",
            reviewed / "production-ledger.json",
        )
        with self.assertRaisesRegex(
                promotion.PromotionError,
                "regular single-link"):
            promotion.promote_reviewed_evidence(
                self.candidate,
                self.snapshot,
                self.workspace,
            )


if __name__ == "__main__":
    unittest.main()
