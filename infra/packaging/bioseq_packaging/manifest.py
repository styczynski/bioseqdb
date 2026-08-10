"""Current package manifest construction and durable checkpoints.

The worker owns one mutable manifest per architecture.  This module keeps the
wire representation explicit, validates every lifecycle transition, and makes
each checkpoint durable before package work continues.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from datetime import datetime, timezone
from enum import Enum
import json
import os
from pathlib import Path
import re
import tempfile
from typing import Any

from .config import (
    MANIFEST_SCHEMA_REVISION,
    PACKAGE_VERSION,
    PINNED_SIGNING_FINGERPRINT,
)

_HEX_40_OR_64 = re.compile(r"(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})\Z")
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_CELL_STATES = frozenset({"building", "validated", "smoked", "uploaded", "failed"})
_FORMAT_SUFFIX = {"deb": ".deb", "rpm": ".rpm", "pacman": ".pkg.tar.zst"}
_SIGNATURE_KIND = {
    "deb": "signed-deb-release-sha256",
    "rpm": "embedded-rpm-openpgp",
    "pacman": "detached-pacman-openpgp",
}
_MANIFEST_ROOT_KEYS = frozenset({
    "schema_revision",
    "package_version",
    "architecture",
    "status",
    "started_at",
    "completed_at",
    "requested",
    "s3",
    "signing",
    "source_revision",
    "poetry_lock_sha256",
    "python_environment",
    "cells",
    "errors",
})


class ManifestError(RuntimeError):
    """Raised when a manifest transition or attestation is invalid."""


def utc_now() -> str:
    """Return a schema-compatible UTC timestamp."""

    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _manifest_value(value: object) -> object:
    """Convert only explicitly serializable values to the manifest wire form."""

    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, Path):
        return str(value)
    if isinstance(value, Enum):
        return _manifest_value(value.value)
    to_manifest = getattr(value, "to_manifest", None)
    if callable(to_manifest):
        return _manifest_value(to_manifest())
    if isinstance(value, Mapping):
        converted: dict[str, object] = {}
        for key, item in value.items():
            if not isinstance(key, str):
                raise ManifestError("manifest mapping keys must be strings")
            converted[key] = _manifest_value(item)
        return converted
    if isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray)):
        return [_manifest_value(item) for item in value]
    raise ManifestError(f"unsupported manifest value type: {type(value).__name__}")


def _manifest_mapping(value: object, *, label: str) -> dict[str, object]:
    converted = _manifest_value(value)
    if not isinstance(converted, dict):
        raise ManifestError(f"{label} must serialize as an object")
    return converted


def _fsync_directory(directory: Path) -> None:
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
    descriptor = os.open(directory, flags)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def write_manifest(path: Path, payload: Mapping[str, object]) -> None:
    """Atomically replace *path* and durably persist both file and directory.

    The temporary file is always created beside the destination, so
    ``os.replace`` cannot cross filesystems.  Mode, contents, and the rename are
    fsynced in that order.
    """

    actual_keys = set(payload)
    if actual_keys != _MANIFEST_ROOT_KEYS:
        raise ManifestError(
            "package manifest has unexpected keys: "
            f"missing={sorted(_MANIFEST_ROOT_KEYS - actual_keys)} "
            f"extra={sorted(actual_keys - _MANIFEST_ROOT_KEYS)}"
        )
    revision = payload.get("schema_revision")
    if type(revision) is not int or revision != MANIFEST_SCHEMA_REVISION:
        raise ManifestError(
            "package manifest schema_revision must be integer revision "
            f"{MANIFEST_SCHEMA_REVISION}"
        )

    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    document = _manifest_mapping(payload, label="manifest")
    encoded = (
        json.dumps(document, indent=2, sort_keys=True, allow_nan=False) + "\n"
    ).encode("utf-8")
    descriptor, temporary_name = tempfile.mkstemp(
        dir=destination.parent,
        prefix=f".{destination.name}.",
        suffix=".tmp",
    )
    temporary = Path(temporary_name)
    replaced = False
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(encoded)
            handle.flush()
            os.fchmod(handle.fileno(), 0o644)
            os.fsync(handle.fileno())
        os.replace(temporary, destination)
        replaced = True
        _fsync_directory(destination.parent)
    finally:
        if not replaced:
            try:
                os.close(descriptor)
            except OSError:
                pass
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _required_string(value: object, *, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ManifestError(f"{label} must be a non-empty string")
    return value.strip()


def _required_sha256(value: object, *, label: str) -> str:
    text = _required_string(value, label=label).lower()
    if _SHA256.fullmatch(text) is None:
        raise ManifestError(f"{label} must be a lowercase SHA-256 digest")
    return text


def _format_name(value: object) -> str:
    if isinstance(value, Enum):
        value = value.value
    name = str(value)
    if name not in _FORMAT_SUFFIX:
        raise ManifestError(f"unsupported package format {name!r}")
    return name


def _require_versioned_object(
    evidence: Mapping[str, object], *, label: str, expected_key: str | None = None
) -> None:
    key = _required_string(evidence.get("key"), label=f"{label} key")
    if expected_key is not None and key != expected_key:
        raise ManifestError(f"{label} key {key!r} does not equal {expected_key!r}")
    size = evidence.get("size")
    if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
        raise ManifestError(f"{label} size must be a positive integer")
    _required_sha256(evidence.get("sha256"), label=f"{label} SHA-256")
    _required_string(evidence.get("etag"), label=f"{label} ETag")
    _required_string(evidence.get("version_id"), label=f"{label} VersionId")
    if evidence.get("server_side_encryption") != "AES256":
        raise ManifestError(f"{label} must attest exact AES256 encryption")


def _validate_validation(value: object, *, label: str) -> None:
    validation = _manifest_mapping(value, label=label)
    required = {
        "passed": True,
        "six_artifacts": True,
        "rpath_absent": True,
        "compiler_runtime_safe": True,
        "template_flatten_profile": "dna4_uint64",
    }
    if any(validation.get(key) != expected for key, expected in required.items()):
        raise ManifestError(f"{label} did not pass every package assertion")
    binaries = validation.get("binaries")
    if not isinstance(binaries, list) or len(binaries) != 2:
        raise ManifestError(f"{label} must attest exactly two extension DSOs")
    names = {
        Path(str(binary.get("path", ""))).name
        for binary in binaries
        if isinstance(binary, dict)
    }
    if names != {"bioseq.so", "bioseq_cache.so"}:
        raise ManifestError(f"{label} attested unexpected extension DSOs")


def _validate_smoke(
    value: object, *, architecture: str, postgres_major: int, distro: str, label: str
) -> None:
    smoke = _manifest_mapping(value, label=label)
    required_true = (
        "passed",
        "package_installed",
        "preload_active",
        "bioseq_extension_created",
        "bioseq_cache_extension_created",
        "nonempty_roundtrip",
        "fresh_container",
    )
    if any(smoke.get(key) is not True for key in required_true):
        raise ManifestError(f"{label} did not pass every PostgreSQL assertion")
    if (
        smoke.get("architecture") != architecture
        or smoke.get("postgres_major") != postgres_major
        or smoke.get("distro") != distro
        or smoke.get("roundtrip") != "ACGT"
        or not isinstance(smoke.get("container_image"), str)
        or not str(smoke["container_image"]).strip()
    ):
        raise ManifestError(f"{label} attested the wrong matrix cell")


def _validate_artifact(value: object, *, label: str) -> dict[str, object]:
    artifact = _manifest_mapping(value, label=label)
    package_format = _format_name(artifact.get("format"))
    filename = _required_string(artifact.get("filename"), label=f"{label} filename")
    if not filename.endswith(_FORMAT_SUFFIX[package_format]):
        raise ManifestError(f"{label} filename has the wrong package suffix")
    for prefix in ("", "build_"):
        size = artifact.get(f"{prefix}size")
        if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
            raise ManifestError(f"{label} {prefix}size must be positive")
        _required_sha256(
            artifact.get(f"{prefix}sha256"), label=f"{label} {prefix}SHA-256"
        )
    _required_string(artifact.get("path"), label=f"{label} canonical path")
    _required_string(artifact.get("build_path"), label=f"{label} build path")
    if package_format != "rpm" and (
        artifact.get("size") != artifact.get("build_size")
        or artifact.get("sha256") != artifact.get("build_sha256")
    ):
        raise ManifestError(
            f"{label} DEB/PACMAN bytes differ between build and exact-version download"
        )
    return artifact


class ManifestStore:
    """Own the current manifest and checkpoint every cell transition."""

    __slots__ = ("path", "_payload", "_clock", "_redactor")

    def __init__(
        self,
        path: Path,
        payload: dict[str, object],
        *,
        clock: Callable[[], str],
        redactor: Callable[[str], str],
    ) -> None:
        self.path = Path(path)
        self._payload = payload
        self._clock = clock
        self._redactor = redactor

    @classmethod
    def create(
        cls,
        path: Path,
        *,
        architecture: object,
        requested_postgres: Sequence[int],
        requested_distros: Sequence[str],
        bucket: str,
        region: str,
        base_prefix: str,
        signing_fingerprint: str | None,
        source_revision: str,
        poetry_lock_sha256: str,
        python_environment: Mapping[str, str],
        package_version: str = PACKAGE_VERSION,
        clock: Callable[[], str] = utc_now,
        redactor: Callable[[str], str] | None = None,
    ) -> ManifestStore:
        """Create and immediately checkpoint a failed-until-complete manifest."""

        architecture_name = (
            str(architecture.value) if isinstance(architecture, Enum) else str(architecture)
        )
        if architecture_name not in {"arm64", "x64"}:
            raise ManifestError(f"unsupported manifest architecture {architecture_name!r}")
        postgres = list(requested_postgres)
        distros = list(requested_distros)
        if (
            not postgres
            or any(not isinstance(major, int) or isinstance(major, bool) or major < 17 for major in postgres)
            or len(set(postgres)) != len(postgres)
        ):
            raise ManifestError("requested PostgreSQL majors must be unique integers >= 17")
        if (
            not distros
            or any(not isinstance(distro, str) or not distro.strip() for distro in distros)
            or len(set(distros)) != len(distros)
        ):
            raise ManifestError("requested distros must be unique non-empty strings")
        revision = _required_string(source_revision, label="source revision").lower()
        if _HEX_40_OR_64.fullmatch(revision) is None:
            raise ManifestError("source revision must contain exactly 40 or 64 hexadecimal digits")
        lock_digest = _required_sha256(poetry_lock_sha256, label="Poetry lock SHA-256")
        fingerprint: str | None = None
        if signing_fingerprint is not None and signing_fingerprint.strip():
            fingerprint = signing_fingerprint.strip().upper()
            if fingerprint != PINNED_SIGNING_FINGERPRINT:
                raise ManifestError("package signing fingerprint does not equal the pinned key")
        python_version = _required_string(
            python_environment.get("python"), label="runtime Python version"
        )
        poetry_version = _required_string(
            python_environment.get("poetry"), label="runtime Poetry version"
        )
        payload: dict[str, object] = {
            "schema_revision": MANIFEST_SCHEMA_REVISION,
            "package_version": _required_string(package_version, label="package version"),
            "architecture": architecture_name,
            "status": "failed",
            "started_at": clock(),
            "completed_at": None,
            "requested": {"postgres": postgres, "distros": distros},
            "s3": {
                "bucket": _required_string(bucket, label="S3 bucket"),
                "region": _required_string(region, label="AWS region"),
                "base_prefix": _required_string(base_prefix, label="S3 base prefix").strip("/"),
            },
            "signing": {"fingerprint": fingerprint},
            "source_revision": revision,
            "poetry_lock_sha256": lock_digest,
            "python_environment": {
                "python": python_version,
                "poetry": poetry_version,
            },
            "cells": [],
            "errors": [],
        }
        store = cls(
            path,
            payload,
            clock=clock,
            redactor=redactor or (lambda value: value),
        )
        store.checkpoint()
        return store

    @property
    def payload(self) -> dict[str, object]:
        """Return the live manifest dictionary owned by this store."""

        return self._payload

    def checkpoint(self) -> None:
        """Durably checkpoint the current payload."""

        write_manifest(self.path, self._payload)

    def set_signing_fingerprint(self, fingerprint: str) -> None:
        """Pin the fetched signing key and checkpoint it before starting cells."""

        normalized = _required_string(fingerprint, label="signing fingerprint").upper()
        if normalized != PINNED_SIGNING_FINGERPRINT:
            raise ManifestError("package signing fingerprint does not equal the pinned key")
        self._payload["signing"] = {"fingerprint": normalized}
        self.checkpoint()

    def _cells(self) -> list[dict[str, object]]:
        cells = self._payload.get("cells")
        if not isinstance(cells, list) or not all(isinstance(cell, dict) for cell in cells):
            raise ManifestError("manifest cells were corrupted")
        return cells

    def _owned_cell(self, cell: dict[str, object]) -> dict[str, object]:
        if not any(candidate is cell for candidate in self._cells()):
            raise ManifestError("cell is not owned by this manifest")
        return cell

    def start_cell(
        self,
        *,
        postgres_major: int,
        distro: str,
        package_name: str,
        build_dir: Path,
    ) -> dict[str, object]:
        """Add one requested cell in ``building`` state and checkpoint it."""

        requested = _manifest_mapping(self._payload.get("requested"), label="requested matrix")
        requested_keys = {
            (major, distro_name)
            for major in requested.get("postgres", [])
            for distro_name in requested.get("distros", [])
        }
        key = (postgres_major, distro)
        if key not in requested_keys:
            raise ManifestError(f"cell {key!r} is outside the requested matrix")
        if any(
            (candidate.get("postgres_major"), candidate.get("distro")) == key
            for candidate in self._cells()
        ):
            raise ManifestError(f"duplicate manifest cell {key!r}")
        if package_name != f"bioseq-postgresql{postgres_major}":
            raise ManifestError(f"cell {key!r} has an invalid package name")
        cell: dict[str, object] = {
            "postgres_major": postgres_major,
            "distro": distro,
            "package_name": package_name,
            "status": "building",
            "build_dir": str(Path(build_dir)),
            "artifact": None,
            "validation": None,
            "smoke": None,
            "upload": None,
            "error": None,
        }
        self._cells().append(cell)
        self.checkpoint()
        return cell

    def _expect_state(self, cell: dict[str, object], expected: str) -> None:
        self._owned_cell(cell)
        actual = cell.get("status")
        if actual not in _CELL_STATES:
            raise ManifestError(f"cell has an unknown state {actual!r}")
        if actual != expected:
            raise ManifestError(f"cell transition requires {expected!r}, found {actual!r}")

    def mark_validated(
        self, cell: dict[str, object], artifact: object, validation: object
    ) -> None:
        """Record built artifact evidence and advance ``building -> validated``."""

        self._expect_state(cell, "building")
        artifact_document = _validate_artifact(artifact, label="built artifact")
        _validate_validation(validation, label="built-package validation")
        cell["artifact"] = artifact_document
        cell["validation"] = _manifest_value(validation)
        cell["status"] = "validated"
        self.checkpoint()

    def mark_smoked(self, cell: dict[str, object], smoke: object) -> None:
        """Record fresh-container SQL evidence and advance ``validated -> smoked``."""

        self._expect_state(cell, "validated")
        _validate_smoke(
            smoke,
            architecture=str(self._payload["architecture"]),
            postgres_major=int(cell["postgres_major"]),
            distro=str(cell["distro"]),
            label="built-package smoke",
        )
        cell["smoke"] = _manifest_value(smoke)
        cell["status"] = "smoked"
        self.checkpoint()

    def mark_uploaded(
        self,
        cell: dict[str, object],
        upload: object,
        *,
        artifact: object | None = None,
    ) -> None:
        """Record exact-version publication and advance ``smoked -> uploaded``."""

        self._expect_state(cell, "smoked")
        if artifact is not None:
            cell["artifact"] = _manifest_value(artifact)
        self._validate_uploaded_cell(cell, upload)
        cell["upload"] = _manifest_value(upload)
        cell["status"] = "uploaded"
        cell["error"] = None
        self.checkpoint()

    def _safe_error(self, error: BaseException | str) -> str:
        message = self._redactor(str(error)).strip()
        return message or type(error).__name__

    def append_error(
        self,
        error: BaseException | str,
        *,
        postgres_major: int | None = None,
        distro: str | None = None,
    ) -> None:
        """Append one redacted top-level error without checkpointing."""

        item: dict[str, object] = {"error": self._safe_error(error)}
        if postgres_major is not None:
            item["postgres_major"] = postgres_major
        if distro is not None:
            item["distro"] = distro
        errors = self._payload.get("errors")
        if not isinstance(errors, list):
            raise ManifestError("manifest errors were corrupted")
        if item not in errors:
            errors.append(item)

    def fail_cell(self, cell: dict[str, object], error: BaseException | str) -> None:
        """Fail only the current cell, append a redacted error, and checkpoint."""

        self._owned_cell(cell)
        if cell.get("status") == "uploaded":
            raise ManifestError("an uploaded cell cannot transition to failed")
        message = self._safe_error(error)
        cell["status"] = "failed"
        cell["error"] = message
        self.append_error(
            message,
            postgres_major=int(cell["postgres_major"]),
            distro=str(cell["distro"]),
        )
        self._payload["status"] = "failed"
        self.checkpoint()

    def _validate_uploaded_cell(self, cell: dict[str, object], upload_value: object) -> None:
        artifact = _validate_artifact(cell.get("artifact"), label="canonical artifact")
        upload = _manifest_mapping(upload_value, label="upload evidence")
        if upload.get("published") is not True:
            raise ManifestError("upload evidence is not published")
        s3 = _manifest_mapping(self._payload.get("s3"), label="S3 contract")
        if upload.get("bucket") != s3.get("bucket"):
            raise ManifestError("upload bucket differs from the manifest S3 contract")
        package_format = _format_name(artifact.get("format"))
        base = (
            f"{s3['base_prefix']}/{self._payload['architecture']}/"
            f"postgresql{cell['postgres_major']}/{cell['distro']}"
        )
        if package_format == "deb":
            base += "/stable"
        expected_key = f"{base}/{artifact['filename']}"
        _require_versioned_object(upload, label="published package", expected_key=expected_key)
        if upload.get("size") != artifact.get("size") or upload.get("sha256") != artifact.get("sha256"):
            raise ManifestError("published package evidence differs from canonical download")
        _validate_validation(
            upload.get("download_validation"), label="downloaded-package validation"
        )
        _validate_smoke(
            upload.get("download_smoke"),
            architecture=str(self._payload["architecture"]),
            postgres_major=int(cell["postgres_major"]),
            distro=str(cell["distro"]),
            label="downloaded-package smoke",
        )
        signature = _manifest_mapping(
            upload.get("signature_verification"), label="signature verification"
        )
        if (
            signature.get("passed") is not True
            or signature.get("public_key_imported") is not True
            or signature.get("package_signature_verified") is not True
            or signature.get("repository_signature_verified") is not True
            or signature.get("format") != package_format
            or signature.get("package_signature") != _SIGNATURE_KIND[package_format]
            or signature.get("signing_key_fingerprint") != PINNED_SIGNING_FINGERPRINT
        ):
            raise ManifestError("signature verification did not pass the pinned-key contract")
        signature_base = (
            f"{s3['base_prefix']}/{self._payload['architecture']}/"
            f"postgresql{cell['postgres_major']}/{cell['distro']}"
        )
        expected_signature_keys = {
            "rpm": {
                f"{signature_base}/repodata/repomd.xml.key",
                f"{signature_base}/repodata/repomd.xml",
                f"{signature_base}/repodata/repomd.xml.asc",
            },
            "deb": {
                f"{signature_base}/stable/Release.key",
                f"{signature_base}/stable/Release",
                f"{signature_base}/stable/Release.gpg",
                f"{signature_base}/stable/InRelease",
            },
            "pacman": {
                f"{signature_base}/public.key",
                f"{expected_key}.sig",
                f"{signature_base}/{cell['package_name']}.db.tar.gz",
                f"{signature_base}/{cell['package_name']}.db.tar.gz.sig",
            },
        }[package_format]
        objects = signature.get("objects")
        if not isinstance(objects, list) or len(objects) != len(expected_signature_keys):
            raise ManifestError("signature evidence has the wrong object count")
        actual_keys: set[str] = set()
        for index, item in enumerate(objects):
            if not isinstance(item, dict):
                raise ManifestError("signature object evidence must be an object")
            _require_versioned_object(item, label=f"signature object {index}")
            actual_keys.add(str(item["key"]))
        if actual_keys != expected_signature_keys:
            raise ManifestError("signature evidence has missing, duplicate, or extra objects")
        repository_objects = signature.get("repository_objects", [])
        if not isinstance(repository_objects, list):
            raise ManifestError("repository object evidence must be a list")
        for index, item in enumerate(repository_objects):
            if not isinstance(item, dict):
                raise ManifestError("repository object evidence must be an object")
            _require_versioned_object(item, label=f"repository object {index}")

    def require_complete(self) -> None:
        """Require exact requested-set equality and full uploaded attestations."""

        signing = _manifest_mapping(self._payload.get("signing"), label="signing evidence")
        if signing.get("fingerprint") != PINNED_SIGNING_FINGERPRINT:
            raise ManifestError("successful manifest does not attest the pinned signing key")
        requested = _manifest_mapping(self._payload.get("requested"), label="requested matrix")
        expected = {
            (major, distro)
            for major in requested.get("postgres", [])
            for distro in requested.get("distros", [])
        }
        cells = self._cells()
        actual = [(cell.get("postgres_major"), cell.get("distro")) for cell in cells]
        if len(cells) != len(expected) or len(set(actual)) != len(actual) or set(actual) != expected:
            raise ManifestError("manifest cell set has missing, duplicate, stale, or extra cells")
        errors = self._payload.get("errors")
        if not isinstance(errors, list) or errors:
            raise ManifestError("a successful manifest cannot contain errors")
        for cell in cells:
            if cell.get("status") != "uploaded" or cell.get("error") not in (None, ""):
                raise ManifestError(f"cell {(cell.get('postgres_major'), cell.get('distro'))!r} is incomplete")
            _validate_validation(cell.get("validation"), label="built-package validation")
            _validate_smoke(
                cell.get("smoke"),
                architecture=str(self._payload["architecture"]),
                postgres_major=int(cell["postgres_major"]),
                distro=str(cell["distro"]),
                label="built-package smoke",
            )
            self._validate_uploaded_cell(cell, cell.get("upload"))

    def finalize(self) -> dict[str, object]:
        """Set terminal status, timestamp, checkpoint, and return the payload."""

        try:
            self.require_complete()
        except ManifestError as error:
            self._payload["status"] = "failed"
            if not self._payload.get("errors"):
                self.append_error(error)
        else:
            self._payload["status"] = "success"
        self._payload["completed_at"] = self._clock()
        self.checkpoint()
        return self._payload
