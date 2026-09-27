"""Deterministic signed APT repository construction for one BioSeq DEB."""

from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path, PurePosixPath
import re
import shutil

from ..artifacts import sha256_file
from ..model import (
    BindMount,
    CellContext,
    ContainerPlan,
    ContainerPurpose,
    PackageFormat,
)
from ..packagers import ContainerExecutor, WorkspaceService
from ..signing import (
    SIGNING_ENVIRONMENT_NAME,
    SigningMaterial,
    repository_environment as signing_repository_environment,
    require_pinned_fingerprint,
)
from . import RepositoryBuild, RepositoryBuildError


APT_REPOSITORY_SCRIPT = r"""set -Eeuo pipefail
private_key_value="${GPG_PRIVATE_KEY:?GPG_PRIVATE_KEY is required}"
unset GPG_PRIVATE_KEY
export -n private_key_value

export DEBIAN_FRONTEND=noninteractive
export LC_ALL=C

apt-get update
apt-get install -y --no-install-recommends ca-certificates dpkg-dev gzip gnupg

cd /repo/stable
shopt -s nullglob
packages=(*.deb)
if (( ${#packages[@]} != 1 )); then
  printf 'Expected exactly one retained DEB; found %s\n' "${#packages[@]}" >&2
  exit 65
fi
if [[ "${packages[0]}" != "${BIOSEQ_PACKAGE_FILENAME}" ]]; then
  printf 'Unexpected retained DEB filename\n' >&2
  exit 65
fi

# A fresh repository contains one fixed-version package.  dpkg-scanpackages is
# still followed by a locale-fixed paragraph sort so this stays canonical if
# the retention policy is extended deliberately later.
dpkg-scanpackages . /dev/null > Packages.scan
awk 'BEGIN { RS=""; ORS="\0" } { print $0 "\n" }' Packages.scan \
  | sort -z > Packages.sorted
tr '\0' '\n' < Packages.sorted \
  | sed 's#^Filename: \./#Filename: #' > Packages
rm -f -- Packages.scan Packages.sorted
gzip -n -9 -c Packages > Packages.gz

package_sha256="$(sha256sum -- "${BIOSEQ_PACKAGE_FILENAME}" | cut -d ' ' -f 1)"
package_size="$(stat -c '%s' -- "${BIOSEQ_PACKAGE_FILENAME}")"
packages_sha256="$(sha256sum -- Packages | cut -d ' ' -f 1)"
packages_size="$(stat -c '%s' -- Packages)"
packages_gz_sha256="$(sha256sum -- Packages.gz | cut -d ' ' -f 1)"
packages_gz_size="$(stat -c '%s' -- Packages.gz)"

{
  printf 'Origin: BioSeq\n'
  printf 'Label: BioSeq\n'
  printf 'Suite: stable\n'
  printf 'Codename: stable\n'
  printf 'Version: %s\n' "${BIOSEQ_PACKAGE_VERSION}"
  printf 'Architectures: %s\n' "${BIOSEQ_DEBIAN_ARCHITECTURE}"
  printf 'Components: main\n'
  printf 'Description: Native BioSeq PostgreSQL packages\n'
  printf 'SHA256:\n'
  printf ' %s %s %s\n' \
    "${package_sha256}" "${package_size}" "${BIOSEQ_PACKAGE_FILENAME}"
  printf ' %s %s Packages\n' "${packages_sha256}" "${packages_size}"
  printf ' %s %s Packages.gz\n' \
    "${packages_gz_sha256}" "${packages_gz_size}"
} > Release

gpg_home="$(mktemp -d /tmp/bioseq-apt-gpg.XXXXXXXX)"
private_key="${gpg_home}/repository-private.asc"
cleanup() {
  unset private_key_value GPG_PRIVATE_KEY || true
  rm -rf -- "${gpg_home}"
}
trap cleanup EXIT INT TERM HUP
chmod 0700 "${gpg_home}"
printf '%s' "${private_key_value}" | base64 --decode > "${private_key}"
unset private_key_value
chmod 0600 "${private_key}"
gpg --batch --no-tty --homedir "${gpg_home}" --import "${private_key}" \
  >/dev/null 2>&1
rm -f -- "${private_key}"

actual_fingerprint="$({
  gpg --batch --no-tty --homedir "${gpg_home}" --with-colons \
    --list-secret-keys "${BIOSEQ_SIGNING_FINGERPRINT}"
} | awk -F: '$1 == "sec" { primary = 1; next } primary && $1 == "fpr" { print $10; exit }')"
if [[ "${actual_fingerprint}" != "${BIOSEQ_SIGNING_FINGERPRINT}" ]]; then
  printf 'Imported repository key does not match the pinned fingerprint\n' >&2
  exit 66
fi

gpg --batch --no-tty --homedir "${gpg_home}" --armor \
  --export "${BIOSEQ_SIGNING_FINGERPRINT}" > Release.key
gpg --batch --no-tty --homedir "${gpg_home}" --yes --armor \
  --local-user "${BIOSEQ_SIGNING_FINGERPRINT}" \
  --detach-sign --output Release.gpg Release
gpg --batch --no-tty --homedir "${gpg_home}" --yes --armor \
  --local-user "${BIOSEQ_SIGNING_FINGERPRINT}" \
  --clearsign --output InRelease Release
"""

_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_REPOSITORY_TARGET = PurePosixPath("/repo")


def _debian_architecture(cell: CellContext) -> str:
    architecture = str(getattr(cell.request.architecture, "value", cell.request.architecture))
    try:
        return {"x64": "amd64", "arm64": "arm64"}[architecture]
    except KeyError as error:
        raise RepositoryBuildError(
            f"unsupported APT repository architecture: {architecture}"
        ) from error


@contextmanager
def repository_environment(
    cell: CellContext,
    *,
    artifact_name: str,
    signing: SigningMaterial,
) -> Iterator[dict[str, str]]:
    """Yield and then clear the repository container's short-lived environment."""

    pinned = require_pinned_fingerprint(signing.fingerprint)
    with signing_repository_environment(signing) as environment:
        if set(environment) != {SIGNING_ENVIRONMENT_NAME} or not environment.get(
            SIGNING_ENVIRONMENT_NAME
        ):
            raise RepositoryBuildError(
                "signing provider did not yield exactly one private-key value"
            )
        environment.update(
            {
                "BIOSEQ_DEBIAN_ARCHITECTURE": _debian_architecture(cell),
                "BIOSEQ_PACKAGE_FILENAME": artifact_name,
                "BIOSEQ_PACKAGE_VERSION": cell.package_version,
                "BIOSEQ_SIGNING_FINGERPRINT": pinned,
            }
        )
        try:
            yield environment
        finally:
            # The signing context clears the mapping too; explicitly remove the
            # public additions here so no expanded environment can be retained.
            for name in tuple(environment):
                if name != SIGNING_ENVIRONMENT_NAME:
                    environment.pop(name, None)


def repository_plan(
    cell: CellContext,
    *,
    artifact_name: str,
    signing: SigningMaterial,
) -> ContainerPlan:
    """Create the no-capability APT metadata/signing container plan."""

    if cell.distro.package_format is not PackageFormat.DEB:
        raise RepositoryBuildError(
            f"APT repository backend cannot publish {cell.distro.package_format}"
        )
    if artifact_name != Path(artifact_name).name or not artifact_name.endswith(".deb"):
        raise RepositoryBuildError("DEB artifact has an invalid repository filename")
    require_pinned_fingerprint(signing.fingerprint)
    environment_names = (
        "BIOSEQ_DEBIAN_ARCHITECTURE",
        "BIOSEQ_PACKAGE_FILENAME",
        "BIOSEQ_PACKAGE_VERSION",
        "BIOSEQ_SIGNING_FINGERPRINT",
        SIGNING_ENVIRONMENT_NAME,
    )
    return ContainerPlan(
        image=cell.distro.image_for(cell.request.architecture),
        argv=("/bin/bash", "-c", APT_REPOSITORY_SCRIPT),
        mounts=(
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
            f"{cell.distro.distro_id.value}-deb-repository"
        ),
        purpose=ContainerPurpose.REPOSITORY,
    )


def _parse_control_fields(path: Path) -> dict[str, str]:
    paragraphs = [
        paragraph for paragraph in path.read_text(encoding="utf-8").split("\n\n")
        if paragraph.strip()
    ]
    if len(paragraphs) != 1:
        raise RepositoryBuildError(
            f"APT Packages must contain exactly one paragraph; found {len(paragraphs)}"
        )
    fields: dict[str, str] = {}
    current: str | None = None
    for line in paragraphs[0].splitlines():
        if line.startswith((" ", "\t")):
            if current is None:
                raise RepositoryBuildError("APT Packages has an orphan continuation")
            fields[current] += "\n" + line
            continue
        name, separator, value = line.partition(":")
        if not separator or not name:
            raise RepositoryBuildError(f"malformed APT Packages line: {line!r}")
        current = name
        fields[name] = value.lstrip()
    return fields


def _release_sha256_entries(path: Path) -> dict[str, tuple[str, int]]:
    entries: dict[str, tuple[str, int]] = {}
    in_sha256 = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line == "SHA256:":
            in_sha256 = True
            continue
        if in_sha256 and not line.startswith(" "):
            break
        if not in_sha256:
            continue
        fields = line.split()
        if len(fields) != 3 or not _SHA256_RE.fullmatch(fields[0]) or not fields[1].isdigit():
            raise RepositoryBuildError(f"malformed APT Release SHA256 entry: {line!r}")
        if fields[2] in entries:
            raise RepositoryBuildError(f"duplicate APT Release path: {fields[2]}")
        entries[fields[2]] = (fields[0], int(fields[1]))
    return entries


def _validate_repository(root: Path, package_path: Path) -> tuple[Path, ...]:
    stable = root / "stable"
    expected_names = {
        package_path.name,
        "Packages",
        "Packages.gz",
        "Release.key",
        "Release",
        "Release.gpg",
        "InRelease",
    }
    actual_names = {path.name for path in stable.iterdir()}
    if actual_names != expected_names:
        raise RepositoryBuildError(
            "APT stable repository has unexpected objects: "
            f"{sorted(actual_names ^ expected_names)}"
        )
    paths = tuple(stable / name for name in sorted(expected_names))
    for path in paths:
        if path.is_symlink() or not path.is_file() or path.stat().st_size <= 0:
            raise RepositoryBuildError(f"APT repository object is invalid: {path}")

    packages = stable / "Packages"
    packages_fields = _parse_control_fields(packages)
    if packages_fields.get("Filename") != package_path.name:
        raise RepositoryBuildError("APT Packages does not name the retained DEB")
    expected_package = (sha256_file(package_path), package_path.stat().st_size)
    if (
        packages_fields.get("SHA256"),
        int(packages_fields.get("Size", "-1")),
    ) != expected_package:
        raise RepositoryBuildError("APT Packages does not bind the retained DEB bytes")

    compressed = stable / "Packages.gz"
    header = compressed.read_bytes()[:10]
    if len(header) != 10 or header[:3] != b"\x1f\x8b\x08" or header[4:8] != b"\0\0\0\0":
        raise RepositoryBuildError("Packages.gz was not generated with gzip -n")

    release = stable / "Release"
    release_entries = _release_sha256_entries(release)
    expected_release_paths = {package_path.name, "Packages", "Packages.gz"}
    if set(release_entries) != expected_release_paths:
        raise RepositoryBuildError("APT Release has an unexpected SHA256 path set")
    for relative in sorted(expected_release_paths):
        payload = stable / relative
        if release_entries[relative] != (sha256_file(payload), payload.stat().st_size):
            raise RepositoryBuildError(
                f"APT Release does not bind the exact {relative} bytes"
            )
    return paths


def build_repository(
    cell: CellContext,
    artifact: Path,
    *,
    signing: SigningMaterial,
    containers: ContainerExecutor,
    workspace: WorkspaceService,
) -> RepositoryBuild:
    """Build one fresh deterministic APT tree and sign its stable Release."""

    source_artifact = artifact.resolve()
    if (
        source_artifact.suffix != ".deb"
        or not source_artifact.is_file()
        or source_artifact.stat().st_size <= 0
    ):
        raise RepositoryBuildError(f"DEB artifact is missing or invalid: {artifact}")
    if source_artifact.name != artifact.name or "/" in source_artifact.name:
        raise RepositoryBuildError(f"unsafe DEB artifact name: {artifact.name}")

    root = workspace.prepare_fresh_directory(cell.paths.repository_root)
    if root.resolve() != cell.paths.repository_root.resolve():
        raise RepositoryBuildError("workspace returned the wrong APT repository root")
    stable = root / "stable"
    stable.mkdir(mode=0o755)
    package_path = stable / source_artifact.name
    shutil.copyfile(source_artifact, package_path)
    package_path.chmod(0o644)
    if (
        package_path.stat().st_size != source_artifact.stat().st_size
        or sha256_file(package_path) != sha256_file(source_artifact)
    ):
        raise RepositoryBuildError("APT repository copy changed the DEB artifact bytes")

    plan = repository_plan(
        cell,
        artifact_name=package_path.name,
        signing=signing,
    )
    with repository_environment(
        cell,
        artifact_name=package_path.name,
        signing=signing,
    ) as environment:
        containers.run(plan, environment=environment)
    metadata_paths = _validate_repository(root, package_path)

    signature_paths = tuple(
        stable / name
        for name in ("Release.key", "Release", "Release.gpg", "InRelease")
    )
    return RepositoryBuild(
        # The publisher maps this root directly onto the cell's ``stable/``
        # S3 prefix; returning the parent would create ``stable/stable``.
        root=stable,
        package_path=package_path,
        signature_paths=signature_paths,
        metadata_paths=metadata_paths,
    )
