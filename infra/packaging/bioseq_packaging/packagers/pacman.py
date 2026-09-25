"""Direct native PACMAN package construction for Arch Linux."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path

from ..config import SOURCE_PROJECTION_EXCLUDES, package_build_parallelism
from ..containers import native_container_plan
from ..model import (
    CellContext,
    ContainerCapability,
    ContainerPlan,
    ContainerPurpose,
    PackageFormat,
)
from . import ContainerExecutor, PackageBuildError, TemplateService, WorkspaceService


PACMAN_BUILD_SCRIPT = r"""set -Eeuo pipefail
pacman_conf=/etc/pacman.conf
sandbox_option=DisableSandbox
options_headers="$(awk '$0 ~ /^\[options\][[:space:]]*$/ { count++ } END { print count + 0 }' "${pacman_conf}")"
if [[ "${options_headers}" == 0 ]]; then
  printf 'Expected at least one [options] section in pacman.conf\n' >&2
  exit 64
fi
sandbox_entries="$(awk -v option="${sandbox_option}" '$1 == option { count++ } END { print count + 0 }' "${pacman_conf}")"
if [[ "${sandbox_entries}" == 0 ]]; then
  first_options_line="$(awk '$0 ~ /^\[options\][[:space:]]*$/ { print NR; exit }' "${pacman_conf}")"
  sed -i "${first_options_line}a ${sandbox_option}" "${pacman_conf}"
elif [[ "${sandbox_entries}" != 1 ]]; then
  printf 'pacman.conf contains duplicate sandbox settings\n' >&2
  exit 64
fi
if ! awk -v option="${sandbox_option}" '
  /^\[options\][[:space:]]*$/ { in_options = 1; next }
  /^\[/ { in_options = 0 }
  in_options && $1 == option { count++ }
  END { exit count == 1 ? 0 : 1 }
' "${pacman_conf}"; then
  printf 'Sandbox setting is not present exactly once under [options]\n' >&2
  exit 64
fi

mapfile -t build_dependencies < <(
  printf '%s\n' "${BIOSEQ_BUILD_DEPENDENCIES}" | sed '/^$/d'
)
if (( ${#build_dependencies[@]} == 0 )); then
  printf 'BioSeq PACMAN build dependencies are empty\n' >&2
  exit 64
fi

pacman -Syu --noconfirm --needed \
  base-devel rsync shadow util-linux zstd "${build_dependencies[@]}"
/source/infra/packaging/scripts/prepare_build_environment.sh

package_root=/work/package
output_root=/work/package/output
source_root="${package_root}/${BIOSEQ_SOURCE_FOLDER_NAME}"
mkdir -p -- "${output_root}" "${source_root}"

rsync_arguments=(-a --delete)
while IFS= read -r source_exclude; do
  if [[ -n "${source_exclude}" ]]; then
    rsync_arguments+=("--exclude=${source_exclude}")
  fi
done <<< "${BIOSEQ_SOURCE_PROJECTION_EXCLUDES}"
rsync "${rsync_arguments[@]}" /source/ "${source_root}/"

cd "${package_root}"
tar -czf "${BIOSEQ_SOURCE_FOLDER_NAME}.tar.gz" \
  "${BIOSEQ_SOURCE_FOLDER_NAME}/"

sed -i -E '/^[[:space:]]*PKGEXT=/d' /etc/makepkg.conf
printf '%s\n' 'OPTIONS+=(!debug !lto)' "PKGEXT='.pkg.tar.zst'" \
  >> /etc/makepkg.conf
grep -Fqx 'OPTIONS+=(!debug !lto)' /etc/makepkg.conf
grep -Fqx "PKGEXT='.pkg.tar.zst'" /etc/makepkg.conf

build_user=bioseqbuild
if ! id -u "${build_user}" >/dev/null 2>&1; then
  useradd --create-home --shell /bin/bash "${build_user}"
fi
build_home="$(getent passwd "${build_user}" | cut -d: -f6)"
if [[ -z "${build_home}" || ! -d "${build_home}" ]]; then
  printf 'PACMAN build user has no valid home directory\n' >&2
  exit 65
fi
chown -R "${build_user}:${build_user}" \
  "${package_root}" \
  /opt/bioseq-package-toolchain \
  /opt/bioseq-package-cmake-cache

runuser --user "${build_user}" -- \
  env HOME="${build_home}" \
    PKGDEST=/work/package/output \
    BIOSEQ_PACKAGE_PARALLELISM="${BIOSEQ_PACKAGE_PARALLELISM}" \
    VCPKG_MAX_CONCURRENCY="${VCPKG_MAX_CONCURRENCY}" \
  bash -c 'cd /work/package && makepkg --force --nodeps --noconfirm'
"""


def _enum_value(value: object) -> str:
    candidate = getattr(value, "value", value)
    return str(candidate)


def _pacman_architecture(cell: CellContext) -> str:
    architecture = _enum_value(cell.request.architecture)
    try:
        return {"x64": "x86_64", "arm64": "aarch64"}[architecture]
    except KeyError as error:
        raise PackageBuildError(
            f"unsupported PACMAN architecture: {architecture}"
        ) from error


def _runtime_dependencies(cell: CellContext) -> tuple[str, ...]:
    dependencies = tuple(cell.distro.runtime_dependencies(cell.postgres_major))
    if not dependencies or any(not dependency.strip() for dependency in dependencies):
        raise PackageBuildError(
            f"PACMAN runtime dependencies are empty for {cell.distro.distro_id}"
        )
    return dependencies


def source_folder_name(cell: CellContext) -> str:
    """Return the canonical PACMAN source archive directory name."""

    return f"{cell.package_name}-{cell.package_version}"


def rendered_pkgbuild_path(cell: CellContext) -> Path:
    """Return the cell-local path of the rendered retained PKGBUILD."""

    return cell.paths.package_root / "PKGBUILD"


def template_variables(cell: CellContext) -> dict[str, object]:
    """Build the strict Liquid context for the retained PKGBUILD."""

    return {
        "package_name": cell.package_name,
        "version": cell.package_version,
        "source_folder_name": source_folder_name(cell),
        "description": (
            "Bioseq and bioseq_cache extensions for PostgreSQL "
            f"{cell.postgres_major}"
        ),
        "maintainer": "Bioseq Packaging <packages@bioseq.dev>",
        "homepage": "https://github.com/bioseq/bioseq",
        "architecture": _pacman_architecture(cell),
        "postgres_major": cell.postgres_major,
        "template_flatten_profile": "all_alphabets_uint64",
        "build_dependencies": list(cell.distro.build_dependencies),
        "runtime_dependencies": list(_runtime_dependencies(cell)),
    }


def build_environment(cell: CellContext) -> dict[str, str]:
    """Return non-secret values consumed by the static PACMAN build script."""

    return {
        "BIOSEQ_ARCHITECTURE": _enum_value(cell.request.architecture),
        "BIOSEQ_BUILD_DEPENDENCIES": "\n".join(cell.distro.build_dependencies),
        "BIOSEQ_DISTRO_ID": _enum_value(cell.distro.distro_id),
        "BIOSEQ_TEMPLATE_FLATTEN_PROFILE": "all_alphabets_uint64",
        "BIOSEQ_PACKAGE_NAME": cell.package_name,
        "BIOSEQ_PACKAGE_PARALLELISM": str(package_build_parallelism()),
        "BIOSEQ_PACKAGE_VERSION": cell.package_version,
        "BIOSEQ_POSTGRES_MAJOR": str(cell.postgres_major),
        "BIOSEQ_SOURCE_PROJECTION_EXCLUDES": "\n".join(
            SOURCE_PROJECTION_EXCLUDES
        ),
        "BIOSEQ_SOURCE_FOLDER_NAME": source_folder_name(cell),
        "VCPKG_MAX_CONCURRENCY": "4",
    }


def build_plan(cell: CellContext) -> ContainerPlan:
    """Create the native PACMAN build plan using the fixed mount policy."""

    if cell.distro.package_format is not PackageFormat.PACMAN:
        raise PackageBuildError(
            f"PACMAN backend cannot build {cell.distro.package_format}"
        )
    expected_capabilities = frozenset({ContainerCapability.SYS_NICE})
    if cell.distro.capabilities != expected_capabilities:
        raise PackageBuildError(
            f"PACMAN build requires exactly SYS_NICE for {cell.distro.distro_id}"
        )
    environment = build_environment(cell)
    return native_container_plan(
        cell,
        ContainerPurpose.BUILD,
        ("/bin/bash", "-c", PACMAN_BUILD_SCRIPT),
        tuple(sorted(environment)),
    )


def expected_artifact_name(cell: CellContext) -> str:
    """Return the one canonical makepkg artifact name for the cell."""

    return (
        f"{cell.package_name}-{cell.package_version}-1-"
        f"{_pacman_architecture(cell)}.pkg.tar.zst"
    )


def find_artifact(cell: CellContext) -> Path:
    """Find one non-empty zstd artifact and reject legacy xz production."""

    output_root = cell.paths.package_root / "output"
    legacy = sorted(output_root.glob("*.pkg.tar.xz"))
    if legacy:
        raise PackageBuildError(
            f"PACMAN producer emitted forbidden xz artifacts: {[p.name for p in legacy]}"
        )
    candidates = sorted(output_root.glob("*.pkg.tar.zst"))
    expected_name = expected_artifact_name(cell)
    if len(candidates) != 1 or candidates[0].name != expected_name:
        raise PackageBuildError(
            f"expected exactly {expected_name} under {output_root}; "
            f"found {[path.name for path in candidates]}"
        )
    artifact = candidates[0]
    if artifact.is_symlink() or not artifact.is_file() or artifact.stat().st_size <= 0:
        raise PackageBuildError(f"PACMAN artifact is invalid or empty: {artifact}")
    return artifact


def build_package(
    cell: CellContext,
    *,
    containers: ContainerExecutor,
    templates: TemplateService,
    workspace: WorkspaceService,
) -> Path:
    """Render, build, and return one direct native PACMAN artifact."""

    package_root = workspace.prepare_fresh_directory(cell.paths.package_root)
    if package_root.resolve() != cell.paths.package_root.resolve():
        raise PackageBuildError("workspace returned the wrong PACMAN package root")

    template_path = cell.distro.template_path
    if not template_path.is_absolute():
        template_path = cell.request.source_dir / template_path
    pkgbuild_path = rendered_pkgbuild_path(cell)
    rendered_path = templates.render_to_file(
        template_path, pkgbuild_path, template_variables(cell)
    )
    if (
        rendered_path.resolve() != pkgbuild_path.resolve()
        or not pkgbuild_path.is_file()
    ):
        raise PackageBuildError(
            "PACMAN template renderer did not create the expected PKGBUILD"
        )

    environment: Mapping[str, str] = build_environment(cell)
    containers.run(build_plan(cell), environment=environment)
    return find_artifact(cell)
