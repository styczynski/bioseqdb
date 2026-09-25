"""Bounded cell workspaces and persistent native build caches."""

from __future__ import annotations

from fnmatch import fnmatch
import os
from pathlib import Path, PurePath
import shutil

from .config import SOURCE_PROJECTION_EXCLUDES
from .model import CellContext, CellPaths, CellSpec, PackageBuildError, WorkerRequest


MINIMUM_FREE_BYTES = 80 * 1024**3


def _resolved(path: Path) -> Path:
    return path.expanduser().resolve(strict=False)


def _require_descendant(path: Path, parent: Path, *, label: str) -> Path:
    resolved = _resolved(path)
    root = _resolved(parent)
    if resolved == root or not resolved.is_relative_to(root):
        raise PackageBuildError(f"{label} must be a strict descendant of {root}: {resolved}")
    return resolved


def _nearest_existing(path: Path) -> Path:
    candidate = _resolved(path)
    while not candidate.exists():
        if candidate.parent == candidate:
            raise PackageBuildError(f"no existing ancestor for workspace path {path}")
        candidate = candidate.parent
    return candidate


def require_free_space(path: Path, *, minimum_bytes: int = MINIMUM_FREE_BYTES) -> int:
    if minimum_bytes <= 0:
        raise PackageBuildError("minimum free-space requirement must be positive")
    probe = _nearest_existing(path)
    available = shutil.disk_usage(probe).free
    if available < minimum_bytes:
        required_gib = minimum_bytes / 1024**3
        available_gib = available / 1024**3
        raise PackageBuildError(
            f"package workspace has {available_gib:.1f} GiB free; "
            f"at least {required_gib:.0f} GiB is required"
        )
    return available


def ensure_owned_directory(path: Path, *, expected_uid: int | None = None) -> Path:
    if path.is_symlink():
        raise PackageBuildError(f"workspace cache may not be a symlink: {path}")
    resolved = _resolved(path)
    resolved.mkdir(parents=True, exist_ok=True)
    if not resolved.is_dir():
        raise PackageBuildError(f"workspace cache is not a real directory: {resolved}")
    owner = os.geteuid() if expected_uid is None else expected_uid
    if resolved.stat().st_uid != owner:
        raise PackageBuildError(
            f"workspace directory {resolved} is not owned by build uid {owner}"
        )
    return resolved


def prepare_fresh_directory(path: Path, *, within: Path) -> Path:
    """Recreate one exact directory strictly below a caller-owned run root."""

    root = _resolved(within)
    resolved = _require_descendant(path, root, label="fresh directory")
    if path.is_symlink() or resolved.is_symlink():
        raise PackageBuildError(f"refusing to recursively replace symlink {path}")
    if resolved.exists():
        if not resolved.is_dir():
            raise PackageBuildError(f"fresh workspace path is not a directory: {resolved}")
        shutil.rmtree(resolved)
    resolved.mkdir(parents=True, exist_ok=False)
    return resolved


def workspace_root_for_source(source_dir: Path) -> Path:
    source = _resolved(source_dir)
    revision_root = source.parent
    if revision_root.name.startswith("source-"):
        return revision_root.parent
    return revision_root


def is_source_excluded(relative_path: str | PurePath) -> bool:
    relative = PurePath(relative_path)
    if relative.is_absolute() or ".." in relative.parts:
        raise PackageBuildError(f"source projection path must be relative: {relative}")
    relative_text = relative.as_posix()
    for pattern in SOURCE_PROJECTION_EXCLUDES:
        if pattern.startswith("/"):
            rooted_path = pattern.strip("/")
            if (
                relative_text == rooted_path
                or relative_text.startswith(f"{rooted_path}/")
            ):
                return True
        elif any(fnmatch(part, pattern.rstrip("/")) for part in relative.parts):
            return True
    return False


def copy_source_projection(source: Path, destination: Path) -> Path:
    """Copy source using the one common exclusion policy for every format."""

    source = _resolved(source)
    destination = _resolved(destination)
    if not source.is_dir():
        raise PackageBuildError(f"source projection root is not a directory: {source}")
    if destination.exists():
        raise PackageBuildError(f"source projection destination already exists: {destination}")

    def ignore(directory: str, names: list[str]) -> set[str]:
        current = Path(directory).resolve().relative_to(source)
        return {
            name
            for name in names
            if is_source_excluded(current / name)
        }

    shutil.copytree(source, destination, symlinks=True, ignore=ignore)
    leaked = [
        path.relative_to(destination)
        for path in destination.rglob("*")
        if is_source_excluded(path.relative_to(destination))
    ]
    if leaked:
        raise PackageBuildError(
            f"excluded source paths entered projection: {sorted(map(str, leaked))[:10]}"
        )
    return destination


class WorkspaceManager:
    """Derive and safely prepare one worker request's cell directories."""

    def __init__(
        self,
        request: WorkerRequest,
        *,
        minimum_free_bytes: int = MINIMUM_FREE_BYTES,
        expected_uid: int | None = None,
    ) -> None:
        self.request = request
        self.run_root = _resolved(request.build_root)
        self.workspace_root = workspace_root_for_source(request.source_dir)
        self.minimum_free_bytes = minimum_free_bytes
        self.expected_uid = os.geteuid() if expected_uid is None else expected_uid

    def cell_paths(self, spec: CellSpec) -> CellPaths:
        if spec.request != self.request:
            raise PackageBuildError("cell specification belongs to a different worker request")
        root = _require_descendant(
            self.run_root
            / "cells"
            / f"postgresql{spec.postgres_major}"
            / spec.distro.distro_id.value,
            self.run_root,
            label="cell root",
        )
        cache_root = self.workspace_root / "caches"
        # Reuse compiler/CMake/vcpkg tools across server majors at the existing
        # pg17 location. PostgreSQL headers and Bioseq objects stay separate.
        toolchain = _resolved(
            cache_root
            / "package-toolchains"
            / spec.distro.distro_id.value
            / "postgresql17"
        )
        cmake = _resolved(
            cache_root
            / "package-cmake"
            / spec.distro.distro_id.value
            / f"postgresql{spec.postgres_major}"
        )
        return CellPaths(
            root=root,
            package_root=root / "package",
            repository_root=root / "repository",
            download_root=root / "download",
            toolchain_cache=toolchain,
            cmake_cache=cmake,
        )

    def prepare_cell_paths(self, spec: CellSpec) -> CellPaths:
        require_free_space(
            self.workspace_root,
            minimum_bytes=self.minimum_free_bytes,
        )
        self.run_root.mkdir(parents=True, exist_ok=True)
        paths = self.cell_paths(spec)
        prepare_fresh_directory(paths.root, within=self.run_root)
        paths.package_root.mkdir()
        paths.repository_root.mkdir()
        paths.download_root.mkdir()
        ensure_owned_directory(paths.toolchain_cache, expected_uid=self.expected_uid)
        ensure_owned_directory(paths.cmake_cache, expected_uid=self.expected_uid)
        return paths

    def prepare_cell(self, spec: CellSpec) -> CellContext:
        return CellContext.from_spec(spec, self.prepare_cell_paths(spec))

    def prepare_fresh_directory(self, path: Path) -> Path:
        return prepare_fresh_directory(path, within=self.run_root)

    def copy_source_projection(self, source: Path, destination: Path) -> Path:
        _require_descendant(destination, self.run_root, label="source projection")
        return copy_source_projection(source, destination)
