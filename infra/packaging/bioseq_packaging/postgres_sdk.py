"""Read the native prerelease runtime built in the matching distro cache."""
from __future__ import annotations

import json
from pathlib import Path

from .artifacts import sha256_file
from .model import CellContext, DistroId, PackageBuildError


def required(cell: CellContext) -> bool:
    return cell.distro.requires_postgres_sdk(cell.postgres_major, cell.request.architecture)


def runtime_package(cell: CellContext) -> tuple[Path, dict]:
    """Verify the exact cached native SDK package before installing/publishing it."""
    output = cell.paths.toolchain_cache / 'postgres-sdk' / str(cell.postgres_major) / 'output'
    metadata = json.loads((output / 'sdk.json').read_text())
    name = metadata['file']
    if Path(name).name != name:
        raise PackageBuildError('SDK package name must not escape its cache')
    artifact = output / name
    pin = json.loads((cell.request.source_dir / 'infra/packaging/postgres_sdks.json').read_text())[
        str(cell.postgres_major)]
    expected_arch = {'x64': 'x86_64', 'arm64': 'aarch64'}[cell.request.architecture.value]
    expected_distro = {DistroId.ARCH: 'arch', DistroId.OPENSUSE_16_0: 'opensuse',
                      DistroId.FEDORA_44: 'fedora'}[cell.distro.distro_id]
    if (metadata.get('source_sha256') != pin['sha256']
            or metadata.get('postgres_major') != cell.postgres_major
            or metadata.get('architecture') != expected_arch
            or metadata.get('distro') != expected_distro
            or artifact.is_symlink() or not artifact.is_file()
            or artifact.stat().st_size != metadata['bytes']
            or sha256_file(artifact) != metadata['sha256']):
        raise PackageBuildError('Native SDK package identity or bytes differ from the selected cell')
    return artifact, metadata
