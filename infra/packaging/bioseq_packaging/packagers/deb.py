"""Direct native DEB construction for Debian and Ubuntu.

The backend deliberately separates package construction from repository
signing.  It recursively materializes the retained ``debian/`` template tree,
copies the reviewed source projection into a clean container-local build tree,
and returns exactly one unsigned binary package.
"""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
import re
import shutil

from ..config import (
    PACKAGE_DESCRIPTION,
    PACKAGE_HOMEPAGE,
    PACKAGE_MAINTAINER,
    SOURCE_PROJECTION_EXCLUDES,
    package_build_parallelism,
)
from ..containers import native_container_plan
from ..model import (
    CellContext,
    ContainerPlan,
    ContainerPurpose,
    PackageFormat,
)
from . import ContainerExecutor, PackageBuildError, TemplateService, WorkspaceService


DEB_BUILD_SCRIPT = r"""set -Eeuo pipefail
export DEBIAN_FRONTEND=noninteractive

mapfile -t build_dependencies < <(printf '%s\n' "${BIOSEQ_BUILD_DEPENDENCIES}" | sed '/^$/d')
if (( ${#build_dependencies[@]} == 0 )); then
  printf 'BioSeq DEB build dependencies are empty\n' >&2
  exit 64
fi

# An updated security archive can remove a package while an intermediary still
# serves the previous index. Refresh indexes before bounded download retries;
# never restart compilation or hide a permanent dependency failure.
for dependency_attempt in 1 2 3; do
  apt-get -o Acquire::Retries=3 -o Acquire::http::No-Cache=true \
    -o Acquire::https::No-Cache=true update
  if apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
      build-essential ca-certificates debhelper rsync "${build_dependencies[@]}"; then
    break
  fi
  if (( dependency_attempt == 3 )); then exit 100; fi
  printf 'Refreshing package indexes after dependency download failure\n' >&2
done

/source/infra/packaging/scripts/prepare_build_environment.sh

build_root=/work/source
rm -rf -- "${build_root}"
mkdir -p -- "${build_root}"

rsync_arguments=(-a --delete)
while IFS= read -r source_exclude; do
  if [[ -n "${source_exclude}" ]]; then
    rsync_arguments+=("--exclude=${source_exclude}")
  fi
done <<< "${BIOSEQ_SOURCE_PROJECTION_EXCLUDES}"
rsync "${rsync_arguments[@]}" /source/ "${build_root}/"

rm -rf -- "${build_root}/debian"
cp -a -- /work/package/debian "${build_root}/debian"
test "$(stat -c '%a' "${build_root}/debian/rules")" = 755

cd "${build_root}"
DEB_BUILD_OPTIONS=noddebs dpkg-buildpackage --no-sign -b -tc

mapfile -d '' artifacts < <(
  find /work -maxdepth 1 -type f -name '*.deb' -print0
)
if (( ${#artifacts[@]} != 1 )); then
  printf 'Expected exactly one DEB artifact; found %s\n' "${#artifacts[@]}" >&2
  exit 65
fi
cp -- "${artifacts[0]}" /work/package/
"""

_UNRESOLVED_BIOSEQ_ENVIRONMENT = re.compile(rb"\$\{BIOSEQ_[A-Z0-9_]+(?::-[^}]*)?\}")


def _enum_value(value: object) -> str:
    """Return a stable string for a string enum or ordinary scalar."""

    candidate = getattr(value, "value", value)
    return str(candidate)


def _debian_architecture(cell: CellContext) -> str:
    """Map the worker architecture to Debian's canonical architecture name."""

    architecture = _enum_value(cell.request.architecture)
    try:
        return {"x64": "amd64", "arm64": "arm64"}[architecture]
    except KeyError as error:
        raise PackageBuildError(
            f"unsupported DEB architecture: {architecture}"
        ) from error


def _runtime_dependencies(cell: CellContext) -> tuple[str, ...]:
    """Resolve runtime dependencies through the strict catalog API."""

    dependencies = list(cell.distro.runtime_dependencies(cell.postgres_major, cell.request.architecture))
    if not dependencies:
        raise PackageBuildError(
            f"DEB runtime dependencies are empty for {cell.distro.distro_id}"
        )
    return tuple(dependencies)


def source_folder_name(cell: CellContext) -> str:
    """Return the canonical native-package source directory name."""

    return f"{cell.package_name}-{cell.package_version}"


def template_variables(cell: CellContext) -> dict[str, object]:
    """Build the strict, explicit context for every retained DEB template."""

    return {
        "package_name": cell.package_name,
        "version": cell.package_version,
        "source_folder_name": source_folder_name(cell),
        "description": PACKAGE_DESCRIPTION.format(
            postgres_major=cell.postgres_major
        ),
        "maintainer": PACKAGE_MAINTAINER,
        "homepage": PACKAGE_HOMEPAGE,
        "architecture": _debian_architecture(cell),
        "postgres_major": cell.postgres_major,
        "template_flatten_profile": "all_alphabets_uint64",
        "build_dependencies": list(cell.distro.build_dependencies),
        "runtime_dependencies": list(_runtime_dependencies(cell)),
    }


def _template_root(cell: CellContext) -> Path:
    template_root = cell.distro.template_path
    if not template_root.is_absolute():
        template_root = cell.request.source_dir / template_root
    template_root = template_root.resolve()
    if not template_root.is_dir():
        raise PackageBuildError(f"DEB template root is not a directory: {template_root}")
    return template_root


def _assert_resolved_template(path: Path) -> None:
    data = path.read_bytes()
    if b"{{" in data or b"{%" in data or _UNRESOLVED_BIOSEQ_ENVIRONMENT.search(data):
        raise PackageBuildError(f"unresolved token remains in retained DEB file: {path}")


def render_debian_tree(
    cell: CellContext,
    *,
    templates: TemplateService,
) -> Path:
    """Recursively materialize the retained Debian tree for one cell.

    ``.liquid`` is removed from rendered destinations.  Static files are copied
    byte-for-byte, symlinks are rejected, and ``debian/rules`` is always mode
    ``0755`` regardless of the checkout's umask or executable bit.
    """

    template_root = _template_root(cell)
    destination_root = cell.paths.package_root / "debian"
    destination_root.mkdir(parents=True, exist_ok=False)
    variables = template_variables(cell)

    sources = sorted(template_root.rglob("*"), key=lambda path: path.as_posix())
    if not sources:
        raise PackageBuildError(f"DEB template root is empty: {template_root}")

    materialized: list[Path] = []
    for source in sources:
        if source.is_symlink():
            raise PackageBuildError(f"symlink is not allowed in DEB templates: {source}")
        relative = source.relative_to(template_root)
        if source.is_dir():
            (destination_root / relative).mkdir(parents=True, exist_ok=True)
            continue
        if not source.is_file():
            raise PackageBuildError(f"unsupported DEB template entry: {source}")

        if source.suffix == ".liquid":
            relative = relative.with_suffix("")
            destination = destination_root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            rendered = templates.render_to_file(source, destination, variables)
            if rendered.resolve() != destination.resolve():
                raise PackageBuildError(
                    f"DEB renderer returned an unexpected destination for {source}"
                )
        else:
            destination = destination_root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)

        destination.chmod(0o755 if relative == Path("rules") else 0o644)
        _assert_resolved_template(destination)
        materialized.append(destination)

    rules = destination_root / "rules"
    if not rules.is_file() or rules.stat().st_mode & 0o777 != 0o755:
        raise PackageBuildError("rendered debian/rules is missing or not mode 0755")
    if not materialized:
        raise PackageBuildError("DEB template materialization produced no files")
    return destination_root


def build_environment(cell: CellContext) -> dict[str, str]:
    """Return non-secret values consumed by the static DEB build script."""

    return {
        "BIOSEQ_ARCHITECTURE": _enum_value(cell.request.architecture),
        "BIOSEQ_BUILD_DEPENDENCIES": "\n".join(cell.distro.build_dependencies),
        "BIOSEQ_DISTRO_ID": cell.distro.distro_id,
        "BIOSEQ_TEMPLATE_FLATTEN_PROFILE": "all_alphabets_uint64",
        "BIOSEQ_PACKAGE_NAME": cell.package_name,
        "BIOSEQ_PACKAGE_PARALLELISM": str(package_build_parallelism()),
        "BIOSEQ_PACKAGE_VERSION": cell.package_version,
        "BIOSEQ_POSTGRES_MAJOR": str(cell.postgres_major),
        "BIOSEQ_SOURCE_PROJECTION_EXCLUDES": "\n".join(
            SOURCE_PROJECTION_EXCLUDES
        ),
        "VCPKG_MAX_CONCURRENCY": "4",
    }


def build_plan(cell: CellContext) -> ContainerPlan:
    """Create a native DEB build plan with only reviewed fixed mounts."""

    if cell.distro.package_format is not PackageFormat.DEB:
        raise PackageBuildError(
            f"DEB backend cannot build {cell.distro.package_format}"
        )
    environment = build_environment(cell)
    return native_container_plan(
        cell,
        ContainerPurpose.BUILD,
        ("/bin/bash", "-c", DEB_BUILD_SCRIPT),
        tuple(sorted(environment)),
    )


def find_artifact(cell: CellContext) -> Path:
    """Find exactly one non-empty canonical DEB artifact for the cell."""

    candidates = sorted(
        path
        for path in cell.paths.package_root.glob("*.deb")
        if path.is_file()
        and cell.package_name in path.name
        and cell.package_version in path.name
    )
    if len(candidates) != 1:
        raise PackageBuildError(
            f"expected exactly one {cell.package_name} DEB under "
            f"{cell.paths.package_root}; found {len(candidates)}"
        )
    artifact = candidates[0]
    if artifact.stat().st_size <= 0:
        raise PackageBuildError(f"DEB artifact is empty: {artifact}")
    expected_architecture = _debian_architecture(cell)
    if not artifact.name.endswith(f"_{expected_architecture}.deb"):
        raise PackageBuildError(
            f"DEB artifact does not have canonical {expected_architecture} suffix: "
            f"{artifact.name}"
        )
    return artifact


def build_package(
    cell: CellContext,
    *,
    containers: ContainerExecutor,
    templates: TemplateService,
    workspace: WorkspaceService,
) -> Path:
    """Render, build, and return one direct native unsigned DEB artifact."""

    package_root = workspace.prepare_fresh_directory(cell.paths.package_root)
    if package_root.resolve() != cell.paths.package_root.resolve():
        raise PackageBuildError("workspace returned the wrong DEB package root")

    render_debian_tree(cell, templates=templates)
    environment: Mapping[str, str] = build_environment(cell)
    containers.run(build_plan(cell), environment=environment)
    return find_artifact(cell)
