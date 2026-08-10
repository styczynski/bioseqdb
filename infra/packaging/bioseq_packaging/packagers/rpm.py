"""Direct native RPM package construction for Fedora and openSUSE."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path, PurePosixPath

from ..config import (
    PACKAGE_DESCRIPTION,
    PACKAGE_HOMEPAGE,
    PACKAGE_MAINTAINER,
    SOURCE_PROJECTION_EXCLUDES,
)
from ..model import (
    BindMount,
    CellContext,
    ContainerCapability,
    ContainerPlan,
    ContainerPurpose,
    PackageFormat,
)
from . import ContainerExecutor, PackageBuildError, TemplateService, WorkspaceService


RPM_BUILD_SCRIPT = r"""set -Eeuo pipefail
mapfile -t build_dependencies < <(printf '%s\n' "${BIOSEQ_BUILD_DEPENDENCIES}" | sed '/^$/d')
if (( ${#build_dependencies[@]} == 0 )); then
  printf 'BioSeq RPM build dependencies are empty\n' >&2
  exit 64
fi

case "${BIOSEQ_DISTRO_ID}" in
  fedora_44)
    dnf install -y --allowerasing rpmdevtools tar rsync "${build_dependencies[@]}"
    ;;
  opensuse_16.0)
    zypper --non-interactive refresh
    zypper --non-interactive install -y --allow-downgrade --allow-vendor-change \
      -t pattern devel_basis devel_rpm_build
    zypper --non-interactive install -y rpmdevtools rsync "${build_dependencies[@]}"
    ;;
  *)
    printf 'Unsupported RPM distro: %s\n' "${BIOSEQ_DISTRO_ID}" >&2
    exit 64
    ;;
esac

/source/infra/packaging/scripts/prepare_build_environment.sh
rpmdev-setuptree
mkdir -p "/root/rpmbuild/SOURCES/${BIOSEQ_SOURCE_FOLDER_NAME}"

rsync_arguments=(-a --delete)
while IFS= read -r source_exclude; do
  if [[ -n "${source_exclude}" ]]; then
    rsync_arguments+=("--exclude=${source_exclude}")
  fi
done <<< "${BIOSEQ_SOURCE_PROJECTION_EXCLUDES}"
rsync "${rsync_arguments[@]}" /source/ "/root/rpmbuild/SOURCES/${BIOSEQ_SOURCE_FOLDER_NAME}/"

cd /root/rpmbuild/SOURCES
tar -cvzf "${BIOSEQ_SOURCE_FOLDER_NAME}.tar.gz" "${BIOSEQ_SOURCE_FOLDER_NAME}/"
cd "/root/rpmbuild/SOURCES/${BIOSEQ_SOURCE_FOLDER_NAME}"
QA_RPATHS=$(( 0x0001 | 0x0010 | 0x0002 | 0x0004 | 0x0008 | 0x0020 )) \
  rpmbuild --clean -bb "${BIOSEQ_RENDERED_SPEC}"
"""

_SOURCE_TARGET = PurePosixPath("/source")
_RPMBUILD_TARGET = PurePosixPath("/root/rpmbuild")
_TOOLCHAIN_CACHE_TARGET = PurePosixPath("/opt/bioseq-package-toolchain")
_CMAKE_CACHE_TARGET = PurePosixPath("/opt/bioseq-package-cmake-cache")


def _enum_value(value: object) -> str:
    """Return a stable string for a string enum or ordinary scalar."""

    candidate = getattr(value, "value", value)
    return str(candidate)


def _runtime_dependencies(cell: CellContext) -> tuple[str, ...]:
    """Resolve the catalog's PostgreSQL-major runtime dependency patterns."""

    dependencies = cell.distro.runtime_dependencies(cell.postgres_major)
    if not dependencies:
        raise PackageBuildError(
            f"RPM runtime dependencies are empty for {cell.distro.distro_id}"
        )
    if any(not dependency.strip() or "{" in dependency for dependency in dependencies):
        raise PackageBuildError(
            f"unresolved runtime dependency pattern for {cell.distro.distro_id}"
        )
    return dependencies


def source_folder_name(cell: CellContext) -> str:
    """Return the canonical RPM source archive directory name."""

    return f"{cell.package_name}-{cell.package_version}"


def rendered_spec_path(cell: CellContext) -> Path:
    """Return the cell-local path of the rendered retained RPM spec."""

    filename = (
        f"{source_folder_name(cell)}-{cell.distro.distro_id}.spec"
    )
    return cell.paths.package_root / filename


def template_variables(cell: CellContext) -> dict[str, object]:
    """Build the strict, explicit Liquid context for the retained RPM spec."""

    architecture = _enum_value(cell.request.architecture)
    return {
        "package_name": cell.package_name,
        "version": cell.package_version,
        "source_folder_name": source_folder_name(cell),
        "description": PACKAGE_DESCRIPTION.format(
            postgres_major=cell.postgres_major
        ),
        "maintainer": PACKAGE_MAINTAINER,
        "homepage": PACKAGE_HOMEPAGE,
        "architecture": architecture,
        "postgres_major": cell.postgres_major,
        "optimization_level": 0,
        "build_dependencies": list(cell.distro.build_dependencies),
        "runtime_dependencies": list(_runtime_dependencies(cell)),
    }


def build_environment(cell: CellContext) -> dict[str, str]:
    """Return non-secret environment values consumed by the static build script."""

    return {
        "BIOSEQ_ARCHITECTURE": _enum_value(cell.request.architecture),
        "BIOSEQ_BUILD_DEPENDENCIES": "\n".join(
            cell.distro.build_dependencies
        ),
        "BIOSEQ_DISTRO_ID": cell.distro.distro_id,
        "BIOSEQ_OPTTIMIZATION_LEVEL": "0",
        "BIOSEQ_PACKAGE_NAME": cell.package_name,
        "BIOSEQ_PACKAGE_PARALLELISM": "4",
        "BIOSEQ_PACKAGE_VERSION": cell.package_version,
        "BIOSEQ_POSTGRES_MAJOR": str(cell.postgres_major),
        "BIOSEQ_RENDERED_SPEC": str(
            _RPMBUILD_TARGET / rendered_spec_path(cell).name
        ),
        "BIOSEQ_SOURCE_PROJECTION_EXCLUDES": "\n".join(
            SOURCE_PROJECTION_EXCLUDES
        ),
        "BIOSEQ_SOURCE_FOLDER_NAME": source_folder_name(cell),
        "VCPKG_MAX_CONCURRENCY": "4",
    }


def build_plan(cell: CellContext) -> ContainerPlan:
    """Create the native RPM container plan with only fixed reviewed mounts."""

    if cell.distro.package_format is not PackageFormat.RPM:
        raise PackageBuildError(
            f"RPM backend cannot build {cell.distro.package_format}"
        )
    expected_capabilities = frozenset({ContainerCapability.SYS_NICE})
    if cell.distro.capabilities != expected_capabilities:
        raise PackageBuildError(
            f"RPM build requires exactly SYS_NICE for {cell.distro.distro_id}"
        )

    environment = build_environment(cell)
    return ContainerPlan(
        image=cell.distro.image_for(cell.request.architecture),
        argv=("/bin/bash", "-c", RPM_BUILD_SCRIPT),
        mounts=(
            BindMount(
                source=cell.request.source_dir,
                target=_SOURCE_TARGET,
                read_only=True,
            ),
            BindMount(
                source=cell.paths.package_root,
                target=_RPMBUILD_TARGET,
                read_only=False,
            ),
            BindMount(
                source=cell.paths.toolchain_cache,
                target=_TOOLCHAIN_CACHE_TARGET,
                read_only=False,
            ),
            BindMount(
                source=cell.paths.cmake_cache,
                target=_CMAKE_CACHE_TARGET,
                read_only=False,
            ),
        ),
        environment_names=tuple(sorted(environment)),
        capabilities=expected_capabilities,
        output=cell.request.container_output,
        name=(
            f"bioseq-{_enum_value(cell.request.architecture)}-"
            f"pg{cell.postgres_major}-{_enum_value(cell.distro.distro_id)}-rpm-build"
        ),
        purpose=ContainerPurpose.BUILD,
    )


def find_artifact(cell: CellContext) -> Path:
    """Find exactly one non-empty canonical binary RPM for the cell."""

    rpm_root = cell.paths.package_root / "RPMS"
    candidates = sorted(
        path
        for path in rpm_root.rglob("*.rpm")
        if path.is_file()
        and cell.package_name in path.name
        and cell.package_version in path.name
    )
    if len(candidates) != 1:
        raise PackageBuildError(
            f"expected exactly one {cell.package_name} RPM under {rpm_root}; "
            f"found {len(candidates)}"
        )
    artifact = candidates[0]
    if artifact.stat().st_size <= 0:
        raise PackageBuildError(f"RPM artifact is empty: {artifact}")
    return artifact


def build_package(
    cell: CellContext,
    *,
    containers: ContainerExecutor,
    templates: TemplateService,
    workspace: WorkspaceService,
) -> Path:
    """Render, build, and return one direct native RPM artifact."""

    package_root = workspace.prepare_fresh_directory(cell.paths.package_root)
    if package_root.resolve() != cell.paths.package_root.resolve():
        raise PackageBuildError("workspace returned the wrong RPM package root")

    template_path = cell.distro.template_path
    if not template_path.is_absolute():
        template_path = cell.request.source_dir / template_path
    spec_path = rendered_spec_path(cell)
    rendered_path = templates.render_to_file(
        template_path, spec_path, template_variables(cell)
    )
    if rendered_path.resolve() != spec_path.resolve() or not spec_path.is_file():
        raise PackageBuildError("RPM template renderer did not create the expected spec")

    plan = build_plan(cell)
    environment: Mapping[str, str] = build_environment(cell)
    containers.run(plan, environment=environment)
    return find_artifact(cell)
