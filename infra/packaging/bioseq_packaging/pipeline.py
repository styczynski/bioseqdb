"""Native package worker orchestration.

This module owns the current cell state machine.  Format backends construct
packages and repositories; the acceptance, signing, and publication services
produce the evidence that permits each manifest transition.
"""

from __future__ import annotations

import argparse
from collections.abc import Mapping
from dataclasses import dataclass
import os
from pathlib import Path
import platform
import re
from typing import Any, Callable

from .acceptance import AcceptanceService, verify_published_signatures
from .artifacts import sha256_file
from .commands import CommandRunner, Redactor
from .config import (
    MANIFEST_SCHEMA_REVISION,
    PACKAGE_VERSION,
    build_cells,
    native_architecture,
    parse_distros,
    parse_postgres,
    validate_bucket,
    validate_prefix,
)
from .containers import ContainerRunner
from .manifest import ManifestStore, utc_now, write_manifest
from .model import (
    Architecture,
    CellContext,
    CellSpec,
    ContainerOutput,
    PackageBuildError,
    PackageFormat,
    UploadEvidence,
    WorkerRequest,
)
from .packagers import deb as deb_packager
from .packagers import pacman as pacman_packager
from .packagers import rpm as rpm_packager
from .publishing import Publisher
from .repositories import RepositoryBuild
from .repositories import deb as deb_repository
from .repositories import pacman as pacman_repository
from .repositories import rpm as rpm_repository
from .signing import SigningMaterial, SigningProvider
from .templates import TemplateRenderer
from .workspace import WorkspaceManager


_POETRY_VERSION = re.compile(r"1\.8\.\d+\Z")


class PackagePipelineError(PackageBuildError):
    """A redacted, actionable native-package pipeline failure."""


@dataclass(frozen=True, slots=True)
class _Services:
    runner: CommandRunner
    containers: ContainerRunner
    templates: TemplateRenderer
    workspace: WorkspaceManager
    acceptance: AcceptanceService
    publisher: Publisher
    environment: Mapping[str, str]


def _python_environment() -> dict[str, str]:
    return {
        "python": platform.python_version(),
        "poetry": os.environ.get("BIOSEQ_POETRY_VERSION", "").strip(),
    }


def _worker_request(
    arguments: argparse.Namespace,
    *,
    manifest_path: Path,
) -> WorkerRequest:
    source_dir = Path(arguments.source_dir).expanduser().resolve()
    build_root = Path(arguments.build_root).expanduser().resolve()
    return WorkerRequest(
        architecture=Architecture(arguments.architecture),
        bucket=validate_bucket(str(arguments.bucket)),
        region=str(arguments.region).strip(),
        postgres_majors=parse_postgres(arguments.postgres),
        distro_ids=parse_distros(arguments.distros),
        source_dir=source_dir,
        build_root=build_root,
        manifest_path=Path(manifest_path).expanduser().resolve(),
        s3_prefix=validate_prefix(str(arguments.s3_prefix)),
        aws_bin=str(arguments.aws_bin).strip(),
        container_output=ContainerOutput(arguments.container_output),
        fail_fast=bool(arguments.fail_fast),
        signing_secret_arn=(
            str(arguments.signing_secret_arn).strip()
            if arguments.signing_secret_arn
            else None
        ),
        source_revision=str(arguments.source_revision),
        poetry_lock_sha256=str(arguments.poetry_lock_sha256),
    )


def _validate_provenance(
    request: WorkerRequest,
    python_environment: dict[str, str],
) -> None:
    source_dir = request.source_dir
    if (
        not source_dir.is_dir()
        or not (source_dir / "CMakeLists.txt").is_file()
    ):
        raise PackageBuildError(
            f"--source-dir must name the BioSeq repository source root: {source_dir}"
        )

    sealed_papers = source_dir / "main" / "papers"
    if sealed_papers.exists() or sealed_papers.is_symlink():
        raise PackageBuildError(
            "sealed papers must be absent from the committed package source archive"
        )

    revision_file = source_dir / ".bioseq-source-revision"
    if revision_file.is_symlink() or not revision_file.is_file():
        raise PackageBuildError(
            f"committed source revision marker is missing: {revision_file}"
        )
    try:
        transferred_revision = revision_file.read_text(encoding="ascii").strip()
    except (OSError, UnicodeError) as error:
        raise PackageBuildError("committed source revision marker is unreadable") from error
    if transferred_revision != request.source_revision:
        raise PackageBuildError(
            "transferred source revision differs from --source-revision"
        )

    packaging_dir = source_dir / "infra" / "packaging"
    lock_file = packaging_dir / "poetry.lock"
    project_file = packaging_dir / "pyproject.toml"
    if (
        lock_file.is_symlink()
        or project_file.is_symlink()
        or not lock_file.is_file()
        or not project_file.is_file()
    ):
        raise PackageBuildError("committed Poetry project or lock file is missing")
    if sha256_file(lock_file) != request.poetry_lock_sha256:
        raise PackageBuildError(
            "committed poetry.lock differs from --poetry-lock-sha256"
        )

    poetry_version = python_environment["poetry"]
    if _POETRY_VERSION.fullmatch(poetry_version) is None:
        raise PackageBuildError(
            "BIOSEQ_POETRY_VERSION must identify the locked Poetry 1.8 runtime"
        )
    if os.environ.get("BIOSEQ_TEMPLATE_FLATTEN_PROFILE") != "dna4_uint64":
        raise PackageBuildError(
            "BIOSEQ_TEMPLATE_FLATTEN_PROFILE must be dna4_uint64 for package builds"
        )

    actual_architecture = native_architecture()
    if actual_architecture is not request.architecture:
        raise PackageBuildError(
            f"requested {request.architecture.value} on native "
            f"{actual_architecture.value} worker"
        )


def _build_package(cell: CellContext, services: _Services) -> Path:
    builders: dict[PackageFormat, Callable[..., Path]] = {
        PackageFormat.RPM: rpm_packager.build_package,
        PackageFormat.DEB: deb_packager.build_package,
        PackageFormat.PACMAN: pacman_packager.build_package,
    }
    try:
        builder = builders[cell.distro.package_format]
    except KeyError as error:
        raise PackageBuildError(
            f"no package backend for {cell.distro.package_format}"
        ) from error
    return builder(
        cell,
        containers=services.containers,
        templates=services.templates,
        workspace=services.workspace,
    )


def _build_repository(
    cell: CellContext,
    artifact: Path,
    signing: SigningMaterial,
    services: _Services,
) -> RepositoryBuild:
    builders: dict[PackageFormat, Callable[..., RepositoryBuild]] = {
        PackageFormat.RPM: rpm_repository.build_repository,
        PackageFormat.DEB: deb_repository.build_repository,
        PackageFormat.PACMAN: pacman_repository.build_repository,
    }
    try:
        builder = builders[cell.distro.package_format]
    except KeyError as error:
        raise PackageBuildError(
            f"no repository backend for {cell.distro.package_format}"
        ) from error
    return builder(
        cell,
        artifact,
        signing=signing,
        containers=services.containers,
        workspace=services.workspace,
    )


def _process_cell(
    spec: CellSpec,
    *,
    signing: SigningMaterial,
    services: _Services,
    manifest: ManifestStore,
) -> None:
    paths = services.workspace.cell_paths(spec)
    cell_entry = manifest.start_cell(
        postgres_major=spec.postgres_major,
        distro=spec.distro.distro_id.value,
        package_name=spec.package_name,
        build_dir=paths.root,
    )
    try:
        cell = services.workspace.prepare_cell(spec)
        artifact_path = _build_package(cell, services)
        built_artifact, validation = services.acceptance.validate_built(
            cell, artifact_path
        )
        manifest.mark_validated(cell_entry, built_artifact, validation)

        built_smoke = services.acceptance.smoke(cell, artifact_path)
        manifest.mark_smoked(cell_entry, built_smoke)

        repository = _build_repository(cell, artifact_path, signing, services)
        upload = services.publisher.publish(cell, repository)
        downloaded_artifact, downloaded_validation = (
            services.acceptance.validate_download(
                cell,
                built_artifact,
                upload.downloaded_path,
            )
        )
        downloaded_smoke = services.acceptance.smoke(
            cell, upload.downloaded_path
        )
        signature_keys = {item.key for item in upload.signature_objects}
        repository_objects = tuple(
            item
            for item in upload.objects
            if item.key != upload.package.key and item.key not in signature_keys
        )
        signature_evidence = verify_published_signatures(
            runner=services.runner,
            package_format=cell.distro.package_format,
            artifact=downloaded_artifact,
            package_name=cell.package_name,
            architecture=cell.request.architecture,
            expected_signing_fingerprint=signing.fingerprint,
            published_package=upload.package,
            signature_objects=upload.signature_objects,
            downloaded_objects=upload.downloaded_objects,
            work_dir=cell.paths.download_root / "signature-verification",
            repository_objects=repository_objects,
            environment=services.environment,
        )
        evidence = UploadEvidence(
            published=True,
            bucket=cell.request.bucket,
            package=upload.package,
            download_validation=downloaded_validation,
            download_smoke=downloaded_smoke,
            signature_verification=signature_evidence,
        )
        manifest.mark_uploaded(
            cell_entry,
            evidence,
            artifact=downloaded_artifact,
        )
    except Exception as error:
        manifest.fail_cell(cell_entry, error)
        raise


def _create_manifest(
    request: WorkerRequest,
    *,
    python_environment: dict[str, str],
    redactor: Redactor,
) -> ManifestStore:
    return ManifestStore.create(
        request.manifest_path,
        architecture=request.architecture,
        requested_postgres=request.postgres_majors,
        requested_distros=tuple(value.value for value in request.distro_ids),
        bucket=request.bucket,
        region=request.region,
        base_prefix=request.s3_prefix,
        signing_fingerprint=None,
        source_revision=request.source_revision,
        poetry_lock_sha256=request.poetry_lock_sha256,
        python_environment=python_environment,
        package_version=PACKAGE_VERSION,
        redactor=redactor.redact,
    )


def _bootstrap_failure_manifest(
    arguments: argparse.Namespace,
    *,
    manifest_path: Path,
    python_environment: dict[str, str],
    redactor: Redactor,
    error: BaseException,
) -> None:
    """Write a diagnostic manifest when strict request construction cannot start."""

    now = utc_now()
    payload: dict[str, object] = {
        "schema_revision": MANIFEST_SCHEMA_REVISION,
        "package_version": PACKAGE_VERSION,
        "architecture": str(getattr(arguments, "architecture", "")),
        "status": "failed",
        "started_at": now,
        "completed_at": now,
        "requested": {"postgres": [], "distros": []},
        "s3": {
            "bucket": str(getattr(arguments, "bucket", "")),
            "region": str(getattr(arguments, "region", "")),
            "base_prefix": str(getattr(arguments, "s3_prefix", "")).strip("/"),
        },
        "signing": {"fingerprint": None},
        "source_revision": str(getattr(arguments, "source_revision", "")),
        "poetry_lock_sha256": str(
            getattr(arguments, "poetry_lock_sha256", "")
        ),
        "python_environment": dict(python_environment),
        "cells": [],
        "errors": [{"error": redactor.redact(error)}],
    }
    write_manifest(Path(manifest_path).expanduser().resolve(), payload)


def run_pipeline(
    arguments: argparse.Namespace,
    *,
    manifest_path: Path,
) -> dict[str, Any]:
    """Run one native architecture's selected matrix and return its manifest."""

    redactor = Redactor()
    runner = CommandRunner(redactor)
    python_environment = _python_environment()
    public_environment = dict(os.environ)
    public_environment.pop("GPG_PRIVATE_KEY", None)
    try:
        request = _worker_request(arguments, manifest_path=manifest_path)
        manifest = _create_manifest(
            request,
            python_environment=python_environment,
            redactor=redactor,
        )
    except Exception as error:
        try:
            _bootstrap_failure_manifest(
                arguments,
                manifest_path=manifest_path,
                python_environment=python_environment,
                redactor=redactor,
                error=error,
            )
        except Exception:
            pass
        raise PackagePipelineError(redactor.redact(error)) from error

    signing: SigningMaterial | None = None
    fatal_error: Exception | None = None
    try:
        _validate_provenance(request, python_environment)
        workspace = WorkspaceManager(request)
        services = _Services(
            runner=runner,
            containers=ContainerRunner(runner),
            templates=TemplateRenderer(),
            workspace=workspace,
            acceptance=AcceptanceService(
                runner,
                environment=public_environment,
            ),
            publisher=Publisher(
                aws_bin=request.aws_bin,
                region=request.region,
                bucket=request.bucket,
                prefix=request.s3_prefix,
                runner=runner,
                redactor=redactor,
            ),
            environment=public_environment,
        )
        signing = SigningProvider(
            aws_bin=request.aws_bin,
            environment=dict(os.environ),
            register_redaction=redactor.register,
            runner=runner,
        ).fetch(
            secret_arn=request.signing_secret_arn,
            region=request.region,
        )
        manifest.set_signing_fingerprint(signing.fingerprint)

        for spec in build_cells(request):
            try:
                _process_cell(
                    spec,
                    signing=signing,
                    services=services,
                    manifest=manifest,
                )
            except Exception as error:
                if request.fail_fast:
                    fatal_error = error
                    break
    except Exception as error:
        fatal_error = error
        manifest.append_error(error)
    finally:
        if signing is not None:
            signing.close()

    try:
        result = manifest.finalize()
    except Exception as error:
        raise PackagePipelineError(redactor.redact(error)) from error
    if fatal_error is not None:
        raise PackagePipelineError(redactor.redact(fatal_error)) from fatal_error
    return result


__all__ = ("PackagePipelineError", "run_pipeline")
