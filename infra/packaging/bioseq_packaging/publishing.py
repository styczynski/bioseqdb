"""Fail-closed, exact-version S3 publication for one package matrix cell."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
from types import MappingProxyType
from typing import Mapping

from .commands import Command, CommandRunner, Redactor
from .model import CellContext, DistroId, PackageBuildError, PackageFormat, PublishedObject
from .repositories import RepositoryBuild


_BUCKET_NAME = re.compile(r"[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]\Z")
_KEY_SEGMENT = re.compile(r"[A-Za-z0-9._-]+\Z")
_POSTGRES_SEGMENT = re.compile(r"postgresql([0-9]+)\Z")
_CREDENTIAL_ENVIRONMENT = (
    "AWS_ACCESS_KEY_ID",
    "AWS_SECRET_ACCESS_KEY",
    "AWS_SESSION_TOKEN",
)


@dataclass(frozen=True, slots=True)
class UploadResult:
    """Exact-version objects and downloads produced by one publication."""

    package: PublishedObject
    signature_objects: tuple[PublishedObject, ...]
    downloaded_path: Path
    objects: tuple[PublishedObject, ...]
    downloaded_objects: Mapping[str, Path]

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "downloaded_objects",
            MappingProxyType(dict(self.downloaded_objects)),
        )


@dataclass(frozen=True, slots=True)
class _PutEvidence:
    version_id: str
    etag: str


class Publisher:
    """Publish a complete repository without exposing mismatched entry metadata."""

    def __init__(
        self,
        *,
        aws_bin: str,
        region: str,
        bucket: str,
        prefix: str,
        runner: CommandRunner,
        redactor: Redactor,
    ) -> None:
        self._aws_bin = self._nonempty(aws_bin, label="AWS executable")
        self._region = self._nonempty(region, label="AWS region")
        self._bucket = self._validate_bucket(bucket)
        self._prefix = self._validate_base_prefix(prefix)
        self._runner = runner
        self._redactor = redactor
        self._environment = self._aws_environment(os.environ)

    def publish(
        self,
        cell: CellContext,
        repository_build: RepositoryBuild,
    ) -> UploadResult:
        """Publish and attest one repository using fail-closed object ordering."""

        cell_prefix = self._validate_cell_prefix(cell)
        repository_root, tree = self._repository_tree(repository_build, cell_prefix)
        package_key = self._key_for_path(
            repository_root,
            repository_build.package_path,
            cell_prefix,
        )
        signature_keys = tuple(
            self._key_for_path(repository_root, path, cell_prefix)
            for path in repository_build.signature_paths
        )
        declared_metadata_keys = {
            self._key_for_path(repository_root, path, cell_prefix)
            for path in repository_build.metadata_paths
        }
        if package_key not in tree:
            raise PackageBuildError("repository package is absent from the publish tree")
        if not signature_keys:
            raise PackageBuildError("repository signature evidence must not be empty")
        if len(set(signature_keys)) != len(signature_keys):
            raise PackageBuildError("repository signature evidence contains duplicate paths")
        if any(key not in tree for key in signature_keys):
            raise PackageBuildError("repository signature evidence is outside the publish tree")
        if any(key not in tree for key in declared_metadata_keys):
            raise PackageBuildError("repository metadata evidence is outside the publish tree")

        # Repository builders may report the complete generated metadata tree.
        # Only consumer entrypoints are invalidated/published last; referenced
        # indexes, public keys, and static repository configuration are payload.
        metadata_keys = {
            key
            for key in set(tree) | declared_metadata_keys
            if self._is_consumer_metadata(self._relative_key(key, cell_prefix))
        }
        if not metadata_keys:
            raise PackageBuildError("consumer metadata set must not be empty")
        initial_keys = self._list_keys(cell_prefix)
        old_metadata_keys = {
            key
            for key in initial_keys
            if key in metadata_keys
            or self._is_consumer_metadata(self._relative_key(key, cell_prefix))
        }

        # Invalidate every old consumer entrypoint before changing any payload byte.
        for key in sorted(old_metadata_keys):
            self._delete_object(key, cell_prefix)
        remaining_after_invalidation = self._list_keys(cell_prefix)
        still_visible_metadata = {
            key
            for key in remaining_after_invalidation
            if key in metadata_keys
            or self._is_consumer_metadata(self._relative_key(key, cell_prefix))
        }
        if still_visible_metadata:
            raise PackageBuildError(
                "consumer metadata remained visible after invalidation: "
                + ", ".join(sorted(still_visible_metadata))
            )

        put_evidence: dict[str, _PutEvidence] = {}
        payload_keys = set(tree) - metadata_keys
        payload_order = self._payload_order(package_key, signature_keys, payload_keys)
        for key in payload_order:
            put_evidence[key] = self._put_object(key, tree[key], cell_prefix)

        # Consumer entrypoints become visible only after every referenced payload exists.
        for key in sorted(metadata_keys, key=self._metadata_order):
            put_evidence[key] = self._put_object(key, tree[key], cell_prefix)

        # Delete only currently visible stale objects returned by the exact cell listing.
        visible_after_puts = self._list_keys(cell_prefix)
        for key in sorted(visible_after_puts - set(tree)):
            self._delete_object(key, cell_prefix)
        final_keys = self._list_keys(cell_prefix)
        if final_keys != set(tree):
            missing = sorted(set(tree) - final_keys)
            extra = sorted(final_keys - set(tree))
            raise PackageBuildError(
                f"published S3 tree is not exact; missing={missing}, extra={extra}"
            )

        download_root = self._validated_download_root(cell)
        published_by_key: dict[str, PublishedObject] = {}
        downloaded_by_key: dict[str, Path] = {}
        for key in sorted(tree):
            destination = self._download_destination(download_root, key, cell_prefix)
            published_by_key[key] = self.download_and_attest(
                key=key,
                version_id=put_evidence[key].version_id,
                source_path=tree[key],
                destination=destination,
                expected_etag=put_evidence[key].etag,
                cell_prefix=cell_prefix,
            )
            downloaded_by_key[key] = destination

        return UploadResult(
            package=published_by_key[package_key],
            signature_objects=tuple(published_by_key[key] for key in signature_keys),
            downloaded_path=downloaded_by_key[package_key],
            objects=tuple(published_by_key[key] for key in sorted(published_by_key)),
            downloaded_objects=downloaded_by_key,
        )

    def download_and_attest(
        self,
        *,
        key: str,
        version_id: str,
        source_path: Path,
        destination: Path,
        expected_etag: str = "",
        cell_prefix: str | None = None,
    ) -> PublishedObject:
        """HEAD and GET one captured version, then bind S3 evidence to exact bytes."""

        validated_prefix = cell_prefix or self._prefix_from_object_key(key)
        self._require_key_in_prefix(key, validated_prefix)
        captured_version = self._nonempty(version_id, label="S3 VersionId")
        source = Path(source_path)
        if not source.is_file() or source.stat().st_size <= 0:
            raise PackageBuildError(f"published source object is missing or empty: {source}")

        head = self._json_object(
            self._run(
                "s3api",
                "head-object",
                "--bucket",
                self._bucket,
                "--key",
                key,
                "--version-id",
                captured_version,
                "--region",
                self._region,
                "--output",
                "json",
            ),
            operation="head-object",
        )
        self._validate_exact_version_response(
            head,
            key=key,
            version_id=captured_version,
            operation="head-object",
            require_encryption=True,
        )
        size = self._positive_int(head.get("ContentLength"), label="ContentLength")
        etag = self._etag(head.get("ETag"))
        if expected_etag and etag != expected_etag:
            raise PackageBuildError(f"put/head ETag mismatch for {key}")

        destination = Path(destination)
        self._prepare_destination(destination)
        get_result = self._json_object(
            self._run(
                "s3api",
                "get-object",
                "--bucket",
                self._bucket,
                "--key",
                key,
                "--version-id",
                captured_version,
                "--region",
                self._region,
                "--output",
                "json",
                str(destination),
            ),
            operation="get-object",
        )
        self._validate_exact_version_response(
            get_result,
            key=key,
            version_id=captured_version,
            operation="get-object",
            require_encryption=False,
        )
        if not destination.is_file():
            raise PackageBuildError(f"get-object did not create {destination}")
        downloaded_size = destination.stat().st_size
        downloaded_sha256 = self._sha256(destination)
        if downloaded_size != size:
            raise PackageBuildError(f"GET size differs from exact-version HEAD for {key}")
        if downloaded_size != source.stat().st_size or downloaded_sha256 != self._sha256(source):
            raise PackageBuildError(f"exact-version S3 bytes differ from published source for {key}")

        return PublishedObject(
            key=key,
            size=downloaded_size,
            sha256=downloaded_sha256,
            etag=etag,
            version_id=captured_version,
            server_side_encryption="AES256",
        )

    def _put_object(
        self,
        key: str,
        source: Path,
        cell_prefix: str,
    ) -> _PutEvidence:
        self._require_key_in_prefix(key, cell_prefix)
        result = self._json_object(
            self._run(
                "s3api",
                "put-object",
                "--bucket",
                self._bucket,
                "--key",
                key,
                "--body",
                str(source),
                "--server-side-encryption",
                "AES256",
                "--region",
                self._region,
                "--output",
                "json",
            ),
            operation="put-object",
        )
        version_id = self._nonempty(
            str(result.get("VersionId", "")),
            label=f"put-object VersionId for {key}",
        )
        encryption = str(result.get("ServerSideEncryption", ""))
        if encryption and encryption != "AES256":
            raise PackageBuildError(f"put-object returned non-AES256 encryption for {key}")
        return _PutEvidence(version_id=version_id, etag=self._etag(result.get("ETag")))

    def _delete_object(self, key: str, cell_prefix: str) -> None:
        self._require_key_in_prefix(key, cell_prefix)
        self._run(
            "s3api",
            "delete-object",
            "--bucket",
            self._bucket,
            "--key",
            key,
            "--region",
            self._region,
            "--output",
            "json",
        )

    def _list_keys(self, cell_prefix: str) -> set[str]:
        keys: set[str] = set()
        continuation_token: str | None = None
        seen_tokens: set[str] = set()
        while True:
            argv = [
                "s3api",
                "list-objects-v2",
                "--bucket",
                self._bucket,
                "--prefix",
                cell_prefix,
                "--max-keys",
                "1000",
                "--region",
                self._region,
                "--output",
                "json",
                "--no-paginate",
            ]
            if continuation_token is not None:
                argv.extend(("--continuation-token", continuation_token))
            result = self._json_object(self._run(*argv), operation="list-objects-v2")
            contents = result.get("Contents", [])
            if contents is None:
                contents = []
            if not isinstance(contents, list):
                raise PackageBuildError("list-objects-v2 Contents must be a list")
            for entry in contents:
                if not isinstance(entry, dict) or not isinstance(entry.get("Key"), str):
                    raise PackageBuildError("list-objects-v2 returned an invalid object entry")
                key = entry["Key"]
                self._require_key_in_prefix(key, cell_prefix)
                if key in keys:
                    raise PackageBuildError(f"list-objects-v2 returned duplicate key {key}")
                keys.add(key)
            if result.get("IsTruncated") is not True:
                return keys
            next_token = self._nonempty(
                str(result.get("NextContinuationToken", "")),
                label="S3 continuation token",
            )
            if next_token in seen_tokens:
                raise PackageBuildError("list-objects-v2 repeated a continuation token")
            seen_tokens.add(next_token)
            continuation_token = next_token

    def _repository_tree(
        self,
        repository_build: RepositoryBuild,
        cell_prefix: str,
    ) -> tuple[Path, dict[str, Path]]:
        root = Path(repository_build.root)
        if not root.is_absolute() or not root.is_dir():
            raise PackageBuildError("repository root must be an existing absolute directory")
        resolved_root = root.resolve(strict=True)
        tree: dict[str, Path] = {}
        for candidate in sorted(root.rglob("*")):
            if candidate.is_dir():
                continue
            if not candidate.is_file():
                raise PackageBuildError(f"repository contains a non-file object: {candidate}")
            resolved_candidate = candidate.resolve(strict=True)
            if not resolved_candidate.is_relative_to(resolved_root):
                raise PackageBuildError(f"repository symlink escapes its root: {candidate}")
            if candidate.stat().st_size <= 0:
                raise PackageBuildError(f"repository object is empty: {candidate}")
            relative = candidate.relative_to(root)
            self._validate_relative_path(relative)
            key = cell_prefix + relative.as_posix()
            self._require_key_in_prefix(key, cell_prefix)
            if key in tree:
                raise PackageBuildError(f"repository produces duplicate S3 key {key}")
            tree[key] = candidate
        if not tree:
            raise PackageBuildError("repository publish tree is empty")
        return root, tree

    def _key_for_path(self, root: Path, path: Path, cell_prefix: str) -> str:
        candidate = Path(path)
        if not candidate.is_absolute():
            candidate = root / candidate
        try:
            relative = candidate.relative_to(root)
        except ValueError as error:
            raise PackageBuildError(f"repository evidence path is outside its root: {path}") from error
        self._validate_relative_path(relative)
        resolved_root = root.resolve(strict=True)
        try:
            resolved_candidate = candidate.resolve(strict=True)
        except OSError as error:
            raise PackageBuildError(f"repository evidence path is missing: {path}") from error
        if not resolved_candidate.is_relative_to(resolved_root):
            raise PackageBuildError(f"repository evidence path escapes its root: {path}")
        key = cell_prefix + relative.as_posix()
        self._require_key_in_prefix(key, cell_prefix)
        return key

    def _validate_cell_prefix(self, cell: CellContext) -> str:
        value = cell.s3_cell_prefix
        if value != value.strip() or not value.endswith("/") or value.endswith("//"):
            raise PackageBuildError("S3 cell prefix must be normalized with one trailing slash")
        body = value[:-1]
        self._validate_key_path(body, label="S3 cell prefix")
        base = self._prefix + "/"
        if not value.startswith(base):
            raise PackageBuildError("S3 cell prefix is outside the configured prefix")
        relative_parts = body[len(base) :].split("/")
        if len(relative_parts) not in (3, 4):
            raise PackageBuildError("S3 cell prefix has the wrong number of segments")
        architecture, postgres_segment, distro_id, *suffix = relative_parts
        if architecture not in ("arm64", "x64"):
            raise PackageBuildError("S3 cell prefix has an invalid architecture")
        postgres_match = _POSTGRES_SEGMENT.fullmatch(postgres_segment)
        if postgres_match is None or int(postgres_match.group(1)) < 17:
            raise PackageBuildError("S3 cell prefix has an invalid PostgreSQL major")
        try:
            parsed_distro = DistroId(distro_id)
        except ValueError as error:
            raise PackageBuildError("S3 cell prefix has an invalid distro") from error
        apt_distro = parsed_distro in (DistroId.DEBIAN_13, DistroId.UBUNTU_26_04)
        if suffix != (["stable"] if apt_distro else []):
            raise PackageBuildError("S3 cell prefix has an invalid repository suffix")

        requested_architecture = str(cell.request.architecture.value)
        requested_distro = str(cell.distro.distro_id.value)
        expected = (
            f"{self._prefix}/{requested_architecture}/postgresql{cell.postgres_major}/"
            f"{requested_distro}/"
        )
        if cell.distro.package_format is PackageFormat.DEB:
            expected += "stable/"
        if value != expected:
            raise PackageBuildError(
                f"S3 cell prefix does not match the cell identity: {value!r} != {expected!r}"
            )
        return value

    @classmethod
    def _validate_bucket(cls, value: str) -> str:
        bucket = cls._nonempty(value, label="S3 bucket")
        if bucket != value or _BUCKET_NAME.fullmatch(bucket) is None:
            raise PackageBuildError("S3 bucket name is not normalized")
        if ".." in bucket or ".-" in bucket or "-." in bucket:
            raise PackageBuildError("S3 bucket name is invalid")
        try:
            ipaddress.ip_address(bucket)
        except ValueError:
            return bucket
        raise PackageBuildError("S3 bucket name must not be an IP address")

    @classmethod
    def _validate_base_prefix(cls, value: str) -> str:
        prefix = cls._nonempty(value, label="S3 prefix")
        if prefix != value or prefix.startswith("/") or prefix.endswith("/"):
            raise PackageBuildError("S3 prefix must be normalized without surrounding slashes")
        cls._validate_key_path(prefix, label="S3 prefix")
        return prefix

    @classmethod
    def _validate_key_path(cls, value: str, *, label: str) -> None:
        if "\\" in value or "//" in value:
            raise PackageBuildError(f"{label} is not normalized")
        parts = value.split("/")
        if any(part in ("", ".", "..") or _KEY_SEGMENT.fullmatch(part) is None for part in parts):
            raise PackageBuildError(f"{label} contains an unsafe path segment")

    @classmethod
    def _validate_relative_path(cls, value: Path) -> None:
        if value.is_absolute():
            raise PackageBuildError("repository object path must be relative")
        cls._validate_key_path(value.as_posix(), label="repository object path")

    @staticmethod
    def _is_consumer_metadata(relative_key: str) -> bool:
        name = relative_key.rsplit("/", 1)[-1]
        if relative_key.endswith("repodata/repomd.xml") or relative_key.endswith(
            "repodata/repomd.xml.asc"
        ):
            return True
        if name in {"Release", "Release.gpg", "InRelease"}:
            return True
        return any(
            name.endswith(suffix)
            for suffix in (
                ".db",
                ".db.sig",
                ".db.tar.gz",
                ".db.tar.gz.sig",
                ".files",
                ".files.sig",
                ".files.tar.gz",
                ".files.tar.gz.sig",
            )
        )

    @staticmethod
    def _payload_order(
        package_key: str,
        signature_keys: tuple[str, ...],
        payload_keys: set[str],
    ) -> tuple[str, ...]:
        result: list[str] = []
        for key in (package_key, *signature_keys):
            if key in payload_keys and key not in result:
                result.append(key)
        result.extend(sorted(payload_keys - set(result)))
        return tuple(result)

    @staticmethod
    def _metadata_order(key: str) -> tuple[int, str]:
        name = key.rsplit("/", 1)[-1]
        if name.endswith((".sig", ".asc", ".gpg")):
            return (0, key)
        if name == "InRelease":
            return (2, key)
        return (1, key)

    @staticmethod
    def _relative_key(key: str, cell_prefix: str) -> str:
        if not key.startswith(cell_prefix):
            raise PackageBuildError(f"S3 key is outside the exact cell prefix: {key}")
        return key[len(cell_prefix) :]

    @classmethod
    def _require_key_in_prefix(cls, key: str, cell_prefix: str) -> None:
        if not key.startswith(cell_prefix):
            raise PackageBuildError(f"S3 key is outside the exact cell prefix: {key}")
        relative = key[len(cell_prefix) :]
        if relative:
            cls._validate_key_path(relative, label="S3 object key")

    def _prefix_from_object_key(self, key: str) -> str:
        base = self._prefix + "/"
        if not key.startswith(base):
            raise PackageBuildError("S3 object key is outside the configured prefix")
        parts = key[len(base) :].split("/")
        if len(parts) < 4:
            raise PackageBuildError("S3 object key is missing its cell prefix")
        cell_length = 4 if parts[2] in {"debian_13", "ubuntu_26.04"} else 3
        return base + "/".join(parts[:cell_length]) + "/"

    @staticmethod
    def _validated_download_root(cell: CellContext) -> Path:
        root = Path(cell.paths.download_root)
        if not root.is_absolute():
            raise PackageBuildError("download root must be absolute")
        root.mkdir(parents=True, exist_ok=True)
        if not root.is_dir():
            raise PackageBuildError("download root is not a directory")
        return root.resolve(strict=True)

    @classmethod
    def _download_destination(cls, root: Path, key: str, cell_prefix: str) -> Path:
        relative = cls._relative_key(key, cell_prefix)
        cls._validate_key_path(relative, label="download object path")
        destination = root / Path(relative)
        resolved_parent = destination.parent.resolve(strict=False)
        if not resolved_parent.is_relative_to(root):
            raise PackageBuildError("download destination escapes the cell download root")
        return destination

    @staticmethod
    def _prepare_destination(destination: Path) -> None:
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.is_symlink() or (destination.exists() and not destination.is_file()):
            raise PackageBuildError(f"unsafe exact-version download destination: {destination}")

    @staticmethod
    def _sha256(path: Path) -> str:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()

    def _run(self, *argv: str) -> str:
        try:
            return self._runner.run_captured(
                Command(argv=(self._aws_bin, *argv)),
                environment=self._environment,
            )
        except Exception as error:
            if isinstance(error, PackageBuildError):
                raise
            raise PackageBuildError(self._redactor.redact(error)) from error

    @staticmethod
    def _json_object(output: str, *, operation: str) -> dict[str, object]:
        try:
            value = json.loads(output)
        except json.JSONDecodeError as error:
            raise PackageBuildError(f"{operation} returned invalid JSON") from error
        if not isinstance(value, dict):
            raise PackageBuildError(f"{operation} returned a non-object JSON value")
        return value

    @classmethod
    def _validate_exact_version_response(
        cls,
        result: Mapping[str, object],
        *,
        key: str,
        version_id: str,
        operation: str,
        require_encryption: bool,
    ) -> None:
        returned_version = str(result.get("VersionId", ""))
        if returned_version != version_id:
            raise PackageBuildError(f"{operation} did not return captured VersionId for {key}")
        encryption = str(result.get("ServerSideEncryption", ""))
        if require_encryption and encryption != "AES256":
            raise PackageBuildError(f"{operation} did not report exact AES256 encryption for {key}")
        if encryption and encryption != "AES256":
            raise PackageBuildError(f"{operation} reported non-AES256 encryption for {key}")

    @staticmethod
    def _etag(value: object) -> str:
        etag = str(value or "").strip().strip('"').strip()
        if not etag:
            raise PackageBuildError("S3 response is missing ETag")
        return etag

    @staticmethod
    def _positive_int(value: object, *, label: str) -> int:
        if isinstance(value, bool):
            raise PackageBuildError(f"{label} must be a positive integer")
        try:
            parsed = int(value)  # type: ignore[arg-type]
        except (TypeError, ValueError) as error:
            raise PackageBuildError(f"{label} must be a positive integer") from error
        if parsed <= 0:
            raise PackageBuildError(f"{label} must be a positive integer")
        return parsed

    @staticmethod
    def _nonempty(value: str, *, label: str) -> str:
        normalized = value.strip()
        if not normalized:
            raise PackageBuildError(f"{label} must not be empty")
        return normalized

    @staticmethod
    def _aws_environment(environment: Mapping[str, str]) -> Mapping[str, str]:
        result = {str(key): str(value) for key, value in environment.items()}
        result.pop("GPG_PRIVATE_KEY", None)
        present = {
            name: bool(result.get(name, "").strip()) for name in _CREDENTIAL_ENVIRONMENT
        }
        if present["AWS_ACCESS_KEY_ID"] != present["AWS_SECRET_ACCESS_KEY"]:
            raise PackageBuildError("partial explicit AWS access-key environment is forbidden")
        if present["AWS_SESSION_TOKEN"] and not present["AWS_ACCESS_KEY_ID"]:
            raise PackageBuildError("AWS session token requires an explicit access-key pair")
        return MappingProxyType(result)


__all__ = ["Publisher", "UploadResult"]
