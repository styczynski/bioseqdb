"""Built and exact-version-downloaded package acceptance.

The retained validator inspects package metadata and extracted ELF files.  The
retained smoke driver starts a fresh matching native distro container and runs
the PostgreSQL preload/extension/round-trip contract.  Neither child receives
package-signing material.  Published signature verification consumes only the
exact-version files downloaded and attested by :mod:`publishing`.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import tarfile
from typing import Any

from .commands import Command, CommandRunner
from .config import PACKAGE_VERSION, PINNED_SIGNING_FINGERPRINT
from .model import (
    Architecture,
    ArtifactRecord,
    CellContext,
    PackageBuildError,
    PackageFormat,
    PublishedObject,
    SignatureEvidence,
)
from .signing import require_pinned_fingerprint, verify_agent_free


_PACKAGE_SUFFIXES = {
    PackageFormat.RPM: ".rpm",
    PackageFormat.DEB: ".deb",
    PackageFormat.PACMAN: ".pkg.tar.zst",
}
_PACKAGE_SIGNATURE_KINDS = {
    PackageFormat.RPM: "embedded-rpm-openpgp",
    PackageFormat.DEB: "signed-deb-release-sha256",
    PackageFormat.PACMAN: "detached-pacman-openpgp",
}
_REQUIRED_TRUE_SMOKE = (
    "passed",
    "package_installed",
    "preload_active",
    "bioseq_extension_created",
    "bioseq_cache_extension_created",
    "nonempty_roundtrip",
    "fresh_container",
)
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _artifact_format(path: Path) -> PackageFormat:
    for package_format, suffix in _PACKAGE_SUFFIXES.items():
        if path.name.endswith(suffix):
            return package_format
    raise PackageBuildError(f"unsupported package artifact: {path}")


def _checked_artifact(path: Path, expected_format: PackageFormat) -> Path:
    if path.is_symlink():
        raise PackageBuildError(f"package artifact may not be a symlink: {path}")
    resolved = path.expanduser().resolve()
    if not resolved.is_file() or resolved.stat().st_size <= 0:
        raise PackageBuildError(f"package artifact is missing or empty: {resolved}")
    actual_format = _artifact_format(resolved)
    if actual_format is not expected_format:
        raise PackageBuildError(
            f"package format {actual_format.value} does not match "
            f"{expected_format.value}"
        )
    return resolved


def _json_mapping(output: str, *, operation: str) -> dict[str, Any]:
    try:
        result = json.loads(output)
    except json.JSONDecodeError as error:
        raise PackageBuildError(
            f"{operation} emitted invalid JSON: {output[-1000:]}"
        ) from error
    if not isinstance(result, dict):
        raise PackageBuildError(f"{operation} must emit one JSON object")
    return result


def _expected_package_architecture(
    package_format: PackageFormat, architecture: Architecture
) -> str:
    try:
        return {
            (PackageFormat.DEB, Architecture.X64): "amd64",
            (PackageFormat.DEB, Architecture.ARM64): "arm64",
            (PackageFormat.RPM, Architecture.X64): "x86_64",
            (PackageFormat.RPM, Architecture.ARM64): "aarch64",
            (PackageFormat.PACMAN, Architecture.X64): "x86_64",
            (PackageFormat.PACMAN, Architecture.ARM64): "aarch64",
        }[(package_format, architecture)]
    except KeyError as error:
        raise PackageBuildError(
            f"unsupported package architecture {package_format}/{architecture}"
        ) from error


def _canonical_elf_architecture(machine: str) -> str:
    if "AArch64" in machine:
        return "aarch64"
    if "X86-64" in machine or "Advanced Micro Devices X86-64" in machine:
        return "x86_64"
    raise PackageBuildError(f"unknown ELF architecture attestation {machine!r}")


def _parse_validation(
    output: str,
    *,
    cell: CellContext,
) -> dict[str, Any]:
    result = _json_mapping(output, operation="package validator")
    expected_runtime = cell.distro.runtime_dependencies(cell.postgres_major)
    if len(expected_runtime) != 2:
        raise PackageBuildError(
            f"{cell.distro.distro_id.value} must declare PostgreSQL and Vulkan runtime dependencies"
        )
    required = {
        "passed": True,
        "six_artifacts": True,
        "format": cell.distro.package_format.value,
        "package_name": cell.package_name,
        "postgres_major": cell.postgres_major,
        "postgres_runtime_dependency": expected_runtime[0],
        "vulkan_runtime_dependency": expected_runtime[1],
        "optimization_level": 0,
        "package_architecture": _expected_package_architecture(
            cell.distro.package_format, cell.request.architecture
        ),
    }
    mismatches = {
        name: (result.get(name), value)
        for name, value in required.items()
        if result.get(name) != value
    }
    if mismatches:
        raise PackageBuildError(
            f"package validator attested the wrong cell fields: {mismatches}"
        )
    version = result.get("package_version")
    if not isinstance(version, str) or not version.startswith(cell.package_version):
        raise PackageBuildError(
            f"package validator attested unexpected version {version!r}"
        )

    binaries = result.get("binaries")
    if (
        not isinstance(binaries, list)
        or len(binaries) != 2
        or not all(isinstance(binary, dict) for binary in binaries)
    ):
        raise PackageBuildError("package validator must attest exactly two ELF DSOs")
    names = {Path(str(binary.get("path", ""))).name for binary in binaries}
    if names != {"bioseq.so", "bioseq_cache.so"}:
        raise PackageBuildError(
            f"package validator attested unexpected DSOs: {sorted(names)}"
        )
    machines = {
        _canonical_elf_architecture(str(binary.get("elf_architecture", "")))
        for binary in binaries
    }
    expected_elf = {
        Architecture.ARM64: "aarch64",
        Architecture.X64: "x86_64",
    }[cell.request.architecture]
    if machines != {expected_elf}:
        raise PackageBuildError(
            f"package validator attested ELF architecture {sorted(machines)}, "
            f"expected {expected_elf}"
        )
    if any(
        binary.get("rpath_absent") is not True
        or binary.get("compiler_runtime_safe") is not True
        for binary in binaries
    ):
        raise PackageBuildError("package validator did not prove portable ELF linkage")
    result["elf_architecture"] = expected_elf
    result["rpath_absent"] = True
    result["compiler_runtime_safe"] = True
    return result


def _parse_smoke(output: str, *, cell: CellContext) -> dict[str, Any]:
    result = _json_mapping(output, operation="installed-package smoke")
    if any(result.get(name) is not True for name in _REQUIRED_TRUE_SMOKE):
        raise PackageBuildError(
            f"installed-package PostgreSQL smoke failed assertions: {result}"
        )
    expected_image = cell.distro.image_for(cell.request.architecture)
    expected = {
        "distro": cell.distro.distro_id.value,
        "architecture": cell.request.architecture.value,
        "postgres_major": cell.postgres_major,
        "roundtrip": "ACGT",
        "container_image": expected_image,
    }
    mismatches = {
        name: (result.get(name), value)
        for name, value in expected.items()
        if result.get(name) != value
    }
    if mismatches:
        raise PackageBuildError(
            f"installed-package smoke attested the wrong cell: {mismatches}"
        )
    return result


class AcceptanceService:
    """Validate and smoke built and downloaded package artifacts."""

    def __init__(
        self,
        runner: CommandRunner,
        *,
        environment: Mapping[str, str] | None = None,
    ) -> None:
        self.runner = runner
        self.environment = dict(environment or {})
        # Acceptance is a public-key-only boundary.  Even an accidentally
        # inherited local development key is excluded from every child.
        self.environment.pop("GPG_PRIVATE_KEY", None)

    def _validation_command(self, cell: CellContext, artifact: Path) -> Command:
        script = (
            cell.request.source_dir
            / "infra"
            / "packaging"
            / "scripts"
            / "validate_package.py"
        )
        if not script.is_file():
            raise PackageBuildError(f"package validator script is missing: {script}")
        runtime_dependencies = cell.distro.runtime_dependencies(cell.postgres_major)
        if len(runtime_dependencies) != 2:
            raise PackageBuildError("package catalog must provide exactly two runtime dependencies")
        return Command(
            argv=(
                sys.executable,
                str(script),
                "--artifact",
                str(artifact),
                "--architecture",
                cell.request.architecture.value,
                "--postgres-major",
                str(cell.postgres_major),
                "--postgres-runtime-package",
                runtime_dependencies[0],
                "--vulkan-runtime-package",
                runtime_dependencies[1],
            )
        )

    def _validate(self, cell: CellContext, artifact: Path) -> dict[str, Any]:
        output = self.runner.run_captured(
            self._validation_command(cell, artifact),
            environment=self.environment,
        )
        return _parse_validation(output, cell=cell)

    def validate_built(
        self, cell: CellContext, artifact: Path
    ) -> tuple[ArtifactRecord, dict[str, Any]]:
        resolved = _checked_artifact(artifact, cell.distro.package_format)
        validation = self._validate(cell, resolved)
        size = resolved.stat().st_size
        digest = _sha256(resolved)
        return (
            ArtifactRecord(
                path=resolved,
                filename=resolved.name,
                size=size,
                sha256=digest,
                format=cell.distro.package_format,
                build_path=resolved,
                build_size=size,
                build_sha256=digest,
            ),
            validation,
        )

    def validate_download(
        self,
        cell: CellContext,
        built: ArtifactRecord,
        downloaded: Path,
    ) -> tuple[ArtifactRecord, dict[str, Any]]:
        if built.format is not cell.distro.package_format:
            raise PackageBuildError("built artifact format differs from the matrix cell")
        resolved = _checked_artifact(downloaded, cell.distro.package_format)
        if resolved.name != built.filename:
            raise PackageBuildError(
                "downloaded package filename differs from the built artifact"
            )
        validation = self._validate(cell, resolved)
        size = resolved.stat().st_size
        digest = _sha256(resolved)
        if cell.distro.package_format is not PackageFormat.RPM and (
            size != built.build_size or digest != built.build_sha256
        ):
            raise PackageBuildError(
                "published DEB/PACMAN package bytes differ from the validated build artifact"
            )
        return (
            ArtifactRecord(
                path=resolved,
                filename=resolved.name,
                size=size,
                sha256=digest,
                format=cell.distro.package_format,
                build_path=built.build_path,
                build_size=built.build_size,
                build_sha256=built.build_sha256,
            ),
            validation,
        )

    def smoke(self, cell: CellContext, artifact: Path) -> dict[str, Any]:
        resolved = _checked_artifact(artifact, cell.distro.package_format)
        script = (
            cell.request.source_dir
            / "infra"
            / "packaging"
            / "scripts"
            / "smoke_test_package.py"
        )
        if not script.is_file():
            raise PackageBuildError(f"installed-package smoke script is missing: {script}")
        command = Command(
            argv=(
                sys.executable,
                str(script),
                "--artifact",
                str(resolved),
                "--architecture",
                cell.request.architecture.value,
                "--postgres-major",
                str(cell.postgres_major),
                "--distro",
                cell.distro.distro_id.value,
                "--arch-image",
                cell.distro.image_for(cell.request.architecture),
            )
        )
        output = self.runner.run_captured(command, environment=self.environment)
        return _parse_smoke(output, cell=cell)


def _validate_downloaded_object(
    published: PublishedObject,
    downloaded_objects: Mapping[str, Path],
) -> Path:
    try:
        raw_path = downloaded_objects[published.key]
    except KeyError as error:
        raise PackageBuildError(
            f"exact-version downloaded object is missing for {published.key}"
        ) from error
    path = Path(raw_path)
    if path.is_symlink():
        raise PackageBuildError(f"downloaded repository object may not be a symlink: {path}")
    resolved = path.resolve()
    if not resolved.is_file() or resolved.stat().st_size != published.size:
        raise PackageBuildError(
            f"downloaded repository object size differs for {published.key}"
        )
    digest = _sha256(resolved)
    if digest != published.sha256:
        raise PackageBuildError(
            f"downloaded repository object SHA-256 differs for {published.key}"
        )
    if published.server_side_encryption != "AES256" or not published.version_id:
        raise PackageBuildError(
            f"downloaded repository object lacks exact-version AES256 evidence: {published.key}"
        )
    return resolved


def _objects_by_key(
    objects: Sequence[PublishedObject], *, label: str
) -> dict[str, PublishedObject]:
    result: dict[str, PublishedObject] = {}
    for item in objects:
        if item.key in result:
            raise PackageBuildError(f"{label} contains duplicate key {item.key}")
        result[item.key] = item
    return result


def _required_signature_keys(
    package_format: PackageFormat,
    *,
    package_name: str,
    package_key: str,
) -> tuple[str, ...]:
    base = package_key.rsplit("/", 1)[0]
    if package_format is PackageFormat.RPM:
        return (
            f"{base}/repodata/repomd.xml.key",
            f"{base}/repodata/repomd.xml",
            f"{base}/repodata/repomd.xml.asc",
        )
    if package_format is PackageFormat.DEB:
        return (
            f"{base}/Release.key",
            f"{base}/Release",
            f"{base}/Release.gpg",
            f"{base}/InRelease",
        )
    if package_format is PackageFormat.PACMAN:
        database = f"{base}/{package_name}.db.tar.gz"
        return (
            f"{base}/public.key",
            f"{package_key}.sig",
            database,
            f"{database}.sig",
        )
    raise PackageBuildError(f"unsupported signature format {package_format}")


def _verify_deb_release(release: Path, artifact: ArtifactRecord) -> None:
    entries: dict[str, tuple[str, int]] = {}
    in_sha256 = False
    try:
        lines = release.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise PackageBuildError("downloaded APT Release is unreadable") from error
    for line in lines:
        if line == "SHA256:":
            in_sha256 = True
            continue
        if in_sha256 and not line.startswith(" "):
            break
        if not in_sha256:
            continue
        fields = line.split()
        if len(fields) != 3 or _SHA256.fullmatch(fields[0]) is None or not fields[1].isdigit():
            raise PackageBuildError(f"malformed APT Release SHA256 entry: {line!r}")
        if fields[2] in entries:
            raise PackageBuildError(f"duplicate APT Release SHA256 entry {fields[2]}")
        entries[fields[2]] = (fields[0], int(fields[1]))
    if entries.get(artifact.filename) != (artifact.sha256, artifact.size):
        raise PackageBuildError(
            "signed APT Release does not bind the exact downloaded package bytes"
        )


def _pacman_database_fields(database: Path) -> dict[str, tuple[str, ...]]:
    try:
        with tarfile.open(database, "r:*") as archive:
            descriptions = [
                member
                for member in archive.getmembers()
                if member.isfile() and PurePosixPath(member.name).name == "desc"
            ]
            if len(descriptions) != 1:
                raise PackageBuildError(
                    "downloaded PACMAN database must contain one package description"
                )
            handle = archive.extractfile(descriptions[0])
            if handle is None:
                raise PackageBuildError("downloaded PACMAN database description is unreadable")
            payload = handle.read().decode("utf-8")
    except PackageBuildError:
        raise
    except (OSError, tarfile.TarError, UnicodeError) as error:
        raise PackageBuildError("downloaded PACMAN database is malformed") from error

    fields: dict[str, tuple[str, ...]] = {}
    lines = payload.splitlines()
    index = 0
    while index < len(lines):
        marker = lines[index]
        index += 1
        if not marker:
            continue
        if not (marker.startswith("%") and marker.endswith("%") and len(marker) > 2):
            raise PackageBuildError("downloaded PACMAN database has malformed fields")
        name = marker[1:-1]
        values: list[str] = []
        while index < len(lines) and lines[index]:
            values.append(lines[index])
            index += 1
        if name in fields or not values:
            raise PackageBuildError("downloaded PACMAN database has duplicate or empty fields")
        fields[name] = tuple(values)
    return fields


def _single_pacman_field(fields: Mapping[str, tuple[str, ...]], name: str) -> str:
    values = fields.get(name, ())
    if len(values) != 1:
        raise PackageBuildError(
            f"downloaded PACMAN database field {name} must have one value"
        )
    return values[0]


def _verify_pacman_database(
    database: Path,
    *,
    artifact: ArtifactRecord,
    package_name: str,
    architecture: Architecture,
) -> None:
    fields = _pacman_database_fields(database)
    expected = {
        "NAME": package_name,
        "FILENAME": artifact.filename,
        "VERSION": f"{PACKAGE_VERSION}-1",
        "ARCH": {
            Architecture.X64: "x86_64",
            Architecture.ARM64: "aarch64",
        }[architecture],
        "CSIZE": str(artifact.size),
        "SHA256SUM": artifact.sha256,
    }
    for name, value in expected.items():
        if _single_pacman_field(fields, name) != value:
            raise PackageBuildError(
                f"signed PACMAN database does not bind exact package field {name}"
            )


def _verify_equal_downloads(left: Path, right: Path, *, label: str) -> None:
    if left.stat().st_size != right.stat().st_size or _sha256(left) != _sha256(right):
        raise PackageBuildError(f"downloaded PACMAN alias differs from {label}")


def _verify_rpm_signature(
    *,
    runner: CommandRunner,
    public_key: Path,
    artifact: Path,
    work_dir: Path,
    environment: Mapping[str, str],
) -> None:
    rpm = shutil.which("rpm", path=environment.get("PATH"))
    rpmkeys = shutil.which("rpmkeys", path=environment.get("PATH"))
    if not rpm or not rpmkeys:
        raise PackageBuildError(
            "rpm and rpmkeys are required to verify the downloaded RPM signature"
        )
    rpm_database = work_dir / "rpmdb"
    if rpm_database.exists():
        raise PackageBuildError("RPM verification database already exists")
    rpm_database.mkdir(parents=True)
    runner.run_captured(
        Command((rpm, "--dbpath", str(rpm_database), "--initdb")),
        environment=environment,
    )
    runner.run_captured(
        Command((rpmkeys, "--dbpath", str(rpm_database), "--import", str(public_key))),
        environment=environment,
    )
    output = runner.run_captured(
        Command((rpmkeys, "--dbpath", str(rpm_database), "--checksig", str(artifact))),
        environment=environment,
    )
    if "NOT OK" in output or re.search(r"\bOK\b", output) is None:
        raise PackageBuildError(
            f"downloaded RPM embedded signature verification failed: {output.strip()}"
        )


def verify_published_signatures(
    *,
    runner: CommandRunner,
    package_format: PackageFormat,
    artifact: ArtifactRecord,
    package_name: str,
    architecture: Architecture,
    expected_signing_fingerprint: str,
    published_package: PublishedObject,
    signature_objects: Sequence[PublishedObject],
    downloaded_objects: Mapping[str, Path],
    work_dir: Path,
    repository_objects: Sequence[PublishedObject] = (),
    environment: Mapping[str, str] | None = None,
) -> SignatureEvidence:
    """Verify exact-version downloaded package and repository signatures."""

    normalized_format = PackageFormat(package_format)
    normalized_architecture = Architecture(architecture)
    fingerprint = require_pinned_fingerprint(expected_signing_fingerprint)
    if fingerprint != PINNED_SIGNING_FINGERPRINT:
        raise PackageBuildError("published signing fingerprint differs from the pinned key")
    if artifact.format is not normalized_format:
        raise PackageBuildError("downloaded artifact format differs from signature format")
    public_environment = dict(environment or {})
    public_environment.pop("GPG_PRIVATE_KEY", None)

    package_path = _validate_downloaded_object(published_package, downloaded_objects)
    if (
        package_path != artifact.path.resolve()
        or published_package.size != artifact.size
        or published_package.sha256 != artifact.sha256
        or published_package.key.rsplit("/", 1)[-1] != artifact.filename
    ):
        raise PackageBuildError(
            "published package evidence differs from the accepted downloaded artifact"
        )

    required_by_key = _objects_by_key(signature_objects, label="signature evidence")
    expected_keys = _required_signature_keys(
        normalized_format,
        package_name=package_name,
        package_key=published_package.key,
    )
    if set(required_by_key) != set(expected_keys):
        raise PackageBuildError(
            "published signature-object keys differ from the controller-required subset"
        )
    required_paths = {
        key: _validate_downloaded_object(required_by_key[key], downloaded_objects)
        for key in expected_keys
    }
    remaining_by_key = _objects_by_key(repository_objects, label="repository evidence")
    if set(remaining_by_key) & set(required_by_key):
        raise PackageBuildError("repository evidence duplicates required signature objects")
    remaining_paths = {
        key: _validate_downloaded_object(item, downloaded_objects)
        for key, item in remaining_by_key.items()
    }

    verification_root = Path(work_dir).expanduser().resolve(strict=False)
    if verification_root.is_symlink():
        raise PackageBuildError("signature verification root may not be a symlink")
    verification_root.mkdir(parents=True, exist_ok=True)
    if not verification_root.is_dir():
        raise PackageBuildError("signature verification root is not a directory")

    base = published_package.key.rsplit("/", 1)[0]
    if normalized_format is PackageFormat.RPM:
        public_key = required_paths[f"{base}/repodata/repomd.xml.key"]
        repomd = required_paths[f"{base}/repodata/repomd.xml"]
        repomd_signature = required_paths[f"{base}/repodata/repomd.xml.asc"]
        actual = verify_agent_free(
            public_key=public_key,
            signatures=((repomd_signature, repomd),),
            expected_fingerprint=fingerprint,
            environment=public_environment,
            runner=runner,
        )
        _verify_rpm_signature(
            runner=runner,
            public_key=public_key,
            artifact=package_path,
            work_dir=verification_root,
            environment=public_environment,
        )
    elif normalized_format is PackageFormat.DEB:
        public_key = required_paths[f"{base}/Release.key"]
        release = required_paths[f"{base}/Release"]
        release_signature = required_paths[f"{base}/Release.gpg"]
        inrelease = required_paths[f"{base}/InRelease"]
        actual = verify_agent_free(
            public_key=public_key,
            signatures=((release_signature, release), (inrelease, None)),
            expected_fingerprint=fingerprint,
            environment=public_environment,
            runner=runner,
        )
        _verify_deb_release(release, artifact)
        expected_remaining = {f"{base}/Packages", f"{base}/Packages.gz"}
        if set(remaining_by_key) != expected_remaining:
            raise PackageBuildError(
                "APT repository evidence must contain exactly Packages and Packages.gz"
            )
    elif normalized_format is PackageFormat.PACMAN:
        public_key = required_paths[f"{base}/public.key"]
        package_signature = required_paths[f"{published_package.key}.sig"]
        database_key = f"{base}/{package_name}.db.tar.gz"
        database = required_paths[database_key]
        database_signature = required_paths[f"{database_key}.sig"]
        files_key = f"{base}/{package_name}.files.tar.gz"
        files_signature_key = f"{files_key}.sig"
        expected_remaining = {
            files_key,
            files_signature_key,
            f"{base}/{package_name}.db",
            f"{base}/{package_name}.db.sig",
            f"{base}/{package_name}.files",
            f"{base}/{package_name}.files.sig",
        }
        if set(remaining_by_key) != expected_remaining:
            raise PackageBuildError(
                "PACMAN repository evidence has an incomplete canonical/alias set"
            )
        files = remaining_paths[files_key]
        files_signature = remaining_paths[files_signature_key]
        actual = verify_agent_free(
            public_key=public_key,
            signatures=(
                (package_signature, package_path),
                (database_signature, database),
                (files_signature, files),
            ),
            expected_fingerprint=fingerprint,
            environment=public_environment,
            runner=runner,
        )
        _verify_pacman_database(
            database,
            artifact=artifact,
            package_name=package_name,
            architecture=normalized_architecture,
        )
        _verify_equal_downloads(
            database,
            remaining_paths[f"{base}/{package_name}.db"],
            label="the canonical PACMAN database",
        )
        _verify_equal_downloads(
            database_signature,
            remaining_paths[f"{base}/{package_name}.db.sig"],
            label="the canonical PACMAN database signature",
        )
        _verify_equal_downloads(
            files,
            remaining_paths[f"{base}/{package_name}.files"],
            label="the canonical PACMAN files database",
        )
        _verify_equal_downloads(
            files_signature,
            remaining_paths[f"{base}/{package_name}.files.sig"],
            label="the canonical PACMAN files signature",
        )
    else:  # pragma: no cover - PackageFormat is closed, retain fail-closed behavior.
        raise PackageBuildError(f"unsupported signature format {normalized_format}")

    if actual != fingerprint:
        raise PackageBuildError(
            "published repository verification returned an unexpected fingerprint"
        )
    return SignatureEvidence(
        format=normalized_format,
        signing_key_fingerprint=fingerprint,
        package_signature=_PACKAGE_SIGNATURE_KINDS[normalized_format],
        objects=tuple(signature_objects),
        repository_objects=tuple(repository_objects),
    )


__all__ = ("AcceptanceService", "verify_published_signatures")

