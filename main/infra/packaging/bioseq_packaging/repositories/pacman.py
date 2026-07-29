"""Fresh signed PACMAN repository construction for one native package."""

from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
import hashlib
from pathlib import Path, PurePosixPath
import shutil
import tarfile

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
    SIGNING_ENVIRONMENT_NAME,
    SigningError,
    SigningMaterial,
    repository_environment as signing_repository_environment,
    require_pinned_fingerprint,
    verify_with_gpgv,
)
from . import RepositoryBuild, RepositoryBuildError


PACMAN_REPOSITORY_SCRIPT = r"""set -Eeuo pipefail
private_key_value="${GPG_PRIVATE_KEY:?GPG_PRIVATE_KEY is required}"
unset GPG_PRIVATE_KEY
export -n private_key_value

signing_home="$(mktemp -d /tmp/bioseq-pacman-signing.XXXXXXXX)"
private_key_file="${signing_home}/private-key.asc"
chmod 0700 "${signing_home}"
export GNUPGHOME="${signing_home}"
cleanup_repository_signing() {
  original_status=$?
  trap - EXIT HUP INT TERM
  unset private_key_value GPG_PRIVATE_KEY
  cleanup_failed=0
  if ! rm -f -- "${private_key_file}"; then
    cleanup_failed=1
  fi
  if ! rm -rf -- "${signing_home}"; then
    cleanup_failed=1
  fi
  if (( cleanup_failed != 0 )); then
    if (( original_status != 0 )); then
      printf 'PACMAN repository work failed with status %s and signing cleanup failed\n' \
        "${original_status}" >&2
    else
      printf 'PACMAN repository signing cleanup failed\n' >&2
    fi
    exit 70
  fi
  exit "${original_status}"
}
trap cleanup_repository_signing EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

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

pacman -Syu --noconfirm --needed gnupg

printf '%s' "${private_key_value}" | base64 --decode > "${private_key_file}"
unset private_key_value
chmod 0600 "${private_key_file}"
gpg --batch --no-tty --homedir "${signing_home}" \
  --import "${private_key_file}" >/dev/null 2>&1
rm -f -- "${private_key_file}"

secret_key_listing="$(
  gpg --batch --no-tty --homedir "${signing_home}" \
    --with-colons --list-secret-keys
)"
mapfile -t primary_fingerprints < <(
  printf '%s\n' "${secret_key_listing}" | awk -F: '
    $1 == "sec" { primary = 1; next }
    $1 == "ssb" { primary = 0; next }
    primary && $1 == "fpr" { print $10; primary = 0 }
  '
)
unset secret_key_listing
if (( ${#primary_fingerprints[@]} != 1 )) \
    || [[ "${primary_fingerprints[0]}" != "${BIOSEQ_SIGNING_FINGERPRINT}" ]]; then
  printf 'Imported PACMAN signing key does not match the pinned fingerprint\n' >&2
  exit 66
fi

repository_root=/repo
cd "${repository_root}"
shopt -s nullglob
packages=(*.pkg.tar.zst)
if (( ${#packages[@]} != 1 )); then
  printf 'Expected exactly one retained PACMAN package; found %s\n' \
    "${#packages[@]}" >&2
  exit 65
fi
if [[ "${packages[0]}" != "${BIOSEQ_PACKAGE_FILENAME}" ]]; then
  printf 'Unexpected retained PACMAN package filename\n' >&2
  exit 65
fi

gpg --batch --no-tty --homedir "${signing_home}" --yes \
  --local-user "${BIOSEQ_SIGNING_FINGERPRINT}" \
  --detach-sign --no-armor \
  --output "${BIOSEQ_PACKAGE_FILENAME}.sig" \
  "${BIOSEQ_PACKAGE_FILENAME}"
gpg --batch --no-tty --homedir "${signing_home}" --armor \
  --export "${BIOSEQ_SIGNING_FINGERPRINT}" > public.key

# The fresh root is proven to contain exactly one package above.
# shellcheck disable=SC2035
repo-add --sign --verify --key "${BIOSEQ_SIGNING_FINGERPRINT}" \
  "${BIOSEQ_REPOSITORY_NAME}.db.tar.gz" *.pkg.tar.zst

materialize_alias() {
  canonical_path="$1"
  alias_path="$2"
  if [[ ! -f "${canonical_path}" || -L "${canonical_path}" ]]; then
    printf 'PACMAN canonical repository object is missing or a symlink\n' >&2
    return 1
  fi
  temporary_alias="$(mktemp "${alias_path}.tmp.XXXXXXXX")"
  cp -- "${canonical_path}" "${temporary_alias}"
  mv -fT -- "${temporary_alias}" "${alias_path}"
  if [[ ! -f "${alias_path}" || -L "${alias_path}" ]]; then
    printf 'PACMAN consumer alias was not materialized as a regular file\n' >&2
    return 1
  fi
}

database="${BIOSEQ_REPOSITORY_NAME}.db.tar.gz"
database_signature="${database}.sig"
files="${BIOSEQ_REPOSITORY_NAME}.files.tar.gz"
files_signature="${files}.sig"
materialize_alias "${database}" "${BIOSEQ_REPOSITORY_NAME}.db"
materialize_alias "${database_signature}" "${BIOSEQ_REPOSITORY_NAME}.db.sig"
materialize_alias "${files}" "${BIOSEQ_REPOSITORY_NAME}.files"
materialize_alias "${files_signature}" "${BIOSEQ_REPOSITORY_NAME}.files.sig"
"""


def _enum_value(value: object) -> str:
    candidate = getattr(value, "value", value)
    return str(candidate)


def _pacman_architecture(cell: CellContext) -> str:
    architecture = _enum_value(cell.request.architecture)
    try:
        return {"x64": "x86_64", "arm64": "aarch64"}[architecture]
    except KeyError as error:
        raise RepositoryBuildError(
            f"unsupported PACMAN architecture: {architecture}"
        ) from error


def _expected_artifact_name(cell: CellContext) -> str:
    return (
        f"{cell.package_name}-{cell.package_version}-1-"
        f"{_pacman_architecture(cell)}.pkg.tar.zst"
    )


def _validated_fingerprint(signing: SigningMaterial) -> str:
    try:
        return require_pinned_fingerprint(signing.fingerprint)
    except SigningError as error:
        raise RepositoryBuildError(
            "PACMAN signing fingerprint is not the pinned BioSeq key"
        ) from error


@contextmanager
def repository_environment(
    cell: CellContext,
    artifact: Path,
    signing: SigningMaterial,
) -> Iterator[dict[str, str]]:
    """Yield the short-lived repository environment and clear its key copy."""

    fingerprint = _validated_fingerprint(signing)
    public_values = {
        "BIOSEQ_PACKAGE_FILENAME": artifact.name,
        "BIOSEQ_REPOSITORY_NAME": cell.package_name,
        "BIOSEQ_SIGNING_FINGERPRINT": fingerprint,
    }
    with signing_repository_environment(signing) as sensitive_values:
        if set(sensitive_values) != {SIGNING_ENVIRONMENT_NAME} or not sensitive_values[
            SIGNING_ENVIRONMENT_NAME
        ]:
            raise RepositoryBuildError(
                "signing provider did not yield one private-key environment value"
            )
        environment = {**public_values, **sensitive_values}
        try:
            yield environment
        finally:
            if SIGNING_ENVIRONMENT_NAME in environment:
                environment[SIGNING_ENVIRONMENT_NAME] = "<cleared>"
            environment.clear()


def repository_plan(
    cell: CellContext,
    artifact: Path,
    signing: SigningMaterial,
) -> ContainerPlan:
    """Create the no-capability PACMAN signing/repository container plan."""

    if cell.distro.package_format is not PackageFormat.PACMAN:
        raise RepositoryBuildError(
            f"PACMAN repository cannot publish {cell.distro.package_format}"
        )
    if artifact.name != _expected_artifact_name(cell):
        raise RepositoryBuildError(
            f"unexpected PACMAN artifact name: {artifact.name}"
        )
    _validated_fingerprint(signing)
    environment_names = tuple(
        sorted(
            (
                "BIOSEQ_PACKAGE_FILENAME",
                "BIOSEQ_REPOSITORY_NAME",
                "BIOSEQ_SIGNING_FINGERPRINT",
                SIGNING_ENVIRONMENT_NAME,
            )
        )
    )
    return ContainerPlan(
        image=cell.distro.image_for(cell.request.architecture),
        argv=("/bin/bash", "-c", PACMAN_REPOSITORY_SCRIPT),
        mounts=(
            BindMount(
                source=cell.paths.repository_root,
                target=PurePosixPath("/repo"),
                read_only=False,
            ),
        ),
        environment_names=environment_names,
        capabilities=frozenset(),
        output=cell.request.container_output,
        name=(
            f"bioseq-{cell.request.architecture.value}-pg{cell.postgres_major}-"
            f"{cell.distro.distro_id.value}-pacman-repository"
        ),
        purpose=ContainerPurpose.REPOSITORY,
    )


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _database_fields(database: Path) -> dict[str, tuple[str, ...]]:
    try:
        with tarfile.open(database, "r:*") as archive:
            descriptions = [
                member
                for member in archive.getmembers()
                if member.isfile() and PurePosixPath(member.name).name == "desc"
            ]
            if len(descriptions) != 1:
                raise RepositoryBuildError(
                    "PACMAN database must contain exactly one package description"
                )
            handle = archive.extractfile(descriptions[0])
            if handle is None:
                raise RepositoryBuildError("PACMAN database description is unreadable")
            payload = handle.read().decode("utf-8")
    except (OSError, tarfile.TarError, UnicodeDecodeError) as error:
        raise RepositoryBuildError("PACMAN database is malformed") from error

    fields: dict[str, tuple[str, ...]] = {}
    lines = payload.splitlines()
    index = 0
    while index < len(lines):
        marker = lines[index]
        index += 1
        if not (marker.startswith("%") and marker.endswith("%") and len(marker) > 2):
            if marker:
                raise RepositoryBuildError("PACMAN database has malformed fields")
            continue
        name = marker[1:-1]
        values: list[str] = []
        while index < len(lines) and lines[index]:
            values.append(lines[index])
            index += 1
        if name in fields or not values:
            raise RepositoryBuildError("PACMAN database has duplicate or empty fields")
        fields[name] = tuple(values)
    return fields


def _single_field(fields: dict[str, tuple[str, ...]], name: str) -> str:
    values = fields.get(name, ())
    if len(values) != 1:
        raise RepositoryBuildError(
            f"PACMAN database field {name} must contain exactly one value"
        )
    return values[0]


def _verify_database_binding(
    cell: CellContext,
    database: Path,
    package_path: Path,
) -> None:
    fields = _database_fields(database)
    expected = {
        "NAME": cell.package_name,
        "FILENAME": package_path.name,
        "VERSION": f"{cell.package_version}-1",
        "ARCH": _pacman_architecture(cell),
        "CSIZE": str(package_path.stat().st_size),
        "SHA256SUM": _sha256(package_path),
    }
    for field, value in expected.items():
        if _single_field(fields, field) != value:
            raise RepositoryBuildError(
                f"PACMAN database does not bind the exact package {field}"
            )


def _repository_result(cell: CellContext, package_path: Path) -> RepositoryBuild:
    root = cell.paths.repository_root
    repository_name = cell.package_name
    public_key = root / "public.key"
    package_signature = root / f"{package_path.name}.sig"
    database = root / f"{repository_name}.db.tar.gz"
    database_signature = root / f"{repository_name}.db.tar.gz.sig"
    files = root / f"{repository_name}.files.tar.gz"
    files_signature = root / f"{repository_name}.files.tar.gz.sig"
    aliases = (
        root / f"{repository_name}.db",
        root / f"{repository_name}.db.sig",
        root / f"{repository_name}.files",
        root / f"{repository_name}.files.sig",
    )
    expected_paths = (
        package_path,
        package_signature,
        public_key,
        database,
        database_signature,
        files,
        files_signature,
        *aliases,
    )
    expected_names = {path.name for path in expected_paths}
    actual_names = {path.name for path in root.iterdir()}
    if actual_names != expected_names:
        raise RepositoryBuildError(
            "PACMAN repository has unexpected objects: "
            f"{sorted(actual_names ^ expected_names)}"
        )
    for path in expected_paths:
        if path.is_symlink() or not path.is_file() or path.stat().st_size <= 0:
            raise RepositoryBuildError(
                f"PACMAN repository object is invalid: {path.name}"
            )
    alias_pairs = (
        (database, aliases[0]),
        (database_signature, aliases[1]),
        (files, aliases[2]),
        (files_signature, aliases[3]),
    )
    for canonical, alias in alias_pairs:
        if canonical.stat().st_size != alias.stat().st_size or _sha256(
            canonical
        ) != _sha256(alias):
            raise RepositoryBuildError(
                f"PACMAN alias does not match canonical bytes: {alias.name}"
            )

    try:
        verify_with_gpgv(
            public_key=public_key,
            signatures=(
                (package_signature, package_path),
                (database_signature, database),
                (files_signature, files),
            ),
            expected_fingerprint=PINNED_SIGNING_FINGERPRINT,
        )
    except SigningError as error:
        raise RepositoryBuildError(
            "PACMAN package/database/files signature verification failed"
        ) from error
    _verify_database_binding(cell, database, package_path)

    signature_paths = (
        public_key,
        package_signature,
        database,
        database_signature,
    )
    metadata_paths = (files, files_signature, *aliases)
    return RepositoryBuild(
        root=root,
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
    """Copy, sign, index, verify, and return one fresh PACMAN repository."""

    if artifact.is_symlink():
        raise RepositoryBuildError("PACMAN repository received a symlink artifact")
    source_artifact = artifact.resolve()
    if (
        source_artifact.name != _expected_artifact_name(cell)
        or not source_artifact.is_file()
        or source_artifact.stat().st_size <= 0
    ):
        raise RepositoryBuildError(
            f"PACMAN artifact is missing, empty, or noncanonical: {artifact}"
        )

    root = workspace.prepare_fresh_directory(cell.paths.repository_root)
    if root.resolve() != cell.paths.repository_root.resolve():
        raise RepositoryBuildError("workspace returned the wrong PACMAN repository root")
    package_path = root / source_artifact.name
    shutil.copy2(source_artifact, package_path)
    if (
        package_path.stat().st_size != source_artifact.stat().st_size
        or _sha256(package_path) != _sha256(source_artifact)
    ):
        raise RepositoryBuildError("PACMAN repository copy changed the artifact bytes")

    plan = repository_plan(cell, package_path, signing)
    with repository_environment(cell, package_path, signing) as environment:
        containers.run(plan, environment=environment)
    return _repository_result(cell, package_path)
