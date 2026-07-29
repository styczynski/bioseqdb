"""Native package builders owned by the BioSeq packaging tool."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any, Protocol

from ..model import PackageBuildError


class ContainerExecutor(Protocol):
    """Execute one validated container plan with name-only environment wiring."""

    def run(
        self, plan: Any, *, environment: Mapping[str, str]
    ) -> None:
        """Run ``plan`` and raise when the container exits unsuccessfully."""


class TemplateService(Protocol):
    """Strictly render reviewed packaging templates."""

    def render_to_file(
        self,
        template_path: Path,
        destination: Path,
        variables: Mapping[str, object],
    ) -> Path:
        """Render ``template_path`` to ``destination`` and return that path."""


class WorkspaceService(Protocol):
    """Prepare only paths proven to belong to the current cell workspace."""

    def prepare_fresh_directory(self, path: Path) -> Path:
        """Create an empty, cell-owned directory and return its resolved path."""
