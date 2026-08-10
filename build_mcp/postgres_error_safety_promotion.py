#!/usr/bin/env python3
"""Promote DOC0388 PostgreSQL ERROR-safety candidates under an explicit waiver.

The promotion is deliberately mechanical: it authenticates one content-sealed
user waiver, byte-copies the three generated candidate artifacts, and records
the hashes in a deterministic receipt.  It never represents the copy as an
independent review.
"""

from __future__ import annotations

import argparse
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat
import sys
from typing import Any


REVIEW_WAIVER_KIND = "doc0388_postgres_error_safety_review_waiver"
PROMOTION_RECEIPT_KIND = (
    "doc0388_postgres_error_safety_review_waiver_promotion"
)
REVIEW_WAIVER_FIELDS = frozenset({
    "schema_version",
    "artifact_kind",
    "approval_reference",
    "waiver_reason",
    "independent_review_performed",
    "content_sha256",
})
REVIEW_WAIVER_SNAPSHOT_FIELDS = frozenset({
    "schema_version",
    "artifact_kind",
    "approval_reference",
    "waiver_reason",
    "independent_review_performed",
    "waiver_request_path",
    "waiver_request_file_sha256",
    "waiver_request_content_sha256",
})
PROMOTED_ARTIFACT_NAMES = (
    "production-ledger.json",
    "runtime-matrix.json",
    "test-indexes-ledger.json",
)
PROMOTION_RECEIPT_NAME = "promotion-receipt.json"
_SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
_CONTROL_PATTERN = re.compile(r"[\x00-\x1f\x7f]")


class PromotionError(RuntimeError):
    """Raised when waiver authentication or candidate promotion fails."""


def _sha256_bytes(body: bytes) -> str:
    return hashlib.sha256(body).hexdigest()


def _canonical_json_sha256(value: dict[str, Any]) -> str:
    return _sha256_bytes(json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8"))


def _json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise PromotionError(f"duplicate JSON field {key!r}")
        result[key] = value
    return result


def _parse_json(body: bytes, context: str) -> dict[str, Any]:
    try:
        value = json.loads(
            body.decode("utf-8"),
            object_pairs_hook=_json_object,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise PromotionError(f"{context} is not valid UTF-8 JSON: {error}") \
            from error
    if not isinstance(value, dict):
        raise PromotionError(f"{context} root must be an object")
    return value


def _single_line_text(value: object, field: str) -> str:
    if (
        not isinstance(value, str)
        or not value.strip()
        or len(value) > 4096
        or _CONTROL_PATTERN.search(value) is not None
    ):
        raise PromotionError(
            f"{field} must be non-empty control-free text of at most "
            "4096 characters"
        )
    return value


def _lowercase_sha256(value: object, field: str) -> str:
    if (
        not isinstance(value, str)
        or _SHA256_PATTERN.fullmatch(value) is None
    ):
        raise PromotionError(f"{field} must be a lowercase SHA-256")
    return value


def _repository_root(path: Path) -> Path:
    try:
        resolved = path.resolve(strict=True)
        metadata = resolved.lstat()
    except OSError as error:
        raise PromotionError(f"repository root is unavailable: {path}") \
            from error
    if not stat.S_ISDIR(metadata.st_mode):
        raise PromotionError(f"repository root is not a directory: {resolved}")
    return resolved


def _repository_path(
        path: Path,
        repository_root: Path,
        *,
        strict: bool,
        context: str,
) -> Path:
    candidate = path
    if not candidate.is_absolute():
        candidate = repository_root / candidate
    try:
        if strict and candidate.is_symlink():
            raise PromotionError(f"{context} must not be a symlink: {candidate}")
        resolved = candidate.resolve(strict=strict)
        resolved.relative_to(repository_root)
    except (OSError, ValueError) as error:
        raise PromotionError(
            f"{context} must remain inside the repository: {candidate}"
        ) from error
    return resolved


def _read_regular_single_link(
        path: Path,
        context: str,
        *,
        required_mode: int | None = None,
) -> tuple[bytes, os.stat_result]:
    try:
        pathname_metadata = path.lstat()
    except OSError as error:
        raise PromotionError(f"{context} is unavailable: {path}") from error
    if (
        not stat.S_ISREG(pathname_metadata.st_mode)
        or stat.S_ISLNK(pathname_metadata.st_mode)
        or pathname_metadata.st_nlink != 1
    ):
        raise PromotionError(
            f"{context} must be a regular single-link non-symlink file: "
            f"{path}"
        )
    if (
        required_mode is not None
        and stat.S_IMODE(pathname_metadata.st_mode) != required_mode
    ):
        raise PromotionError(
            f"{context} must have mode {required_mode:04o}: {path}"
        )

    flags = os.O_RDONLY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags)
    try:
        opened_metadata = os.fstat(descriptor)
        if (
            not stat.S_ISREG(opened_metadata.st_mode)
            or opened_metadata.st_nlink != 1
            or (
                opened_metadata.st_dev,
                opened_metadata.st_ino,
            ) != (
                pathname_metadata.st_dev,
                pathname_metadata.st_ino,
            )
        ):
            raise PromotionError(
                f"{context} changed while it was opened: {path}"
            )
        chunks: list[bytes] = []
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
        final_metadata = os.fstat(descriptor)
    finally:
        os.close(descriptor)

    if (
        (
            final_metadata.st_dev,
            final_metadata.st_ino,
            final_metadata.st_size,
            final_metadata.st_mtime_ns,
        )
        != (
            opened_metadata.st_dev,
            opened_metadata.st_ino,
            opened_metadata.st_size,
            opened_metadata.st_mtime_ns,
        )
    ):
        raise PromotionError(f"{context} changed while it was read: {path}")
    body = b"".join(chunks)
    if len(body) != final_metadata.st_size:
        raise PromotionError(f"{context} size changed while it was read: {path}")
    return body, final_metadata


def _validate_waiver_fields(value: dict[str, Any]) -> None:
    if set(value) != REVIEW_WAIVER_FIELDS:
        raise PromotionError(
            "review waiver fields must be exactly: "
            + ", ".join(sorted(REVIEW_WAIVER_FIELDS))
        )
    if (
        type(value.get("schema_version")) is not int
        or value.get("schema_version") != 1
    ):
        raise PromotionError("review waiver schema_version must be 1")
    if value.get("artifact_kind") != REVIEW_WAIVER_KIND:
        raise PromotionError(
            f"review waiver artifact_kind must be {REVIEW_WAIVER_KIND!r}"
        )
    _single_line_text(
        value.get("approval_reference"),
        "approval_reference",
    )
    _single_line_text(value.get("waiver_reason"), "waiver_reason")
    if value.get("independent_review_performed") is not False:
        raise PromotionError(
            "review waiver must set independent_review_performed to false"
        )
    seal = _lowercase_sha256(
        value.get("content_sha256"),
        "content_sha256",
    )
    unsealed = dict(value)
    del unsealed["content_sha256"]
    if _canonical_json_sha256(unsealed) != seal:
        raise PromotionError("review waiver content seal does not match")


def load_review_waiver(
        path_value: object,
        repository_root: Path) -> dict[str, Any]:
    """Authenticate a repository waiver and return an immutable snapshot."""
    repository = _repository_root(repository_root)
    if not isinstance(path_value, str) or not path_value.strip():
        raise PromotionError(
            "postgres_error_safety_review_waiver must name a JSON file"
        )
    request_path = _repository_path(
        Path(path_value),
        repository,
        strict=True,
        context="review waiver",
    )
    if request_path.suffix.lower() != ".json":
        raise PromotionError("review waiver must name a .json file")
    body, _ = _read_regular_single_link(request_path, "review waiver")
    value = _parse_json(body, "review waiver")
    _validate_waiver_fields(value)
    snapshot = {
        "schema_version": 1,
        "artifact_kind": REVIEW_WAIVER_KIND,
        "approval_reference": value["approval_reference"],
        "waiver_reason": value["waiver_reason"],
        "independent_review_performed": False,
        "waiver_request_path": str(request_path),
        "waiver_request_file_sha256": _sha256_bytes(body),
        "waiver_request_content_sha256": value["content_sha256"],
    }
    return validate_review_waiver_snapshot(snapshot, repository)


def validate_review_waiver_snapshot(
        value: object,
        repository_root: Path) -> dict[str, Any]:
    """Validate the immutable waiver authority passed to a detached worker."""
    repository = _repository_root(repository_root)
    if not isinstance(value, dict):
        raise PromotionError("review waiver snapshot must be an object")
    if set(value) != REVIEW_WAIVER_SNAPSHOT_FIELDS:
        raise PromotionError(
            "review waiver snapshot fields must be exactly: "
            + ", ".join(sorted(REVIEW_WAIVER_SNAPSHOT_FIELDS))
        )
    if (
        type(value.get("schema_version")) is not int
        or value.get("schema_version") != 1
    ):
        raise PromotionError("review waiver snapshot schema_version must be 1")
    if value.get("artifact_kind") != REVIEW_WAIVER_KIND:
        raise PromotionError("review waiver snapshot artifact_kind is invalid")
    approval_reference = _single_line_text(
        value.get("approval_reference"),
        "approval_reference",
    )
    waiver_reason = _single_line_text(
        value.get("waiver_reason"),
        "waiver_reason",
    )
    if value.get("independent_review_performed") is not False:
        raise PromotionError(
            "review waiver snapshot cannot claim an independent review"
        )
    request_path_value = value.get("waiver_request_path")
    if not isinstance(request_path_value, str) or not request_path_value:
        raise PromotionError("waiver_request_path must be an absolute path")
    request_path = _repository_path(
        Path(request_path_value),
        repository,
        strict=False,
        context="waiver_request_path",
    )
    if (
        not Path(request_path_value).is_absolute()
        or str(request_path) != request_path_value
        or request_path.suffix.lower() != ".json"
    ):
        raise PromotionError(
            "waiver_request_path must be a canonical absolute repository JSON "
            "path"
        )
    request_file_sha256 = _lowercase_sha256(
        value.get("waiver_request_file_sha256"),
        "waiver_request_file_sha256",
    )
    request_content_sha256 = _lowercase_sha256(
        value.get("waiver_request_content_sha256"),
        "waiver_request_content_sha256",
    )
    expected_content_sha256 = _canonical_json_sha256({
        "schema_version": 1,
        "artifact_kind": REVIEW_WAIVER_KIND,
        "approval_reference": approval_reference,
        "waiver_reason": waiver_reason,
        "independent_review_performed": False,
    })
    if request_content_sha256 != expected_content_sha256:
        raise PromotionError(
            "waiver_request_content_sha256 does not bind the snapshot fields"
        )
    return {
        "schema_version": 1,
        "artifact_kind": REVIEW_WAIVER_KIND,
        "approval_reference": approval_reference,
        "waiver_reason": waiver_reason,
        "independent_review_performed": False,
        "waiver_request_path": str(request_path),
        "waiver_request_file_sha256": request_file_sha256,
        "waiver_request_content_sha256": request_content_sha256,
    }


def _secure_directory(path: Path, context: str, *, create: bool) -> Path:
    if create:
        try:
            path.mkdir(mode=0o700)
        except FileExistsError:
            pass
    try:
        metadata = path.lstat()
    except OSError as error:
        raise PromotionError(f"{context} is unavailable: {path}") from error
    if (
        not stat.S_ISDIR(metadata.st_mode)
        or stat.S_ISLNK(metadata.st_mode)
        or stat.S_IMODE(metadata.st_mode) != 0o700
    ):
        raise PromotionError(
            f"{context} must be a real directory with mode 0700: {path}"
        )
    if hasattr(os, "geteuid") and metadata.st_uid != os.geteuid():
        raise PromotionError(f"{context} is not owned by the current user: {path}")
    return path.resolve(strict=True)


def _atomic_private_file(path: Path, body: bytes, context: str) -> None:
    temporary = path.with_name(
        f".{path.name}.tmp-{os.getpid()}-{secrets.token_hex(8)}"
    )
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(temporary, flags, 0o600)
        try:
            os.fchmod(descriptor, 0o600)
            offset = 0
            while offset < len(body):
                written = os.write(descriptor, body[offset:])
                if written <= 0:
                    raise PromotionError(
                        f"{context} atomic write made no progress: {temporary}"
                    )
                offset += written
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        if path.exists() or path.is_symlink():
            raise PromotionError(
                f"{context} appeared during atomic publication: {path}"
            )
        os.replace(temporary, path)
        directory_descriptor = os.open(path.parent, os.O_RDONLY)
        try:
            try:
                os.fsync(directory_descriptor)
            except OSError as error:
                if error.errno not in {
                    errno.EINVAL,
                    getattr(errno, "ENOTSUP", errno.EINVAL),
                }:
                    raise
        finally:
            os.close(directory_descriptor)
    finally:
        temporary.unlink(missing_ok=True)


def _candidate_stamp(
        candidate_root: Path,
        candidate_hashes: dict[str, str]) -> None:
    body, _ = _read_regular_single_link(
        candidate_root / "candidate.stamp",
        "candidate stamp",
    )
    value = _parse_json(body, "candidate stamp")
    if type(value.get("schema_version")) is not int:
        raise PromotionError("candidate stamp schema_version must be integer 1")
    expected = {
        "schema_version": 1,
        "production_ledger_sha256":
            candidate_hashes["production-ledger.json"],
        "runtime_matrix_sha256":
            candidate_hashes["runtime-matrix.json"],
        "test_indexes_ledger_sha256":
            candidate_hashes["test-indexes-ledger.json"],
    }
    if value != expected:
        raise PromotionError(
            "candidate stamp does not bind the three promoted artifacts"
        )


def promote_reviewed_evidence(
        candidate_root: Path,
        waiver_snapshot: object,
        repository_root: Path) -> tuple[Path, dict[str, Any]]:
    """Byte-copy the candidate evidence and publish a sealed waiver receipt."""
    repository = _repository_root(repository_root)
    waiver = validate_review_waiver_snapshot(
        waiver_snapshot,
        repository,
    )
    root = _repository_path(
        candidate_root,
        repository,
        strict=True,
        context="candidate root",
    )
    try:
        root_metadata = root.lstat()
    except OSError as error:
        raise PromotionError(f"candidate root is unavailable: {root}") \
            from error
    if (
        not stat.S_ISDIR(root_metadata.st_mode)
        or stat.S_ISLNK(root_metadata.st_mode)
    ):
        raise PromotionError(
            f"candidate root must be a real directory: {root}"
        )

    candidates: dict[str, tuple[bytes, os.stat_result]] = {}
    candidate_hashes: dict[str, str] = {}
    for name in PROMOTED_ARTIFACT_NAMES:
        body, metadata = _read_regular_single_link(
            root / name,
            f"candidate {name}",
        )
        candidates[name] = (body, metadata)
        candidate_hashes[name] = _sha256_bytes(body)
    _candidate_stamp(root, candidate_hashes)

    reviewed = _secure_directory(
        root / "reviewed",
        "reviewed evidence root",
        create=True,
    )
    reviewed_path_metadata = reviewed.lstat()
    directory_flags = os.O_RDONLY
    if hasattr(os, "O_DIRECTORY"):
        directory_flags |= os.O_DIRECTORY
    if hasattr(os, "O_NOFOLLOW"):
        directory_flags |= os.O_NOFOLLOW
    directory_descriptor = os.open(reviewed, directory_flags)
    opened_directory_metadata = os.fstat(directory_descriptor)
    if (
        not stat.S_ISDIR(opened_directory_metadata.st_mode)
        or (
            opened_directory_metadata.st_dev,
            opened_directory_metadata.st_ino,
        ) != (
            reviewed_path_metadata.st_dev,
            reviewed_path_metadata.st_ino,
        )
    ):
        os.close(directory_descriptor)
        raise PromotionError(
            f"reviewed evidence root changed while opened: {reviewed}"
        )
    try:
        fcntl.flock(directory_descriptor, fcntl.LOCK_EX)
    except OSError:
        os.close(directory_descriptor)
        raise
    try:
        receipt_path = reviewed / PROMOTION_RECEIPT_NAME
        receipt_preexists = receipt_path.exists() or receipt_path.is_symlink()
        artifact_records: list[dict[str, Any]] = []
        for name in PROMOTED_ARTIFACT_NAMES:
            source_path = root / name
            reviewed_path = reviewed / name
            body, source_metadata = candidates[name]
            destination_preexists = (
                reviewed_path.exists() or reviewed_path.is_symlink()
            )
            if receipt_preexists and not destination_preexists:
                raise PromotionError(
                    "promotion receipt exists but reviewed artifact is "
                    f"missing: {reviewed_path}"
                )
            if destination_preexists:
                reviewed_body, reviewed_metadata = _read_regular_single_link(
                    reviewed_path,
                    f"reviewed {name}",
                    required_mode=0o600,
                )
                if reviewed_body != body:
                    raise PromotionError(
                        f"preexisting reviewed artifact differs from candidate: "
                        f"{reviewed_path}"
                    )
            else:
                _atomic_private_file(
                    reviewed_path,
                    body,
                    f"reviewed {name}",
                )
                reviewed_body, reviewed_metadata = _read_regular_single_link(
                    reviewed_path,
                    f"reviewed {name}",
                    required_mode=0o600,
                )
                if reviewed_body != body:
                    raise PromotionError(
                        f"atomic reviewed copy differs from candidate: "
                        f"{reviewed_path}"
                    )
            if (
                source_metadata.st_dev,
                source_metadata.st_ino,
            ) == (
                reviewed_metadata.st_dev,
                reviewed_metadata.st_ino,
            ):
                raise PromotionError(
                    f"reviewed artifact aliases candidate instead of being a "
                    f"byte copy: {reviewed_path}"
                )
            artifact_records.append({
                "name": name,
                "candidate_path": str(source_path),
                "reviewed_path": str(reviewed_path),
                "size": len(body),
                "sha256": candidate_hashes[name],
            })

        # Authenticate the candidate leaves again before sealing the receipt.
        for name in PROMOTED_ARTIFACT_NAMES:
            body, metadata = _read_regular_single_link(
                root / name,
                f"candidate {name}",
            )
            original_body, original_metadata = candidates[name]
            if (
                body != original_body
                or (
                    metadata.st_dev,
                    metadata.st_ino,
                    metadata.st_size,
                    metadata.st_mtime_ns,
                ) != (
                    original_metadata.st_dev,
                    original_metadata.st_ino,
                    original_metadata.st_size,
                    original_metadata.st_mtime_ns,
                )
            ):
                raise PromotionError(
                    f"candidate changed during promotion: {root / name}"
                )
            reviewed_body, reviewed_metadata = _read_regular_single_link(
                reviewed / name,
                f"reviewed {name}",
                required_mode=0o600,
            )
            if reviewed_body != original_body:
                raise PromotionError(
                    f"reviewed artifact changed during promotion: "
                    f"{reviewed / name}"
                )
            if (
                metadata.st_dev,
                metadata.st_ino,
            ) == (
                reviewed_metadata.st_dev,
                reviewed_metadata.st_ino,
            ):
                raise PromotionError(
                    f"reviewed artifact aliases candidate: {reviewed / name}"
                )

        receipt: dict[str, Any] = {
            "schema_version": 1,
            "artifact_kind": PROMOTION_RECEIPT_KIND,
            "independent_review_performed": False,
            "approval_reference": waiver["approval_reference"],
            "waiver_reason": waiver["waiver_reason"],
            "waiver_request_path": waiver["waiver_request_path"],
            "waiver_request_file_sha256":
                waiver["waiver_request_file_sha256"],
            "waiver_request_content_sha256":
                waiver["waiver_request_content_sha256"],
            "source_root": str(root),
            "reviewed_root": str(reviewed),
            "artifacts": artifact_records,
        }
        receipt["content_sha256"] = _canonical_json_sha256(receipt)
        receipt_body = (
            json.dumps(receipt, indent=2, sort_keys=True) + "\n"
        ).encode("utf-8")
        if receipt_preexists:
            existing_body, _ = _read_regular_single_link(
                receipt_path,
                "promotion receipt",
                required_mode=0o600,
            )
            if existing_body != receipt_body:
                raise PromotionError(
                    "preexisting promotion receipt does not bind this exact "
                    "waiver and candidate set"
                )
        else:
            _atomic_private_file(
                receipt_path,
                receipt_body,
                "promotion receipt",
            )
            existing_body, _ = _read_regular_single_link(
                receipt_path,
                "promotion receipt",
                required_mode=0o600,
            )
            if existing_body != receipt_body:
                raise PromotionError(
                    "published promotion receipt differs from expected bytes"
                )
        return receipt_path, receipt
    finally:
        try:
            fcntl.flock(directory_descriptor, fcntl.LOCK_UN)
        finally:
            os.close(directory_descriptor)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate-root", type=Path, required=True)
    parser.add_argument("--repository-root", type=Path, required=True)
    parser.add_argument("--review-waiver-snapshot", required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        snapshot = _parse_json(
            arguments.review_waiver_snapshot.encode("utf-8"),
            "review waiver snapshot",
        )
        receipt_path, _ = promote_reviewed_evidence(
            arguments.candidate_root,
            snapshot,
            arguments.repository_root,
        )
    except (OSError, PromotionError) as error:
        print(
            f"postgres_error_safety_promotion: {error}",
            file=sys.stderr,
        )
        return 1
    print(receipt_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
