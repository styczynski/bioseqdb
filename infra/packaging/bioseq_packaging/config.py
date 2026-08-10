"""Strict, project-owned package matrix catalog."""

from __future__ import annotations

from collections.abc import Iterable
from pathlib import Path
import platform
import re
from types import MappingProxyType

from .model import (
    Architecture,
    CellSpec,
    ContainerCapability,
    DistroId,
    DistroSpec,
    PackageBuildError,
    PackageFormat,
    WorkerRequest,
)


PACKAGE_VERSION = "4.2.3"
MANIFEST_SCHEMA_REVISION = 1
PACKAGE_NAME_PREFIX = "bioseq-postgresql"
PACKAGE_MAINTAINER = "Bioseq Packaging <packages@bioseq.dev>"
PACKAGE_HOMEPAGE = "https://github.com/bioseq/bioseq"
PACKAGE_DESCRIPTION = "Bioseq and bioseq_cache extensions for PostgreSQL {postgres_major}"
PINNED_SIGNING_FINGERPRINT = "452F34C5F1EF6D471EC041ADDD94780B0728F093"
MINIMUM_POSTGRES_MAJOR = 17
PACKAGING_ROOT = Path(__file__).resolve().parents[1]

SOURCE_PROJECTION_EXCLUDES = (
    "/.git/",
    "/build/",
    "/DOCS/",
    "/main/papers/",
    "/website/",
    "__pycache__/",
    "*.pyc",
    "report_build_*.json",
)


def _spec(
    distro_id: DistroId,
    package_format: PackageFormat,
    *,
    image: str,
    arm64_image: str | None = None,
    build_dependencies: tuple[str, ...],
    runtime_dependencies: tuple[str, ...],
    template: str,
) -> DistroSpec:
    return DistroSpec(
        distro_id=distro_id,
        package_format=package_format,
        images={
            Architecture.ARM64: arm64_image or image,
            Architecture.X64: image,
        },
        build_dependencies=build_dependencies,
        runtime_dependency_pattern=runtime_dependencies,
        capabilities=frozenset({ContainerCapability.SYS_NICE}),
        template_path=PACKAGING_ROOT / template,
    )


_RPM_BUILD_DEPENDENCIES = (
    "bash", "ca-certificates", "curl", "git", "gzip", "python3", "tar", "xz"
)
_DEB_BUILD_DEPENDENCIES = (
    "bash", "ca-certificates", "curl", "git", "gnupg", "python3", "tar", "xz-utils"
)
_PACMAN_BUILD_DEPENDENCIES = (
    "bash", "ca-certificates", "curl", "git", "gzip", "python", "tar", "xz"
)

DISTRO_CATALOG = MappingProxyType(
    {
        DistroId.OPENSUSE_16_0: _spec(
            DistroId.OPENSUSE_16_0,
            PackageFormat.RPM,
            image="opensuse/leap:16.0",
            build_dependencies=_RPM_BUILD_DEPENDENCIES,
            runtime_dependencies=("postgresql{postgres_major}-server", "libvulkan1"),
            template="rpm/bioseq.spec.liquid",
        ),
        DistroId.FEDORA_44: _spec(
            DistroId.FEDORA_44,
            PackageFormat.RPM,
            image="fedora:44",
            build_dependencies=_RPM_BUILD_DEPENDENCIES,
            runtime_dependencies=("postgresql{postgres_major}-server", "vulkan-loader"),
            template="rpm/bioseq.spec.liquid",
        ),
        DistroId.DEBIAN_13: _spec(
            DistroId.DEBIAN_13,
            PackageFormat.DEB,
            image="debian:trixie",
            build_dependencies=_DEB_BUILD_DEPENDENCIES,
            runtime_dependencies=("postgresql-{postgres_major}", "libvulkan1"),
            template="deb",
        ),
        DistroId.UBUNTU_26_04: _spec(
            DistroId.UBUNTU_26_04,
            PackageFormat.DEB,
            image="ubuntu:26.04",
            build_dependencies=_DEB_BUILD_DEPENDENCIES,
            runtime_dependencies=("postgresql-{postgres_major}", "libvulkan1"),
            template="deb",
        ),
        DistroId.ARCH: _spec(
            DistroId.ARCH,
            PackageFormat.PACMAN,
            image="archlinux:latest",
            arm64_image="menci/archlinuxarm:base",
            build_dependencies=_PACMAN_BUILD_DEPENDENCIES,
            runtime_dependencies=("{arch_postgres_runtime}", "vulkan-icd-loader"),
            template="pacman/PKGBUILD.liquid",
        ),
    }
)

SUPPORTED_DISTROS = tuple(value.value for value in DistroId)


def csv_values(values: Iterable[str], *, label: str) -> list[str]:
    parsed: list[str] = []
    for raw in values:
        for value in raw.split(","):
            normalized = value.strip()
            if normalized and normalized not in parsed:
                parsed.append(normalized)
    if not parsed:
        raise PackageBuildError(f"at least one {label} value is required")
    return parsed


def parse_postgres(values: Iterable[str]) -> tuple[int, ...]:
    parsed: list[int] = []
    for value in csv_values(values, label="PostgreSQL"):
        if re.fullmatch(r"\d+", value) is None:
            raise PackageBuildError(f"invalid PostgreSQL major {value!r}")
        major = int(value)
        if major < MINIMUM_POSTGRES_MAJOR:
            raise PackageBuildError("PostgreSQL majors below 17 are unsupported")
        if major not in parsed:
            parsed.append(major)
    return tuple(parsed)


def parse_distros(values: Iterable[str]) -> tuple[DistroId, ...]:
    parsed: list[DistroId] = []
    unknown: list[str] = []
    for value in csv_values(values, label="distro"):
        try:
            distro_id = DistroId(value)
        except ValueError:
            unknown.append(value)
            continue
        if distro_id not in DISTRO_CATALOG:
            unknown.append(value)
        elif distro_id not in parsed:
            parsed.append(distro_id)
    if unknown:
        raise PackageBuildError(
            f"unsupported distro(s): {', '.join(unknown)}; supported: "
            f"{', '.join(SUPPORTED_DISTROS)}"
        )
    if not parsed:
        raise PackageBuildError("at least one configured distro is required")
    return tuple(parsed)


def native_architecture() -> Architecture:
    machine = platform.machine().lower()
    if machine in {"x86_64", "amd64"}:
        return Architecture.X64
    if machine in {"aarch64", "arm64"}:
        return Architecture.ARM64
    raise PackageBuildError(f"unsupported native machine architecture {machine!r}")


def validate_bucket(bucket: str) -> str:
    if re.fullmatch(r"[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]", bucket) is None:
        raise PackageBuildError(f"invalid S3 bucket name {bucket!r}")
    return bucket


def validate_prefix(prefix: str) -> str:
    value = prefix.strip("/")
    if (
        not value
        or "//" in value
        or any(part in {"", ".", ".."} for part in value.split("/"))
        or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._/-]*", value) is None
    ):
        raise PackageBuildError(f"invalid S3 prefix {prefix!r}")
    return value


def distro_spec(distro_id: DistroId | str) -> DistroSpec:
    try:
        normalized = DistroId(distro_id)
        return DISTRO_CATALOG[normalized]
    except (ValueError, KeyError) as error:
        raise PackageBuildError(f"unsupported or unconfigured distro {distro_id!r}") from error


def runtime_dependencies(distro_id: DistroId | str, postgres_major: int) -> tuple[str, ...]:
    return distro_spec(distro_id).runtime_dependencies(postgres_major)


def build_cells(request: WorkerRequest) -> tuple[CellSpec, ...]:
    prefix = validate_prefix(request.s3_prefix)
    cells: list[CellSpec] = []
    for postgres_major in request.postgres_majors:
        if postgres_major < MINIMUM_POSTGRES_MAJOR:
            raise PackageBuildError("PostgreSQL majors below 17 are unsupported")
        for distro_id in request.distro_ids:
            spec = distro_spec(distro_id)
            cell_prefix = (
                f"{prefix}/{request.architecture.value}/postgresql{postgres_major}/"
                f"{spec.distro_id.value}/"
            )
            if spec.package_format is PackageFormat.DEB:
                cell_prefix += "stable/"
            cells.append(
                CellSpec(
                    request=request,
                    distro=spec,
                    postgres_major=postgres_major,
                    package_name=f"{PACKAGE_NAME_PREFIX}{postgres_major}",
                    package_version=PACKAGE_VERSION,
                    s3_cell_prefix=cell_prefix,
                )
            )
    expected = len(request.postgres_majors) * len(request.distro_ids)
    if len(cells) != expected:
        raise PackageBuildError("requested matrix was silently reduced")
    return tuple(cells)
