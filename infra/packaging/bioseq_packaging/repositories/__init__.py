"""Signed native repository builders owned by the BioSeq packaging tool."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from ..model import PackageBuildError


class RepositoryBuildError(PackageBuildError):
    """Raised when repository construction or signing is incomplete."""


@dataclass(frozen=True, slots=True)
class RepositoryBuild:
    """Complete repository tree and its controller-required signed objects."""

    root: Path
    package_path: Path
    signature_paths: tuple[Path, ...]
    metadata_paths: tuple[Path, ...]

    @property
    def relative_signature_paths(self) -> tuple[Path, ...]:
        """Return signature evidence paths relative to the repository root."""

        return tuple(path.relative_to(self.root) for path in self.signature_paths)
