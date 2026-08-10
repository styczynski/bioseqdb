"""Fresh signed RPM repository construction for Fedora and openSUSE."""

from __future__ import annotations

import re
import shutil
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path, PurePosixPath

from ..model import (
    BindMount,
    CellContext,
    ContainerPlan,
    ContainerPurpose,
    PackageFormat,
)
from ..packagers import ContainerExecutor, WorkspaceService
from ..signing import (
    PINNED_SIGNING_FINGERPRINT,
    SigningMaterial,
    repository_environment as signing_repository_environment,
)
from . import RepositoryBuild, RepositoryBuildError


RPM_REPOSITORY_SCRIPT = r"""set -Eeuo pipefail
private_key_value="${GPG_PRIVATE_KEY:?GPG_PRIVATE_KEY is required}"
unset GPG_PRIVATE_KEY
export -n private_key_value

case "${BIOSEQ_DISTRO_ID}" in
  fedora_44)
    dnf install -y --allowerasing createrepo_c rpm-sign gnupg2 coreutils
    ;;
  opensuse_16.0)
    zypper --non-interactive refresh
    zypper --non-interactive install -y createrepo_c rpm gpg2 coreutils
    ;;
  *)
    printf 'Unsupported RPM repository distro: %s\n' "${BIOSEQ_DISTRO_ID}" >&2
    exit 64
    ;;
esac

signing_home=$(mktemp -d /tmp/bioseq-rpm-signing.XXXXXX)
chmod 0700 "${signing_home}"
export GNUPGHOME="${signing_home}"
cleanup_signing_home() {
  unset private_key_value GPG_PRIVATE_KEY
  rm -rf -- "${signing_home}"
}
trap cleanup_signing_home EXIT

private_key_file="${signing_home}/private-key.asc"
printf '%s' "${private_key_value}" | base64 --decode > "${private_key_file}"
unset private_key_value
chmod 0600 "${private_key_file}"
gpg --no-tty --batch --homedir "${signing_home}" --import "${private_key_file}"
rm -f -- "${private_key_file}"

actual_fingerprint=$(gpg --no-tty --batch --homedir "${signing_home}" \
  --with-colons --fingerprint --list-secret-keys "${BIOSEQ_SIGNING_FINGERPRINT}" \
  | awk -F: '/^fpr:/ {print $10; exit}')
if [[ "${actual_fingerprint}" != "${BIOSEQ_SIGNING_FINGERPRINT}" ]]; then
  printf 'Imported RPM signing key fingerprint does not match\n' >&2
  exit 65
fi

rpm_path="/repo/${BIOSEQ_RPM_FILENAME}"
rpm --define '_signature gpg' \
  --define "_gpg_name ${BIOSEQ_SIGNING_FINGERPRINT}" \
  --addsign "${rpm_path}"
createrepo_c --retain-old-md=0 --compatibility /repo

gpg --no-tty --batch --yes --homedir "${signing_home}" \
  --local-user "${BIOSEQ_SIGNING_FINGERPRINT}" \
  --detach-sign --armor \
  --output /repo/repodata/repomd.xml.asc \
  /repo/repodata/repomd.xml
gpg --no-tty --batch --armor --homedir "${signing_home}" \
  --output /repo/repodata/repomd.xml.key \
  --export "${BIOSEQ_SIGNING_FINGERPRINT}"

gpg --no-tty --batch --homedir "${signing_home}" \
  --verify /repo/repodata/repomd.xml.asc /repo/repodata/repomd.xml
rpm --import /repo/repodata/repomd.xml.key
rpm --checksig "${rpm_path}"
"""

_REPOSITORY_TARGET = PurePosixPath("/repo")


def _validated_fingerprint(signing: SigningMaterial) -> str:
    """Return the one pinned uppercase signing fingerprint."""

    fingerprint = str(signing.fingerprint).upper()
    if not re.fullmatch(r"[0-9A-F]{40}", fingerprint):
        raise RepositoryBuildError("RPM signing fingerprint is not a full v4 fingerprint")
    if fingerprint != PINNED_SIGNING_FINGERPRINT:
        raise RepositoryBuildError("RPM signing fingerprint is not the pinned BioSeq key")
    return fingerprint


@contextmanager
def repository_environment(
    cell: CellContext, artifact: Path, signing: SigningMaterial
) -> Iterator[dict[str, str]]:
    """Yield the short-lived repository environment without exposing it in argv."""

    fingerprint = _validated_fingerprint(signing)
    with signing_repository_environment(signing) as environment:
        if set(environment) != {"GPG_PRIVATE_KEY"} or not environment[
            "GPG_PRIVATE_KEY"
        ]:
            raise RepositoryBuildError(
                "signing provider did not return exactly one private-key environment value"
            )
        environment.update(
            {
                "BIOSEQ_DISTRO_ID": cell.distro.distro_id,
                "BIOSEQ_RPM_FILENAME": artifact.name,
                "BIOSEQ_SIGNING_FINGERPRINT": fingerprint,
            }
        )
        try:
            yield environment
        finally:
            # The signing context clears the private key; remove our public
            # additions first so no caller-retained mapping survives expanded.
            for name in tuple(environment):
                if name != "GPG_PRIVATE_KEY":
                    environment.pop(name, None)


def repository_plan(
    cell: CellContext, artifact: Path, signing: SigningMaterial
) -> ContainerPlan:
    """Create a secret-name-only plan for signing one fresh RPM repository."""

    if cell.distro.package_format is not PackageFormat.RPM:
        raise RepositoryBuildError(
            f"RPM repository cannot publish {cell.distro.package_format}"
        )
    if artifact.name != Path(artifact.name).name or not artifact.name.endswith(".rpm"):
        raise RepositoryBuildError("RPM artifact has an invalid repository filename")
    _validated_fingerprint(signing)
    environment_names = (
        "BIOSEQ_DISTRO_ID",
        "BIOSEQ_RPM_FILENAME",
        "BIOSEQ_SIGNING_FINGERPRINT",
        "GPG_PRIVATE_KEY",
    )
    return ContainerPlan(
        image=cell.distro.image_for(cell.request.architecture),
        argv=("/bin/bash", "-c", RPM_REPOSITORY_SCRIPT),
        mounts=(
            # Repository construction never receives source or persistent caches.
            # It operates only on a fresh, bounded cell repository tree.
            BindMount(
                source=cell.paths.repository_root,
                target=_REPOSITORY_TARGET,
                read_only=False,
            ),
        ),
        environment_names=tuple(sorted(environment_names)),
        capabilities=frozenset(),
        output=cell.request.container_output,
        name=(
            f"bioseq-{cell.request.architecture.value}-pg{cell.postgres_major}-"
            f"{cell.distro.distro_id.value}-rpm-repository"
        ),
        purpose=ContainerPurpose.REPOSITORY,
    )


def _repository_result(cell: CellContext, package_path: Path) -> RepositoryBuild:
    """Validate the signed RPM and every required repository evidence file."""

    repository_root = cell.paths.repository_root
    repodata = repository_root / "repodata"
    signature_paths = (
        repodata / "repomd.xml.key",
        repodata / "repomd.xml",
        repodata / "repomd.xml.asc",
    )
    required = (package_path, *signature_paths)
    missing = [path for path in required if not path.is_file() or path.stat().st_size <= 0]
    if missing:
        relative = [str(path.relative_to(repository_root)) for path in missing]
        raise RepositoryBuildError(
            f"RPM repository is missing non-empty required files: {relative}"
        )
    metadata_paths = tuple(
        sorted(path for path in repodata.rglob("*") if path.is_file())
    )
    if not metadata_paths:
        raise RepositoryBuildError("createrepo_c produced no RPM metadata")
    return RepositoryBuild(
        root=repository_root,
        package_path=package_path,
        signature_paths=signature_paths,
        metadata_paths=metadata_paths,
    )


def build_repository(
    cell: CellContext,
    artifact: Path,
    *,
    signing: SigningMaterial,
    containers: ContainerExecutor,
    workspace: WorkspaceService,
) -> RepositoryBuild:
    """Copy, embed-sign, index, and metadata-sign one fresh RPM repository."""

    artifact = artifact.resolve()
    if not artifact.is_file() or artifact.stat().st_size <= 0:
        raise RepositoryBuildError("RPM artifact is missing or empty")
    if not artifact.name.endswith(".rpm"):
        raise RepositoryBuildError("RPM repository received a non-RPM artifact")
    if (
        cell.package_name not in artifact.name
        or cell.package_version not in artifact.name
    ):
        raise RepositoryBuildError("RPM artifact filename does not match the cell")
    if artifact.is_relative_to(cell.paths.repository_root.resolve()):
        raise RepositoryBuildError(
            "RPM build artifact must be outside the fresh repository root"
        )

    repository_root = workspace.prepare_fresh_directory(
        cell.paths.repository_root
    )
    if repository_root.resolve() != cell.paths.repository_root.resolve():
        raise RepositoryBuildError("workspace returned the wrong RPM repository root")
    package_path = repository_root / artifact.name
    shutil.copy2(artifact, package_path)
    if package_path.stat().st_size != artifact.stat().st_size:
        raise RepositoryBuildError("copied RPM size differs before signing")

    plan = repository_plan(cell, package_path, signing)
    with repository_environment(cell, package_path, signing) as environment:
        containers.run(plan, environment=environment)
    return _repository_result(cell, package_path)
