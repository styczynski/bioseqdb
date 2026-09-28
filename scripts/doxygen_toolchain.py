"""Provision the exact Doxygen release used by the managed website mode."""

from __future__ import annotations

import argparse
import contextlib
import fcntl
import hashlib
import json
import os
import platform
import re
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
from collections.abc import Iterator, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO
from urllib.parse import urlsplit

from integrity import sha256_file
from publication import durable_replace_stream, fsync_directory


EXACT_DOXYGEN_VERSION = "1.9.8"
REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DOXYGEN_CACHE_ROOT = (
    REPOSITORY_ROOT / "build" / "toolchains" / "doxygen"
)
DOXYGEN_TOOLCHAIN_LOCK = Path(__file__).with_name(
    "doxygen_toolchain.lock.json"
)

_SUPPORTED_HOST_KEYS = frozenset({
    "darwin-arm64",
    "darwin-x86_64",
    "linux-x86_64",
})
_ASSET_FIELDS = frozenset({
    "archive_sha256",
    "archive_size_bytes",
    "archive_type",
    "archive_url",
    "executable_architecture",
    "executable_member",
    "executable_sha256",
    "executable_size_bytes",
    "version_output",
})
_SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
_DOWNLOAD_CHUNK_BYTES = 1024 * 1024
_DOWNLOAD_TIMEOUT_SECONDS = 120.0
_VERSION_TIMEOUT_SECONDS = 10.0
_MAX_LOCK_BYTES = 1024 * 1024


class DoxygenProvisioningError(RuntimeError):
    """Report a closed failure to provision or validate exact Doxygen."""


@dataclass(frozen=True, slots=True)
class DoxygenAsset:
    """Describe one immutable release artifact and its executable member."""

    host_key: str
    archive_sha256: str
    archive_size_bytes: int
    archive_type: str
    archive_url: str
    executable_architecture: str
    executable_member: str
    executable_sha256: str
    executable_size_bytes: int
    version_output: str


@dataclass(frozen=True, slots=True)
class ProvisionedDoxygen:
    """Describe the validated executable selected for one website build."""

    executable: Path
    version: str
    version_output: str
    platform_name: str
    architecture: str
    archive_sha256: str
    executable_sha256: str
    cache_hit: bool

    def configure_identity(self) -> dict[str, object]:
        """Return the closed identity sealed into the CMake configure stamp."""
        return {
            "archive_sha256": self.archive_sha256,
            "architecture": self.architecture,
            "executable_path": str(self.executable),
            "executable_sha256": self.executable_sha256,
            "name": "doxygen",
            "platform": self.platform_name,
            "version": self.version,
            "version_output": self.version_output,
        }


def _require_mapping(value: object, label: str) -> Mapping[str, object]:
    """Return a JSON object or reject the malformed lock member."""
    if not isinstance(value, dict):
        raise DoxygenProvisioningError(f"{label} must be a JSON object")
    return value


def _require_positive_integer(value: object, label: str) -> int:
    """Return a positive non-boolean integer from the lock document."""
    if type(value) is not int or value <= 0:
        raise DoxygenProvisioningError(f"{label} must be a positive integer")
    return value


def _require_string(value: object, label: str) -> str:
    """Return a nonempty string from the lock document."""
    if not isinstance(value, str) or not value:
        raise DoxygenProvisioningError(f"{label} must be a nonempty string")
    return value


def _validate_member_name(value: str, label: str) -> None:
    """Reject absolute, aliased, or traversal-bearing archive member names."""
    if (
        value.startswith("/")
        or value.endswith("/")
        or "\\" in value
        or any(part in {"", ".", ".."} for part in value.split("/"))
    ):
        raise DoxygenProvisioningError(
            f"{label} must be a normalized relative archive member"
        )


def _parse_asset(host_key: str, value: object) -> DoxygenAsset:
    """Parse and validate one closed platform entry from the lock."""
    payload = _require_mapping(value, f"platform {host_key}")
    unknown = set(payload) - _ASSET_FIELDS
    missing = _ASSET_FIELDS - set(payload)
    if unknown or missing:
        raise DoxygenProvisioningError(
            f"platform {host_key} has invalid fields: "
            f"missing={sorted(missing)} unknown={sorted(unknown)}"
        )

    archive_sha256 = _require_string(
        payload["archive_sha256"], f"platform {host_key} archive_sha256"
    )
    executable_sha256 = _require_string(
        payload["executable_sha256"],
        f"platform {host_key} executable_sha256",
    )
    if not _SHA256_PATTERN.fullmatch(archive_sha256):
        raise DoxygenProvisioningError(
            f"platform {host_key} archive_sha256 is not lowercase SHA-256"
        )
    if not _SHA256_PATTERN.fullmatch(executable_sha256):
        raise DoxygenProvisioningError(
            f"platform {host_key} executable_sha256 is not lowercase SHA-256"
        )

    archive_url = _require_string(
        payload["archive_url"], f"platform {host_key} archive_url"
    )
    parsed_url = urlsplit(archive_url)
    if (
        parsed_url.scheme != "https"
        or parsed_url.hostname != "github.com"
        or parsed_url.username is not None
        or parsed_url.password is not None
    ):
        raise DoxygenProvisioningError(
            f"platform {host_key} archive_url must be an unauthenticated "
            "HTTPS github.com release URL"
        )

    archive_type = _require_string(
        payload["archive_type"], f"platform {host_key} archive_type"
    )
    expected_archive_type = "dmg" if host_key.startswith("darwin-") else "tar_gz"
    if archive_type != expected_archive_type:
        raise DoxygenProvisioningError(
            f"platform {host_key} must use archive_type {expected_archive_type}"
        )

    executable_member = _require_string(
        payload["executable_member"],
        f"platform {host_key} executable_member",
    )
    _validate_member_name(
        executable_member, f"platform {host_key} executable_member"
    )
    executable_architecture = _require_string(
        payload["executable_architecture"],
        f"platform {host_key} executable_architecture",
    )
    if executable_architecture != "x86_64":
        raise DoxygenProvisioningError(
            f"platform {host_key} executable architecture must be x86_64"
        )
    version_output = _require_string(
        payload["version_output"], f"platform {host_key} version_output"
    )
    if version_output.split(maxsplit=1)[0] != EXACT_DOXYGEN_VERSION:
        raise DoxygenProvisioningError(
            f"platform {host_key} version output is not exact "
            f"Doxygen {EXACT_DOXYGEN_VERSION}"
        )

    return DoxygenAsset(
        host_key=host_key,
        archive_sha256=archive_sha256,
        archive_size_bytes=_require_positive_integer(
            payload["archive_size_bytes"],
            f"platform {host_key} archive_size_bytes",
        ),
        archive_type=archive_type,
        archive_url=archive_url,
        executable_architecture=executable_architecture,
        executable_member=executable_member,
        executable_sha256=executable_sha256,
        executable_size_bytes=_require_positive_integer(
            payload["executable_size_bytes"],
            f"platform {host_key} executable_size_bytes",
        ),
        version_output=version_output,
    )


def load_doxygen_toolchain_lock(
    lock_path: Path = DOXYGEN_TOOLCHAIN_LOCK,
) -> dict[str, DoxygenAsset]:
    """Load the closed exact-version toolchain lock from a regular file.

    @param lock_path Tracked JSON lock containing immutable release identities.
    @return Platform key to validated release artifact.
    @throws DoxygenProvisioningError If the lock or any member is malformed.
    """
    descriptor = -1
    try:
        path_metadata = lock_path.lstat()
        if lock_path.is_symlink() or not stat.S_ISREG(path_metadata.st_mode):
            raise DoxygenProvisioningError(
                f"Doxygen toolchain lock must be a regular non-symlink file: "
                f"{lock_path}"
            )
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(lock_path, flags)
        before = os.fstat(descriptor)
        if (
            not stat.S_ISREG(before.st_mode)
            or before.st_nlink != 1
            or before.st_size <= 0
            or before.st_size > _MAX_LOCK_BYTES
        ):
            raise DoxygenProvisioningError(
                "Doxygen toolchain lock must be a nonempty, singly-linked "
                f"regular file no larger than {_MAX_LOCK_BYTES} bytes"
            )
        chunks: list[bytes] = []
        remaining = before.st_size
        while remaining:
            chunk = os.read(descriptor, min(remaining, 64 * 1024))
            if not chunk:
                raise DoxygenProvisioningError(
                    "Doxygen toolchain lock ended before its recorded size"
                )
            chunks.append(chunk)
            remaining -= len(chunk)
        after = os.fstat(descriptor)
        final_path_metadata = lock_path.lstat()
        identity_before = (
            before.st_dev,
            before.st_ino,
            before.st_mode,
            before.st_nlink,
            before.st_size,
            before.st_mtime_ns,
            before.st_ctime_ns,
        )
        identity_after = (
            after.st_dev,
            after.st_ino,
            after.st_mode,
            after.st_nlink,
            after.st_size,
            after.st_mtime_ns,
            after.st_ctime_ns,
        )
        identity_path = (
            final_path_metadata.st_dev,
            final_path_metadata.st_ino,
            final_path_metadata.st_mode,
            final_path_metadata.st_nlink,
            final_path_metadata.st_size,
            final_path_metadata.st_mtime_ns,
            final_path_metadata.st_ctime_ns,
        )
        if (
            identity_before != identity_after
            or identity_after != identity_path
            or stat.S_ISLNK(final_path_metadata.st_mode)
        ):
            raise DoxygenProvisioningError(
                "Doxygen toolchain lock changed while it was read"
            )
        payload = json.loads(b"".join(chunks).decode("utf-8", errors="strict"))
    except DoxygenProvisioningError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise DoxygenProvisioningError(
            f"cannot read Doxygen toolchain lock {lock_path}: {exc}"
        ) from exc
    finally:
        if descriptor >= 0:
            os.close(descriptor)

    root = _require_mapping(payload, "Doxygen toolchain lock")
    if set(root) != {"platforms", "schema_revision", "version"}:
        raise DoxygenProvisioningError(
            "Doxygen toolchain lock must contain exactly platforms, "
            "schema_revision, and version"
        )
    if root["schema_revision"] != 1 or type(root["schema_revision"]) is not int:
        raise DoxygenProvisioningError(
            "Doxygen toolchain lock schema_revision must equal 1"
        )
    if root["version"] != EXACT_DOXYGEN_VERSION:
        raise DoxygenProvisioningError(
            f"Doxygen toolchain lock version must equal {EXACT_DOXYGEN_VERSION}"
        )
    platforms = _require_mapping(root["platforms"], "platforms")
    if set(platforms) != _SUPPORTED_HOST_KEYS:
        raise DoxygenProvisioningError(
            "Doxygen toolchain lock platforms must equal "
            f"{sorted(_SUPPORTED_HOST_KEYS)}"
        )
    return {
        host_key: _parse_asset(host_key, platforms[host_key])
        for host_key in sorted(platforms)
    }


def _normalize_host(platform_name: str, machine_name: str) -> tuple[str, str, str]:
    """Normalize supported Darwin/Linux platform and architecture spellings."""
    normalized_platform = platform_name.strip().lower()
    if normalized_platform.startswith("linux"):
        normalized_platform = "linux"
    elif normalized_platform == "darwin":
        normalized_platform = "darwin"
    else:
        raise DoxygenProvisioningError(
            f"Doxygen {EXACT_DOXYGEN_VERSION} provisioning supports only "
            f"Darwin and Linux; observed {platform_name!r}"
        )

    normalized_machine = machine_name.strip().lower()
    machine_aliases = {
        "amd64": "x86_64",
        "x64": "x86_64",
        "aarch64": "arm64",
    }
    normalized_machine = machine_aliases.get(
        normalized_machine, normalized_machine
    )
    host_key = f"{normalized_platform}-{normalized_machine}"
    if host_key not in _SUPPORTED_HOST_KEYS:
        raise DoxygenProvisioningError(
            f"no locked Doxygen {EXACT_DOXYGEN_VERSION} artifact exists for "
            f"{host_key}; supported hosts are {sorted(_SUPPORTED_HOST_KEYS)}"
        )
    return normalized_platform, normalized_machine, host_key


def _select_asset(
    *,
    platform_name: str,
    machine_name: str,
    lock_path: Path = DOXYGEN_TOOLCHAIN_LOCK,
) -> tuple[str, str, DoxygenAsset]:
    """Select the immutable artifact for one normalized execution host."""
    normalized_platform, normalized_machine, host_key = _normalize_host(
        platform_name, machine_name
    )
    return (
        normalized_platform,
        normalized_machine,
        load_doxygen_toolchain_lock(lock_path)[host_key],
    )


def _ensure_private_cache_directory(path: Path) -> Path:
    """Create and validate a private, caller-owned, non-symlink cache directory."""
    try:
        path.mkdir(mode=0o700, parents=True, exist_ok=True)
        metadata = path.lstat()
    except OSError as exc:
        raise DoxygenProvisioningError(
            f"cannot create Doxygen cache directory {path}: {exc}"
        ) from exc
    if path.is_symlink() or not stat.S_ISDIR(metadata.st_mode):
        raise DoxygenProvisioningError(
            f"Doxygen cache path must be a directory, not a symlink: {path}"
        )
    if metadata.st_uid != os.geteuid() or metadata.st_mode & 0o022:
        raise DoxygenProvisioningError(
            f"Doxygen cache directory must be caller-owned and not group/world "
            f"writable: {path}"
        )
    return path.resolve(strict=True)


def _ensure_private_cache_subdirectory(
    cache_root: Path,
    *parts: str,
) -> Path:
    """Create cache descendants one component at a time without symlinks."""
    current = cache_root
    for part in parts:
        if not part or part in {".", ".."} or "/" in part or "\\" in part:
            raise DoxygenProvisioningError(
                f"invalid Doxygen cache path component {part!r}"
            )
        current = _ensure_private_cache_directory(current / part)
        try:
            current.relative_to(cache_root)
        except ValueError as exc:
            raise DoxygenProvisioningError(
                f"Doxygen cache directory escapes its root: {current}"
            ) from exc
    return current


@contextlib.contextmanager
def _provisioning_lock(cache_root: Path) -> Iterator[None]:
    """Serialize validation, download, extraction, and publication across workers."""
    lock_path = cache_root / ".provision.lock"
    flags = os.O_RDWR | os.O_CREAT | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(lock_path, flags, 0o600)
    except OSError as exc:
        raise DoxygenProvisioningError(
            f"cannot open Doxygen provisioning lock {lock_path}: {exc}"
        ) from exc
    try:
        metadata = os.fstat(descriptor)
        if (
            not stat.S_ISREG(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
            or metadata.st_mode & 0o077
        ):
            raise DoxygenProvisioningError(
                "Doxygen provisioning lock must be a private caller-owned "
                "regular file"
            )
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        yield
    finally:
        with contextlib.suppress(OSError):
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)


def _regular_file_matches(
    path: Path,
    *,
    size_bytes: int,
    sha256: str,
    executable: bool,
) -> bool:
    """Return whether a cache path has the exact regular-file identity."""
    descriptor = -1
    try:
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(path, flags)
        before = os.fstat(descriptor)
        if (
            not stat.S_ISREG(before.st_mode)
            or before.st_nlink != 1
            or before.st_uid != os.geteuid()
            or before.st_mode & 0o022
            or before.st_size != size_bytes
            or (executable and not before.st_mode & stat.S_IXUSR)
        ):
            return False
        digest = sha256_file(descriptor)
        after = os.fstat(descriptor)
        current = path.lstat()
        stable_before = (
            before.st_dev,
            before.st_ino,
            before.st_mode,
            before.st_nlink,
            before.st_size,
            before.st_mtime_ns,
            before.st_ctime_ns,
        )
        stable_after = (
            after.st_dev,
            after.st_ino,
            after.st_mode,
            after.st_nlink,
            after.st_size,
            after.st_mtime_ns,
            after.st_ctime_ns,
        )
        stable_path = (
            current.st_dev,
            current.st_ino,
            current.st_mode,
            current.st_nlink,
            current.st_size,
            current.st_mtime_ns,
            current.st_ctime_ns,
        )
        return (
            stable_before == stable_after == stable_path
            and not stat.S_ISLNK(current.st_mode)
            and digest == sha256
        )
    except OSError:
        return False
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _write_all(descriptor: int, payload: bytes) -> None:
    """Write an entire byte string to a descriptor without accepting short writes."""
    view = memoryview(payload)
    while view:
        count = os.write(descriptor, view)
        if count <= 0:
            raise OSError("short write while provisioning Doxygen")
        view = view[count:]


def _download_archive(asset: DoxygenAsset, destination: Path) -> bool:
    """Ensure the authenticated release archive exists in the content cache.

    @return ``True`` when an existing verified archive was reused.
    """
    if _regular_file_matches(
        destination,
        size_bytes=asset.archive_size_bytes,
        sha256=asset.archive_sha256,
        executable=False,
    ):
        return True
    if destination.exists() or destination.is_symlink():
        try:
            destination.unlink()
            fsync_directory(destination.parent)
        except OSError as exc:
            raise DoxygenProvisioningError(
                f"cannot remove invalid Doxygen archive cache entry "
                f"{destination}: {exc}"
            ) from exc

    request = urllib.request.Request(
        asset.archive_url,
        headers={
            "Accept": "application/octet-stream",
            "User-Agent": "bioseq-build-doxygen-provisioner/1",
        },
    )

    def write(descriptor: int) -> None:
        digest = hashlib.sha256()
        byte_count = 0
        try:
            with urllib.request.urlopen(
                request, timeout=_DOWNLOAD_TIMEOUT_SECONDS
            ) as response:
                headers = getattr(response, "headers", None)
                content_length = (
                    headers.get("Content-Length") if headers is not None else None
                )
                if (
                    content_length is not None
                    and content_length.isdigit()
                    and int(content_length) != asset.archive_size_bytes
                ):
                    raise DoxygenProvisioningError(
                        "Doxygen archive HTTP size differs from the lock: "
                        f"expected={asset.archive_size_bytes} "
                        f"observed={content_length}"
                    )
                while chunk := response.read(_DOWNLOAD_CHUNK_BYTES):
                    byte_count += len(chunk)
                    if byte_count > asset.archive_size_bytes:
                        raise DoxygenProvisioningError(
                            "Doxygen archive exceeds its locked byte count"
                        )
                    digest.update(chunk)
                    _write_all(descriptor, chunk)
        except DoxygenProvisioningError:
            raise
        except (OSError, TimeoutError, urllib.error.URLError) as exc:
            raise DoxygenProvisioningError(
                f"cannot download locked Doxygen archive: {exc}"
            ) from exc
        if byte_count != asset.archive_size_bytes:
            raise DoxygenProvisioningError(
                "Doxygen archive byte count differs from the lock: "
                f"expected={asset.archive_size_bytes} observed={byte_count}"
            )
        observed_sha256 = digest.hexdigest()
        if observed_sha256 != asset.archive_sha256:
            raise DoxygenProvisioningError(
                "Doxygen archive SHA-256 differs from the lock: "
                f"expected={asset.archive_sha256} observed={observed_sha256}"
            )

    try:
        durable_replace_stream(destination, write, mode=0o600)
    except DoxygenProvisioningError:
        raise
    except OSError as exc:
        raise DoxygenProvisioningError(
            f"cannot publish Doxygen archive cache entry {destination}: {exc}"
        ) from exc
    if not _regular_file_matches(
        destination,
        size_bytes=asset.archive_size_bytes,
        sha256=asset.archive_sha256,
        executable=False,
    ):
        raise DoxygenProvisioningError(
            "published Doxygen archive does not retain its locked identity"
        )
    return False


def _copy_verified_executable(
    source: BinaryIO,
    destination_descriptor: int,
    asset: DoxygenAsset,
) -> None:
    """Copy and authenticate the one executable member before publication."""
    digest = hashlib.sha256()
    byte_count = 0
    while chunk := source.read(_DOWNLOAD_CHUNK_BYTES):
        byte_count += len(chunk)
        if byte_count > asset.executable_size_bytes:
            raise DoxygenProvisioningError(
                "Doxygen executable exceeds its locked byte count"
            )
        digest.update(chunk)
        _write_all(destination_descriptor, chunk)
    if byte_count != asset.executable_size_bytes:
        raise DoxygenProvisioningError(
            "Doxygen executable byte count differs from the lock: "
            f"expected={asset.executable_size_bytes} observed={byte_count}"
        )
    observed_sha256 = digest.hexdigest()
    if observed_sha256 != asset.executable_sha256:
        raise DoxygenProvisioningError(
            "Doxygen executable SHA-256 differs from the lock: "
            f"expected={asset.executable_sha256} observed={observed_sha256}"
        )


def _validate_tar_member_name(name: str) -> None:
    """Reject traversal and alias spellings from any tar archive member."""
    normalized = name[:-1] if name.endswith("/") else name
    if (
        not normalized
        or normalized.startswith("/")
        or "\\" in normalized
        or any(part in {"", ".", ".."} for part in normalized.split("/"))
    ):
        raise DoxygenProvisioningError(
            f"Doxygen archive contains unsafe member {name!r}"
        )


def _extract_tar_executable(
    archive_path: Path,
    destination: Path,
    asset: DoxygenAsset,
) -> None:
    """Extract exactly one regular executable from a validated gzip tarball."""
    try:
        with tarfile.open(archive_path, mode="r:gz") as archive:
            matches: list[tarfile.TarInfo] = []
            for member in archive.getmembers():
                _validate_tar_member_name(member.name)
                if (
                    member.issym()
                    or member.islnk()
                    or member.isdev()
                    or member.isfifo()
                    or not (member.isfile() or member.isdir())
                ):
                    raise DoxygenProvisioningError(
                        "Doxygen archive contains a link, device, or unsupported "
                        f"member: {member.name}"
                    )
                if member.name == asset.executable_member:
                    matches.append(member)
            if len(matches) != 1 or not matches[0].isfile():
                raise DoxygenProvisioningError(
                    "Doxygen archive must contain its locked executable member "
                    "exactly once as a regular file"
                )
            if matches[0].size != asset.executable_size_bytes:
                raise DoxygenProvisioningError(
                    "Doxygen tar member size differs from the lock"
                )
            source = archive.extractfile(matches[0])
            if source is None:
                raise DoxygenProvisioningError(
                    "Doxygen tar member cannot be opened"
                )
            with source:
                durable_replace_stream(
                    destination,
                    lambda descriptor: _copy_verified_executable(
                        source, descriptor, asset
                    ),
                    mode=0o755,
                )
    except DoxygenProvisioningError:
        raise
    except (OSError, tarfile.TarError) as exc:
        raise DoxygenProvisioningError(
            f"cannot extract locked Doxygen tar archive: {exc}"
        ) from exc


def _require_rosetta_if_needed(
    platform_name: str,
    machine_name: str,
    asset: DoxygenAsset,
) -> None:
    """Require Rosetta before using the official x86-64 Darwin 1.9.8 image."""
    if not (
        platform_name == "darwin"
        and machine_name == "arm64"
        and asset.executable_architecture == "x86_64"
    ):
        return
    try:
        result = subprocess.run(
            ["/usr/bin/arch", "-x86_64", "/usr/bin/true"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=_VERSION_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise DoxygenProvisioningError(
            "cannot test Rosetta 2 for the official Doxygen 1.9.8 Darwin "
            f"artifact: {exc}"
        ) from exc
    if result.returncode != 0:
        raise DoxygenProvisioningError(
            "the official Doxygen 1.9.8 Darwin artifact is x86_64 and "
            "requires Rosetta 2 on Apple Silicon; install it with "
            "`softwareupdate --install-rosetta --agree-to-license` and retry"
        )


@contextlib.contextmanager
def _mounted_read_only_dmg(
    archive_path: Path,
    cache_root: Path,
) -> Iterator[Path]:
    """Mount one authenticated DMG read-only and always detach it."""
    hdiutil = Path("/usr/bin/hdiutil")
    if not hdiutil.is_file() or not os.access(hdiutil, os.X_OK):
        raise DoxygenProvisioningError(
            "Darwin Doxygen provisioning requires executable /usr/bin/hdiutil"
        )
    with tempfile.TemporaryDirectory(
        prefix=".doxygen-mount-", dir=cache_root
    ) as temporary:
        mount_point = Path(temporary)
        attached = False
        try:
            result = subprocess.run(
                [
                    str(hdiutil),
                    "attach",
                    "-readonly",
                    "-nobrowse",
                    "-noautoopen",
                    "-mountpoint",
                    str(mount_point),
                    str(archive_path),
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=60.0,
            )
            if result.returncode != 0:
                diagnostic = result.stderr.decode(
                    "utf-8", errors="replace"
                ).strip()
                raise DoxygenProvisioningError(
                    "cannot mount locked Doxygen DMG read-only: "
                    f"{diagnostic or f'exit code {result.returncode}'}"
                )
            attached = True
            yield mount_point
        finally:
            if attached:
                try:
                    detached = subprocess.run(
                        [str(hdiutil), "detach", str(mount_point)],
                        stdin=subprocess.DEVNULL,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        check=False,
                        timeout=60.0,
                    )
                except (OSError, subprocess.SubprocessError) as exc:
                    raise DoxygenProvisioningError(
                        f"cannot detach Doxygen DMG {mount_point}: {exc}"
                    ) from exc
                if detached.returncode != 0:
                    diagnostic = detached.stderr.decode(
                        "utf-8", errors="replace"
                    ).strip()
                    raise DoxygenProvisioningError(
                        "cannot detach Doxygen DMG: "
                        f"{diagnostic or f'exit code {detached.returncode}'}"
                    )


def _extract_dmg_executable(
    archive_path: Path,
    destination: Path,
    asset: DoxygenAsset,
    cache_root: Path,
) -> None:
    """Copy the exact regular executable from an authenticated read-only DMG."""
    with _mounted_read_only_dmg(archive_path, cache_root) as mount_point:
        source = mount_point.joinpath(*asset.executable_member.split("/"))
        try:
            metadata = source.lstat()
            resolved_mount = mount_point.resolve(strict=True)
            resolved_source = source.resolve(strict=True)
        except OSError as exc:
            raise DoxygenProvisioningError(
                f"cannot inspect Doxygen DMG executable member: {exc}"
            ) from exc
        if (
            source.is_symlink()
            or not stat.S_ISREG(metadata.st_mode)
            or not resolved_source.is_relative_to(resolved_mount)
            or metadata.st_size != asset.executable_size_bytes
        ):
            raise DoxygenProvisioningError(
                "Doxygen DMG executable member is not the locked contained "
                "regular file"
            )
        with source.open("rb") as source_handle:
            durable_replace_stream(
                destination,
                lambda descriptor: _copy_verified_executable(
                    source_handle, descriptor, asset
                ),
                mode=0o755,
            )


def _query_exact_version(executable: Path, asset: DoxygenAsset) -> str:
    """Execute the cached binary and require the lock's exact version output."""
    try:
        result = subprocess.run(
            [str(executable), "--version"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=_VERSION_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise DoxygenProvisioningError(
            f"cannot execute provisioned Doxygen {executable}: {exc}"
        ) from exc
    try:
        output = result.stdout.decode("utf-8", errors="strict")
        diagnostic = result.stderr.decode("utf-8", errors="strict")
    except UnicodeError as exc:
        raise DoxygenProvisioningError(
            "Doxygen version output is not valid UTF-8"
        ) from exc
    if output.endswith("\n"):
        output = output[:-1]
        if output.endswith("\r"):
            output = output[:-1]
    if (
        result.returncode != 0
        or diagnostic != ""
        or output != asset.version_output
    ):
        raise DoxygenProvisioningError(
            "provisioned Doxygen version identity differs from the lock: "
            f"expected={asset.version_output!r} observed={output!r} "
            f"stderr={diagnostic!r} exit_code={result.returncode}"
        )
    return output


def _cached_executable_is_valid(
    executable: Path,
    asset: DoxygenAsset,
) -> bool:
    """Return whether the cached binary retains its bytes and runtime identity."""
    if not _regular_file_matches(
        executable,
        size_bytes=asset.executable_size_bytes,
        sha256=asset.executable_sha256,
        executable=True,
    ):
        return False
    try:
        _query_exact_version(executable, asset)
    except DoxygenProvisioningError:
        return False
    return True


def ensure_exact_doxygen(
    cache_root: Path = DEFAULT_DOXYGEN_CACHE_ROOT,
    *,
    platform_name: str | None = None,
    machine_name: str | None = None,
    lock_path: Path = DOXYGEN_TOOLCHAIN_LOCK,
) -> ProvisionedDoxygen:
    """Provision and validate exact Doxygen 1.9.8 before CMake configure.

    @param cache_root Builder-owned cache root used across website runs.
    @param platform_name Optional host platform override for contract tests.
    @param machine_name Optional machine architecture override for contract tests.
    @param lock_path Tracked immutable artifact lock.
    @return The canonical executable and complete configure identity.
    @throws DoxygenProvisioningError On unsupported hosts or any integrity failure.
    """
    selected_platform = platform_name or sys.platform
    selected_machine = machine_name or platform.machine()
    normalized_platform, normalized_machine, asset = _select_asset(
        platform_name=selected_platform,
        machine_name=selected_machine,
        lock_path=lock_path,
    )
    _require_rosetta_if_needed(
        normalized_platform, normalized_machine, asset
    )
    resolved_cache_root = _ensure_private_cache_directory(cache_root)

    with _provisioning_lock(resolved_cache_root):
        archive_directory = _ensure_private_cache_subdirectory(
            resolved_cache_root,
            "archives",
        )
        executable_directory = _ensure_private_cache_subdirectory(
            resolved_cache_root,
            EXACT_DOXYGEN_VERSION,
            asset.host_key,
            "bin",
        )
        archive_suffix = ".dmg" if asset.archive_type == "dmg" else ".tar.gz"
        archive_path = (
            archive_directory / f"{asset.archive_sha256}{archive_suffix}"
        )
        executable = executable_directory / "doxygen"

        if _cached_executable_is_valid(executable, asset):
            return ProvisionedDoxygen(
                executable=executable.resolve(strict=True),
                version=EXACT_DOXYGEN_VERSION,
                version_output=asset.version_output,
                platform_name=normalized_platform,
                architecture=normalized_machine,
                archive_sha256=asset.archive_sha256,
                executable_sha256=asset.executable_sha256,
                cache_hit=True,
            )

        if executable.exists() or executable.is_symlink():
            try:
                metadata = executable.lstat()
                if stat.S_ISDIR(metadata.st_mode) and not executable.is_symlink():
                    raise DoxygenProvisioningError(
                        "invalid Doxygen executable cache entry is a directory: "
                        f"{executable}"
                    )
                executable.unlink()
                fsync_directory(executable.parent)
            except DoxygenProvisioningError:
                raise
            except OSError as exc:
                raise DoxygenProvisioningError(
                    "cannot remove invalid Doxygen executable cache entry "
                    f"{executable}: {exc}"
                ) from exc

        _download_archive(asset, archive_path)
        if asset.archive_type == "tar_gz":
            _extract_tar_executable(archive_path, executable, asset)
        elif asset.archive_type == "dmg":
            _extract_dmg_executable(
                archive_path,
                executable,
                asset,
                resolved_cache_root,
            )
        else:
            raise DoxygenProvisioningError(
                f"unsupported locked archive type {asset.archive_type!r}"
            )

        if not _regular_file_matches(
            executable,
            size_bytes=asset.executable_size_bytes,
            sha256=asset.executable_sha256,
            executable=True,
        ):
            with contextlib.suppress(OSError):
                executable.unlink()
                fsync_directory(executable.parent)
            raise DoxygenProvisioningError(
                "published Doxygen executable does not retain its locked identity"
            )
        try:
            version_output = _query_exact_version(executable, asset)
        except DoxygenProvisioningError:
            with contextlib.suppress(OSError):
                executable.unlink()
                fsync_directory(executable.parent)
            raise
        return ProvisionedDoxygen(
            executable=executable.resolve(strict=True),
            version=EXACT_DOXYGEN_VERSION,
            version_output=version_output,
            platform_name=normalized_platform,
            architecture=normalized_machine,
            archive_sha256=asset.archive_sha256,
            executable_sha256=asset.executable_sha256,
            cache_hit=False,
        )


def build_argument_parser() -> argparse.ArgumentParser:
    """Build the standalone bootstrap prewarm command-line parser."""
    parser = argparse.ArgumentParser(
        description="Provision exact Doxygen 1.9.8 into the bioseq builder cache."
    )
    parser.add_argument(
        "--cache-root",
        type=Path,
        default=DEFAULT_DOXYGEN_CACHE_ROOT,
        help="builder-owned Doxygen cache root",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Prewarm exact Doxygen and print only its canonical executable path."""
    arguments = build_argument_parser().parse_args(argv)
    try:
        provisioned = ensure_exact_doxygen(arguments.cache_root)
    except DoxygenProvisioningError as exc:
        print(f"doxygen provisioning failed: {exc}", file=sys.stderr)
        return 1
    print(provisioned.executable)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
