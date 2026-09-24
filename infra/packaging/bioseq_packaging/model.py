"""Immutable records shared by the native packaging pipeline.

Secret bytes are intentionally absent from every record in this module.  The
records that cross the manifest boundary expose explicit ``to_manifest``
methods so adding a dataclass field cannot silently alter the manifest schema.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import StrEnum
import json
from pathlib import Path, PurePosixPath
import re
from types import MappingProxyType
from typing import Any, Mapping


class PackageBuildError(RuntimeError):
    """A fail-closed packaging or publication error."""


class Architecture(StrEnum):
    ARM64 = "arm64"
    X64 = "x64"


class DistroId(StrEnum):
    OPENSUSE_16_0 = "opensuse_16.0"
    FEDORA_44 = "fedora_44"
    DEBIAN_13 = "debian_13"
    UBUNTU_26_04 = "ubuntu_26.04"
    ARCH = "arch"


class PackageFormat(StrEnum):
    RPM = "rpm"
    DEB = "deb"
    PACMAN = "pacman"


class ContainerCapability(StrEnum):
    SYS_NICE = "SYS_NICE"


class ContainerOutput(StrEnum):
    NULL = "null"
    STDOUT = "stdout"
    STDERR = "stderr"


class ContainerPurpose(StrEnum):
    BUILD = "build"
    REPOSITORY = "repository"
    VALIDATION = "validation"
    SMOKE = "smoke"


def _nonempty(value: str, *, label: str) -> str:
    normalized = value.strip()
    if not normalized:
        raise PackageBuildError(f"{label} must not be empty")
    return normalized


@dataclass(frozen=True, slots=True)
class DistroSpec:
    distro_id: DistroId
    package_format: PackageFormat
    images: Mapping[Architecture, str]
    build_dependencies: tuple[str, ...]
    runtime_dependency_pattern: tuple[str, ...]
    capabilities: frozenset[ContainerCapability]
    template_path: Path

    def __post_init__(self) -> None:
        images = {
            Architecture(architecture): _nonempty(image, label="container image")
            for architecture, image in self.images.items()
        }
        unknown_capabilities = set(self.capabilities) - {ContainerCapability.SYS_NICE}
        if unknown_capabilities:
            raise PackageBuildError(
                f"unsupported capabilities for {self.distro_id}: "
                f"{sorted(str(value) for value in unknown_capabilities)}"
            )
        if not self.build_dependencies or any(not value.strip() for value in self.build_dependencies):
            raise PackageBuildError(f"{self.distro_id} has invalid build dependencies")
        if not self.runtime_dependency_pattern or any(
            not value.strip() for value in self.runtime_dependency_pattern
        ):
            raise PackageBuildError(f"{self.distro_id} has invalid runtime dependencies")
        object.__setattr__(self, "images", MappingProxyType(images))

    def image_for(self, architecture: Architecture) -> str:
        image = self.images.get(Architecture(architecture), "").strip()
        if not image:
            raise PackageBuildError(
                f"no native image for {self.distro_id}/{architecture}"
            )
        return image

    def runtime_dependencies(self, postgres_major: int) -> tuple[str, ...]:
        if isinstance(postgres_major, bool) or postgres_major < 17:
            raise PackageBuildError("PostgreSQL majors below 17 are unsupported")
        if postgres_major in (19, 20) and self.distro_id in (DistroId.ARCH, DistroId.OPENSUSE_16_0):
            pin = json.loads((Path(__file__).resolve().parents[1] / 'postgres_sdks.json').read_text())[
                str(postgres_major)]
            equality = '=' if self.distro_id is DistroId.ARCH else ' = '
            runtime = f"bioseq-postgresql{postgres_major}-runtime{equality}{pin['version']}-1"
            return (runtime, self.runtime_dependency_pattern[1])
        return tuple(
            value.format(
                postgres_major=postgres_major,
                arch_postgres_runtime=(
                    "postgresql-old-upgrade" if postgres_major == 17 else "postgresql"
                ),
            )
            for value in self.runtime_dependency_pattern
        )


@dataclass(frozen=True, slots=True)
class WorkerRequest:
    architecture: Architecture
    bucket: str
    region: str
    postgres_majors: tuple[int, ...]
    distro_ids: tuple[DistroId, ...]
    source_dir: Path
    build_root: Path
    manifest_path: Path
    s3_prefix: str
    aws_bin: str
    container_output: ContainerOutput
    fail_fast: bool
    signing_secret_arn: str | None
    source_revision: str
    poetry_lock_sha256: str

    def __post_init__(self) -> None:
        object.__setattr__(self, "architecture", Architecture(self.architecture))
        object.__setattr__(
            self, "container_output", ContainerOutput(self.container_output)
        )
        object.__setattr__(
            self, "distro_ids", tuple(DistroId(value) for value in self.distro_ids)
        )
        if not self.postgres_majors:
            raise PackageBuildError("at least one PostgreSQL major is required")
        if any(isinstance(value, bool) or value < 17 for value in self.postgres_majors):
            raise PackageBuildError("PostgreSQL majors below 17 are unsupported")
        if len(set(self.postgres_majors)) != len(self.postgres_majors):
            raise PackageBuildError("PostgreSQL major selection contains duplicates")
        if not self.distro_ids:
            raise PackageBuildError("at least one distro is required")
        if len(set(self.distro_ids)) != len(self.distro_ids):
            raise PackageBuildError("distro selection contains duplicates")
        if re.fullmatch(r"[0-9a-f]{40}", self.source_revision) is None:
            raise PackageBuildError("source revision must be the full lower-case Git SHA-1")
        if re.fullmatch(r"[0-9a-fA-F]{64}", self.poetry_lock_sha256) is None:
            raise PackageBuildError("Poetry lock SHA-256 must contain 64 hexadecimal digits")
        object.__setattr__(self, "source_revision", self.source_revision.lower())
        object.__setattr__(self, "poetry_lock_sha256", self.poetry_lock_sha256.lower())
        _nonempty(self.bucket, label="S3 bucket")
        _nonempty(self.region, label="AWS region")
        _nonempty(self.s3_prefix, label="S3 prefix")
        _nonempty(self.aws_bin, label="AWS executable")


@dataclass(frozen=True, slots=True)
class CellPaths:
    root: Path
    package_root: Path
    repository_root: Path
    download_root: Path
    toolchain_cache: Path
    cmake_cache: Path


@dataclass(frozen=True, slots=True)
class CellSpec:
    request: WorkerRequest
    distro: DistroSpec
    postgres_major: int
    package_name: str
    package_version: str
    s3_cell_prefix: str


@dataclass(frozen=True, slots=True)
class CellContext:
    request: WorkerRequest
    distro: DistroSpec
    postgres_major: int
    package_name: str
    package_version: str
    s3_cell_prefix: str
    paths: CellPaths

    @classmethod
    def from_spec(cls, spec: CellSpec, paths: CellPaths) -> "CellContext":
        return cls(
            request=spec.request,
            distro=spec.distro,
            postgres_major=spec.postgres_major,
            package_name=spec.package_name,
            package_version=spec.package_version,
            s3_cell_prefix=spec.s3_cell_prefix,
            paths=paths,
        )


@dataclass(frozen=True, slots=True)
class BindMount:
    source: Path
    target: PurePosixPath
    read_only: bool

    def __post_init__(self) -> None:
        if not self.source.is_absolute():
            raise PackageBuildError(f"bind source must be absolute: {self.source}")
        if not self.target.is_absolute() or self.target == PurePosixPath("/"):
            raise PackageBuildError(f"bind target must be an absolute non-root path: {self.target}")


@dataclass(frozen=True, slots=True)
class ContainerPlan:
    image: str
    argv: tuple[str, ...]
    mounts: tuple[BindMount, ...]
    environment_names: tuple[str, ...]
    capabilities: frozenset[ContainerCapability]
    output: ContainerOutput
    name: str = ""
    purpose: ContainerPurpose = ContainerPurpose.BUILD

    def __post_init__(self) -> None:
        object.__setattr__(self, "image", _nonempty(self.image, label="container image"))
        if not self.argv or any(not isinstance(value, str) or not value for value in self.argv):
            raise PackageBuildError("container argv must contain non-empty strings")
        object.__setattr__(self, "output", ContainerOutput(self.output))
        object.__setattr__(self, "purpose", ContainerPurpose(self.purpose))
        targets = [mount.target for mount in self.mounts]
        if len(set(targets)) != len(targets):
            raise PackageBuildError("container plan contains duplicate mount targets")
        invalid_environment = [
            name
            for name in self.environment_names
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) is None
        ]
        if invalid_environment:
            raise PackageBuildError(
                f"container plan has invalid environment names: {invalid_environment}"
            )
        if len(set(self.environment_names)) != len(self.environment_names):
            raise PackageBuildError("container plan contains duplicate environment names")
        if (
            "GPG_PRIVATE_KEY" in self.environment_names
            and self.purpose is not ContainerPurpose.REPOSITORY
        ):
            raise PackageBuildError("the signing key is restricted to repository containers")


@dataclass(frozen=True, slots=True)
class ArtifactRecord:
    path: Path
    filename: str
    size: int
    sha256: str
    format: PackageFormat
    build_path: Path
    build_size: int
    build_sha256: str

    def __post_init__(self) -> None:
        object.__setattr__(self, "format", PackageFormat(self.format))
        if self.size <= 0 or self.build_size <= 0:
            raise PackageBuildError("artifact sizes must be positive")
        for label, value in (("artifact SHA-256", self.sha256), ("build SHA-256", self.build_sha256)):
            if re.fullmatch(r"[0-9a-f]{64}", value) is None:
                raise PackageBuildError(f"{label} is invalid")

    def to_manifest(self) -> dict[str, object]:
        return {
            "path": str(self.path),
            "filename": self.filename,
            "size": self.size,
            "sha256": self.sha256,
            "format": self.format.value,
            "build_path": str(self.build_path),
            "build_size": self.build_size,
            "build_sha256": self.build_sha256,
        }


@dataclass(frozen=True, slots=True)
class PublishedObject:
    key: str
    size: int
    sha256: str
    etag: str
    version_id: str
    server_side_encryption: str

    def __post_init__(self) -> None:
        _nonempty(self.key, label="published object key")
        _nonempty(self.etag, label="published object ETag")
        _nonempty(self.version_id, label="published object VersionId")
        if self.server_side_encryption != "AES256":
            raise PackageBuildError("published objects must use exactly AES256 encryption")
        if self.size <= 0 or re.fullmatch(r"[0-9a-f]{64}", self.sha256) is None:
            raise PackageBuildError("published object evidence is invalid")

    def to_manifest(self) -> dict[str, object]:
        return {
            "key": self.key,
            "size": self.size,
            "sha256": self.sha256,
            "etag": self.etag,
            "version_id": self.version_id,
            "server_side_encryption": self.server_side_encryption,
        }


@dataclass(frozen=True, slots=True)
class SignatureEvidence:
    format: PackageFormat
    signing_key_fingerprint: str
    package_signature: str
    objects: tuple[PublishedObject, ...]
    repository_objects: tuple[PublishedObject, ...] = ()
    passed: bool = True
    public_key_imported: bool = True
    package_signature_verified: bool = True
    repository_signature_verified: bool = True

    def __post_init__(self) -> None:
        object.__setattr__(self, "format", PackageFormat(self.format))
        if re.fullmatch(r"[0-9A-F]{40,64}", self.signing_key_fingerprint) is None:
            raise PackageBuildError("signature fingerprint is invalid")

    def to_manifest(self) -> dict[str, object]:
        result: dict[str, object] = {
            "passed": self.passed,
            "format": self.format.value,
            "public_key_imported": self.public_key_imported,
            "signing_key_fingerprint": self.signing_key_fingerprint,
            "package_signature_verified": self.package_signature_verified,
            "package_signature": self.package_signature,
            "repository_signature_verified": self.repository_signature_verified,
            "objects": [item.to_manifest() for item in self.objects],
        }
        if self.repository_objects:
            result["repository_objects"] = [
                item.to_manifest() for item in self.repository_objects
            ]
        return result


@dataclass(frozen=True, slots=True)
class UploadEvidence:
    published: bool
    bucket: str
    package: PublishedObject
    download_validation: Mapping[str, Any]
    download_smoke: Mapping[str, Any]
    signature_verification: SignatureEvidence

    def to_manifest(self) -> dict[str, object]:
        return {
            "published": self.published,
            "bucket": self.bucket,
            **self.package.to_manifest(),
            "download_validation": dict(self.download_validation),
            "download_smoke": dict(self.download_smoke),
            "signature_verification": self.signature_verification.to_manifest(),
        }
