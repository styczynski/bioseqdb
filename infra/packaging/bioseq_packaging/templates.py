"""Strict Liquid rendering for the retained reviewed package templates."""

from __future__ import annotations

from collections.abc import Mapping
from datetime import datetime, timezone
from email.utils import format_datetime
import os
from pathlib import Path
import re
import shutil
from typing import Any

from .model import PackageBuildError


PUBLIC_CONTEXT_FIELDS = frozenset(
    {
        "package_name",
        "version",
        "source_folder_name",
        "description",
        "maintainer",
        "homepage",
        "architecture",
        "postgres_major",
        "optimization_level",
        "build_dependencies",
        "runtime_dependencies",
    }
)
_RENDERER_OWNED_FIELDS = frozenset({"current_time_rfc2822"})
_UNRESOLVED = re.compile(r"{{|{%|\$\{BIOSEQ_")


def _render_time(
    *,
    source_date_epoch: str | int | None,
    run_started_at: datetime | None,
) -> str:
    if source_date_epoch is not None:
        try:
            epoch = int(source_date_epoch)
        except (TypeError, ValueError) as error:
            raise PackageBuildError("SOURCE_DATE_EPOCH must be a non-negative integer") from error
        if epoch < 0:
            raise PackageBuildError("SOURCE_DATE_EPOCH must be a non-negative integer")
        try:
            timestamp = datetime.fromtimestamp(epoch, tz=timezone.utc)
        except (OverflowError, OSError, ValueError) as error:
            raise PackageBuildError("SOURCE_DATE_EPOCH is outside the supported range") from error
    else:
        timestamp = run_started_at or datetime.now(timezone.utc)
        if timestamp.tzinfo is None:
            timestamp = timestamp.replace(tzinfo=timezone.utc)
        timestamp = timestamp.astimezone(timezone.utc)
    return format_datetime(timestamp)


def _validate_context(variables: Mapping[str, Any]) -> dict[str, Any]:
    keys = set(variables)
    extra = keys - PUBLIC_CONTEXT_FIELDS
    missing = PUBLIC_CONTEXT_FIELDS - keys
    if extra:
        raise PackageBuildError(
            f"template context contains forbidden fields: {sorted(extra)}"
        )
    if missing:
        raise PackageBuildError(f"template context is missing fields: {sorted(missing)}")
    result = dict(variables)
    for name in (
        "package_name",
        "version",
        "source_folder_name",
        "description",
        "maintainer",
        "homepage",
        "architecture",
    ):
        if not isinstance(result[name], str) or not result[name].strip():
            raise PackageBuildError(f"template context field {name} must be non-empty text")
    if isinstance(result["postgres_major"], bool) or not isinstance(
        result["postgres_major"], int
    ) or result["postgres_major"] < 17:
        raise PackageBuildError("template PostgreSQL major must be an integer at least 17")
    if result["optimization_level"] != 0:
        raise PackageBuildError("package templates must use optimization level zero")
    for name in ("build_dependencies", "runtime_dependencies"):
        values = result[name]
        if (
            isinstance(values, (str, bytes))
            or not isinstance(values, (list, tuple))
            or not values
            or any(not isinstance(value, str) or not value.strip() for value in values)
        ):
            raise PackageBuildError(f"template context field {name} is invalid")
        result[name] = tuple(values)
    return result


class TemplateRenderer:
    """One strict renderer whose generated changelog time is stable per run."""

    def __init__(
        self,
        *,
        source_date_epoch: str | int | None = None,
        run_started_at: datetime | None = None,
    ) -> None:
        if source_date_epoch is None:
            source_date_epoch = os.environ.get("SOURCE_DATE_EPOCH")
        self.current_time_rfc2822 = _render_time(
            source_date_epoch=source_date_epoch,
            run_started_at=run_started_at,
        )
        try:
            from liquid import Environment, StrictUndefined
        except ImportError as error:
            raise PackageBuildError(
                "python-liquid is required; run the locked Poetry environment"
            ) from error
        self._environment = Environment(undefined=StrictUndefined)

    def context(self, variables: Mapping[str, Any]) -> dict[str, Any]:
        context = _validate_context(variables)
        # This compatibility value is owned by the renderer, derived from
        # SOURCE_DATE_EPOCH or the worker run start, and cannot be overridden
        # by callers or widen the public context.
        context["current_time_rfc2822"] = self.current_time_rfc2822
        return context

    def render_text(
        self,
        template_text: str,
        variables: Mapping[str, Any],
        *,
        source: str = "<template>",
    ) -> str:
        try:
            rendered = self._environment.from_string(template_text).render(
                **self.context(variables)
            )
        except Exception as error:
            raise PackageBuildError(f"strict Liquid rendering failed for {source}: {error}") from error
        unresolved = _UNRESOLVED.search(rendered)
        if unresolved:
            raise PackageBuildError(
                f"rendered template {source} contains unresolved token {unresolved.group(0)!r}"
            )
        return rendered

    def render_to_file(
        self,
        template_path: Path,
        destination: Path,
        variables: Mapping[str, Any],
    ) -> Path:
        template_path = template_path.resolve()
        destination = destination.resolve(strict=False)
        if not template_path.is_file():
            raise PackageBuildError(f"Liquid template is missing: {template_path}")
        rendered = self.render_text(
            template_path.read_text(encoding="utf-8"),
            variables,
            source=str(template_path),
        )
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_name(f".{destination.name}.tmp-{os.getpid()}")
        try:
            temporary.write_text(rendered, encoding="utf-8")
            os.chmod(temporary, 0o755 if destination.name == "rules" else 0o644)
            temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)
        return destination

    def render_tree(
        self,
        template_root: Path,
        destination_root: Path,
        variables: Mapping[str, Any],
    ) -> Path:
        template_root = template_root.resolve()
        destination_root = destination_root.resolve(strict=False)
        if not template_root.is_dir():
            raise PackageBuildError(f"template tree is missing: {template_root}")
        destination_root.mkdir(parents=True, exist_ok=True)
        outputs: set[Path] = set()
        for source in sorted(template_root.rglob("*")):
            relative = source.relative_to(template_root)
            if source.is_symlink():
                raise PackageBuildError(f"template tree may not contain symlinks: {source}")
            if source.is_dir():
                (destination_root / relative).mkdir(parents=True, exist_ok=True)
                continue
            if source.suffix == ".liquid":
                relative = relative.with_suffix("")
            destination = destination_root / relative
            if destination in outputs:
                raise PackageBuildError(f"template tree has colliding output {relative}")
            outputs.add(destination)
            destination.parent.mkdir(parents=True, exist_ok=True)
            if source.suffix == ".liquid":
                self.render_to_file(source, destination, variables)
            else:
                shutil.copyfile(source, destination)
                shutil.copymode(source, destination)
                if destination.name == "rules":
                    os.chmod(destination, 0o755)
        rules = destination_root / "rules"
        if rules.exists():
            os.chmod(rules, 0o755)
        return destination_root


_DEFAULT_RENDERER: TemplateRenderer | None = None


def _default_renderer() -> TemplateRenderer:
    global _DEFAULT_RENDERER
    if _DEFAULT_RENDERER is None:
        _DEFAULT_RENDERER = TemplateRenderer()
    return _DEFAULT_RENDERER


def render_to_file(
    template_path: Path,
    destination: Path,
    variables: Mapping[str, Any],
) -> Path:
    return _default_renderer().render_to_file(template_path, destination, variables)


def render_tree(
    template_root: Path,
    destination_root: Path,
    variables: Mapping[str, Any],
) -> Path:
    return _default_renderer().render_tree(template_root, destination_root, variables)
