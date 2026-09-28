#!/usr/bin/env python3
"""Local-only Debian 13/PG17/ARM64 package qualification worker.

The public entry point is :func:`run_package_linux`.  It is intentionally
stdlib-only so ``build_mcp/server.py`` can import it.  The two private CLI
entry points run from the locked, builder-owned Python environment or inside
the exact Linux image and reuse the retained packaging implementation.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
import platform
import re
import shutil
import socket
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
from dataclasses import replace
from pathlib import Path, PurePosixPath
from typing import Callable, Mapping, Sequence, TextIO
from urllib.parse import urlsplit


MODE = "package-linux"
REPORTING_SUBDIR = "bioseq_package_linux"
IMAGE_TAG = "debian:trixie"
POSTGRES_MAJOR = 17
ARCHITECTURE = "arm64"
PACKAGE_PROFILE_REVISION = 3
MINIMUM_FREE_BYTES = 80 * 1024**3
LIQUID_VERSION = "2.3.0"
EXPECTED_BASENAMES = frozenset({
    "bioseq.so",
    "bioseq_cache.so",
    "bioseq--1.0.sql",
    "bioseq_cache--1.0.sql",
    "bioseq.control",
    "bioseq_cache.control",
})
PACKAGE_CACHE_VALUES = {
    "CMAKE_BUILD_TYPE": "Release",
    "BIOSEQ_PACKAGE_BUILD": "ON",
    "BIOSEQ_TEMPLATE_FLATTEN_PROFILE": "all_alphabets_uint64",
    "BIOSEQ_BUILD_CORE": "ON",
    "BIOSEQ_BUILD_CACHE": "ON",
    "BIOSEQ_BUILD_PAPERS": "OFF",
    "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
    "BIOSEQ_BUILD_WEBSITE": "OFF",
    "BIOSEQ_TEST_INDEXES": "OFF",
    "BIOSEQ_CACHE_BUILD_TESTS": "OFF",
    "BIOSEQ_CACHE_TESTING": "OFF",
}
_INPUT_FILES = (
    "infra/packaging/pyproject.toml",
    "infra/packaging/poetry.lock",
    "infra/packaging/bioseq_packaging/config.py",
    "infra/packaging/bioseq_packaging/containers.py",
    "infra/packaging/bioseq_packaging/model.py",
    "infra/packaging/bioseq_packaging/templates.py",
    "infra/packaging/bioseq_packaging/workspace.py",
    "infra/packaging/bioseq_packaging/packagers/deb.py",
    "infra/packaging/scripts/build_and_stage.sh",
    "infra/packaging/scripts/prepare_build_environment.sh",
    "infra/packaging/scripts/validate_package.py",
    "build_mcp/package_linux.py",
)
_SOURCE_SNAPSHOT_FILES = (
    "CMakeLists.txt",
    "CMakePresets.json",
    "vcpkg.json",
    "vcpkg-configuration.json",
)
_SOURCE_SNAPSHOT_DIRECTORIES = (
    "bench",
    "bench_framework",
    "bioseq",
    "bioseq_caching",
    "build_mcp",
    "cmake",
    "codegen",
    "common",
    "data",
    "infra",
    "overlays",
    "vendor",
    "website",
)
# Public, immutable authority for the committed package source closure.  It is
# deliberately explicit: unrelated repository-root material and the sealed
# papers tree is unreachable rather than included and then filtered.
SOURCE_SNAPSHOT_PATHS: tuple[str, ...] = (
    *_SOURCE_SNAPSHOT_FILES,
    *_SOURCE_SNAPSHOT_DIRECTORIES,
)
_SOURCE_SNAPSHOT_EXCLUDE_PATHSPECS = (
    ":(top,exclude,glob)**/.DS_Store",
    ":(top,exclude,glob)**/__pycache__",
    ":(top,exclude,glob)**/__pycache__/**",
    ":(top,exclude,glob)**/*.pyc",
    ":(top,exclude,glob)**/build",
    ":(top,exclude,glob)**/build/**",
    ":(top,exclude,glob)**/reporting",
    ":(top,exclude,glob)**/reporting/**",
)
_DIGEST = re.compile(r"[0-9a-f]{64}\Z")
_REVISION = re.compile(r"[0-9a-f]{40}\Z")
_GENERATED_BYTECODE = re.compile(r"(?:^|/)__pycache__(?:/|$)|\.pyc\Z")
_IGNORED_UNTRACKED_OUTPUT = re.compile(r"(?:^|/)(?:build|reporting)(?:/|$)")


class PackageLinuxError(RuntimeError):
    """A fail-closed local package qualification error."""


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _canonical_sha256(value: object) -> str:
    return _sha256_bytes(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    )


def _sealed(value: Mapping[str, object]) -> dict[str, object]:
    if "content_sha256" in value:
        raise PackageLinuxError("document is already sealed")
    result = dict(value)
    result["content_sha256"] = _canonical_sha256(result)
    return result


def _validate_seal(value: object, label: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise PackageLinuxError(f"{label} is not a JSON object")
    digest = value.get("content_sha256")
    body = {key: item for key, item in value.items() if key != "content_sha256"}
    if not isinstance(digest, str) or not _DIGEST.fullmatch(digest):
        raise PackageLinuxError(f"{label} has no valid content seal")
    if _canonical_sha256(body) != digest:
        raise PackageLinuxError(f"{label} content seal changed")
    return value


def _require_exact_keys(
    value: Mapping[str, object], expected: set[str], label: str,
) -> None:
    actual = set(value)
    if actual != expected:
        raise PackageLinuxError(
            f"{label} has unexpected keys: "
            f"missing={sorted(expected - actual)} extra={sorted(actual - expected)}"
        )


def _require_document_contract(
    value: dict[str, object],
    *,
    label: str,
    revision: int,
    artifact_kind: str,
    keys: set[str],
) -> dict[str, object]:
    _require_exact_keys(
        value,
        {"schema_revision", "artifact_kind", *keys},
        label,
    )
    if (
        type(value["schema_revision"]) is not int
        or value["schema_revision"] != revision
        or value["artifact_kind"] != artifact_kind
    ):
        raise PackageLinuxError(f"{label} schema is invalid")
    return value


def _write_json_atomic(path: Path, value: Mapping[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and (path.is_symlink() or not path.is_file()):
        raise PackageLinuxError(f"JSON destination is not regular: {path}")
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", prefix=f".{path.name}.", suffix=".tmp",
            dir=path.parent, delete=False,
        ) as handle:
            temporary = Path(handle.name)
            json.dump(value, handle, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
        temporary = None
        flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
        directory_fd = os.open(path.parent, flags)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def _load_json(path: Path, label: str, *, sealed: bool = False) -> dict[str, object]:
    try:
        metadata = path.lstat()
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise PackageLinuxError(f"cannot read {label} {path}: {error}") from error
    if path.is_symlink() or not stat.S_ISREG(metadata.st_mode):
        raise PackageLinuxError(f"{label} is not a regular non-symlink file")
    if not isinstance(value, dict):
        raise PackageLinuxError(f"{label} is not a JSON object")
    return _validate_seal(value, label) if sealed else value


def _require_regular(path: Path, label: str) -> Path:
    try:
        metadata = path.lstat()
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise PackageLinuxError(f"cannot inspect {label} {path}: {error}") from error
    if path.is_symlink() or not stat.S_ISREG(metadata.st_mode):
        raise PackageLinuxError(f"{label} is not a regular non-symlink file: {path}")
    return resolved


def _descendant(path: Path, root: Path, label: str) -> Path:
    resolved = path.resolve(strict=False)
    parent = root.resolve(strict=False)
    if resolved == parent or not resolved.is_relative_to(parent):
        raise PackageLinuxError(f"{label} escapes builder-owned root {parent}: {resolved}")
    return resolved


def _safe_rmtree(path: Path, root: Path) -> None:
    resolved = _descendant(path, root, "cleanup path")
    if path.is_symlink() or resolved.is_symlink():
        raise PackageLinuxError(f"refusing to remove symlink {path}")
    if resolved.exists():
        shutil.rmtree(resolved)


def _log(handle: TextIO, message: str) -> None:
    handle.write(message.rstrip() + "\n")
    handle.flush()


def _run(
    argv: Sequence[str], *, log: TextIO, deadline: float, cwd: Path | None = None,
    environment: Mapping[str, str] | None = None, capture: bool = False,
) -> str:
    if not argv or any(not isinstance(value, str) or not value for value in argv):
        raise PackageLinuxError("subprocess argv must contain non-empty strings")
    if any(value == "--platform" or value.startswith("--platform=") for value in argv):
        raise PackageLinuxError("cross-architecture Docker selection is forbidden")
    remaining = deadline - time.time()
    if remaining <= 0:
        raise TimeoutError(f"package-linux deadline reached before {argv[0]}")
    _log(log, "+ " + " ".join(argv))
    process = subprocess.Popen(
        list(argv), cwd=str(cwd) if cwd else None,
        env=dict(environment) if environment is not None else None,
        stdout=subprocess.PIPE if capture else log,
        stderr=subprocess.STDOUT, text=True, start_new_session=True,
    )
    try:
        if capture:
            output, _ = process.communicate(timeout=remaining)
        else:
            process.wait(timeout=remaining)
            output = ""
    except subprocess.TimeoutExpired as error:
        try:
            os.killpg(process.pid, 15)
            process.wait(timeout=2)
        except (OSError, subprocess.TimeoutExpired):
            try:
                os.killpg(process.pid, 9)
            except OSError:
                pass
        raise TimeoutError(f"package-linux deadline reached during {argv[0]}") from error
    if process.returncode != 0:
        detail = output.strip()[-2000:] if capture else "see package-linux log"
        raise PackageLinuxError(
            f"command exited {process.returncode}: {argv[0]}: {detail}")
    return output


def _git(
    repository: Path, arguments: Sequence[str], *, log: TextIO, deadline: float,
) -> str:
    return _run(
        ["git", "-C", str(repository), *arguments], log=log,
        deadline=deadline, capture=True,
    ).strip()


def _git_porcelain_status(
    repository: Path, *, log: TextIO, deadline: float,
) -> str:
    """Capture porcelain without stripping its significant leading columns."""
    return _run(
        ["git", "-C", str(repository), "status", "--porcelain=v1",
         "--untracked-files=all", "--", *SOURCE_SNAPSHOT_PATHS,
         *_SOURCE_SNAPSHOT_EXCLUDE_PATHSPECS],
        log=log, deadline=deadline, capture=True,
    ).rstrip("\r\n")


def _native_arm64() -> str:
    machine = platform.machine().strip().lower()
    if machine not in {"arm64", "aarch64"}:
        raise PackageLinuxError(
            f"{MODE} requires a native ARM64 host; observed {machine or '<empty>'}")
    return ARCHITECTURE


def _require_free_space(path: Path) -> int:
    probe = path
    while not probe.exists() and probe.parent != probe:
        probe = probe.parent
    available = shutil.disk_usage(probe).free
    if available < MINIMUM_FREE_BYTES:
        raise PackageLinuxError(
            f"{MODE} has {available / 1024**3:.1f} GiB free; "
            f"at least {MINIMUM_FREE_BYTES / 1024**3:.0f} GiB is required")
    return available


def _docker_socket(environment: Mapping[str, str]) -> tuple[str, Path]:
    endpoint = environment.get(
        "BIOSEQ_PACKAGE_DOCKER_HOST",
        environment.get("DOCKER_HOST", "unix:///var/run/docker.sock"),
    )
    parsed = urlsplit(endpoint)
    if (
        parsed.scheme != "unix" or parsed.netloc or not parsed.path
        or parsed.query or parsed.fragment
    ):
        raise PackageLinuxError("package-linux requires an explicit Unix Docker socket")
    return endpoint, Path(parsed.path)


def _require_healthy_docker(environment: Mapping[str, str]) -> tuple[str, Path]:
    endpoint, socket_path = _docker_socket(environment)
    try:
        metadata = socket_path.stat()
    except OSError as error:
        raise PackageLinuxError(
            f"Docker daemon socket is unavailable; start it before {MODE}: {error}") from error
    if not stat.S_ISSOCK(metadata.st_mode):
        raise PackageLinuxError(f"Docker endpoint is not a Unix socket: {socket_path}")
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(3.0)
    try:
        connection.connect(str(socket_path))
        connection.sendall(
            b"GET /_ping HTTP/1.1\r\nHost: docker\r\nConnection: close\r\n\r\n")
        response = b""
        while len(response) < 65536:
            chunk = connection.recv(4096)
            if not chunk:
                break
            response += chunk
    except OSError as error:
        raise PackageLinuxError(
            f"Docker daemon is not healthy; start it before {MODE}: {error}") from error
    finally:
        connection.close()
    header, separator, body = response.partition(b"\r\n\r\n")
    if not separator or not header.startswith(b"HTTP/1.1 200 ") or body.strip() != b"OK":
        raise PackageLinuxError("Docker daemon /_ping did not return HTTP 200 OK")
    return endpoint, socket_path.resolve(strict=True)


def _docker_environment(base: Mapping[str, str], endpoint: str) -> dict[str, str]:
    result = {str(key): str(value) for key, value in base.items()}
    result["DOCKER_HOST"] = endpoint
    result.pop("DOCKER_CONTEXT", None)
    return result


def _docker_cli_identity(entry_path: Path) -> dict[str, object]:
    """Seal the absolute ``docker`` entry separately from its multicall target."""
    entry = Path(os.path.abspath(entry_path))
    if not entry.is_absolute() or entry.name != "docker":
        raise PackageLinuxError(
            f"Docker CLI entry must be an absolute path named docker: {entry}")
    try:
        entry_metadata = entry.lstat()
    except OSError as error:
        raise PackageLinuxError(f"cannot inspect Docker CLI entry {entry}: {error}") from error
    if not (stat.S_ISREG(entry_metadata.st_mode) or stat.S_ISLNK(entry_metadata.st_mode)):
        raise PackageLinuxError(f"Docker CLI entry is not a file or symlink: {entry}")

    links: list[dict[str, object]] = []
    current = entry
    seen: set[Path] = set()
    while True:
        if current in seen:
            raise PackageLinuxError(f"Docker CLI symlink cycle at {current}")
        seen.add(current)
        try:
            metadata = current.lstat()
        except OSError as error:
            raise PackageLinuxError(
                f"cannot inspect Docker CLI chain entry {current}: {error}") from error
        if not stat.S_ISLNK(metadata.st_mode):
            break
        target_text = os.readlink(current)
        links.append({
            "path": str(current),
            "mode": stat.S_IMODE(metadata.st_mode),
            "size_bytes": metadata.st_size,
            "link_target": target_text,
            "link_target_sha256": _sha256_bytes(os.fsencode(target_text)),
        })
        target = Path(target_text)
        current = Path(os.path.abspath(
            target if target.is_absolute() else current.parent / target))

    resolved = current.resolve(strict=True)
    target_metadata = resolved.stat()
    if not stat.S_ISREG(target_metadata.st_mode) or not os.access(entry, os.X_OK):
        raise PackageLinuxError(
            f"Docker CLI entry does not resolve to an executable regular file: {entry}")
    entry_identity: dict[str, object] = {
        "schema_revision": 1,
        "artifact_kind": "bioseq-docker-cli-identity",
        "entry_path": str(entry),
        "entry_basename": entry.name,
        "entry_kind": "symlink" if links else "regular",
        "entry_mode": stat.S_IMODE(entry_metadata.st_mode),
        "entry_size_bytes": entry_metadata.st_size,
        "symlink_chain": links,
    }
    return _sealed({
        **entry_identity,
        "entry_identity_sha256": _canonical_sha256(entry_identity),
        "target_lexical_path": str(current),
        "target_resolved_path": str(resolved),
        "target_mode": stat.S_IMODE(target_metadata.st_mode),
        "target_size_bytes": target_metadata.st_size,
        "target_sha256": _sha256_file(resolved),
    })


def _validated_docker_entry(identity: object) -> Path:
    expected = _validate_seal(identity, "Docker CLI identity")
    _require_document_contract(
        expected,
        label="Docker CLI identity",
        revision=1,
        artifact_kind="bioseq-docker-cli-identity",
        keys={
            "entry_path", "entry_basename", "entry_kind", "entry_mode",
            "entry_size_bytes", "symlink_chain", "entry_identity_sha256",
            "target_lexical_path", "target_resolved_path", "target_mode",
            "target_size_bytes", "target_sha256", "content_sha256",
        },
    )
    entry_raw = expected.get("entry_path")
    if not isinstance(entry_raw, str) or not Path(entry_raw).is_absolute():
        raise PackageLinuxError("Docker CLI identity has an invalid entry path")
    observed = _docker_cli_identity(Path(entry_raw))
    if observed != expected:
        raise PackageLinuxError("Docker CLI entry or resolved target identity changed")
    return Path(entry_raw)


def _docker_image(
    docker_identity: Mapping[str, object], environment: Mapping[str, str], *,
    log: TextIO, deadline: float,
) -> dict[str, object]:
    docker = _validated_docker_entry(docker_identity)
    _run([str(docker), "pull", IMAGE_TAG], log=log, deadline=deadline,
         environment=environment)
    docker = _validated_docker_entry(docker_identity)
    raw = _run(
        [str(docker), "image", "inspect", IMAGE_TAG], log=log,
        deadline=deadline, environment=environment, capture=True,
    )
    try:
        rows = json.loads(raw)
    except json.JSONDecodeError as error:
        raise PackageLinuxError("Docker image inspection returned invalid JSON") from error
    if not isinstance(rows, list) or len(rows) != 1 or not isinstance(rows[0], dict):
        raise PackageLinuxError("Docker image inspection did not return one image")
    row = rows[0]
    image_id = row.get("Id")
    if (
        not isinstance(image_id, str)
        or re.fullmatch(r"sha256:[0-9a-f]{64}", image_id) is None
        or row.get("Architecture") not in {"arm64", "aarch64"}
        or row.get("Os") != "linux"
    ):
        raise PackageLinuxError("Debian image is not a native Linux ARM64 image")
    digests = row.get("RepoDigests") or []
    if not isinstance(digests, list) or any(not isinstance(item, str) for item in digests):
        raise PackageLinuxError("Docker image repository digests are malformed")
    return {
        "tag": IMAGE_TAG,
        "id": image_id,
        "repository_digests": sorted(digests),
        "os": "linux",
        "architecture": ARCHITECTURE,
    }


def _safe_extract(archive: Path, destination: Path) -> Path:
    if destination.exists():
        if destination.is_symlink() or not destination.is_dir():
            raise PackageLinuxError("source snapshot extraction root is unsafe")
        if any(destination.iterdir()):
            raise PackageLinuxError("source snapshot extraction root is not empty")
    else:
        destination.mkdir(parents=True, exist_ok=False)
    _require_regular(archive, "source snapshot archive")
    allowed_files = frozenset(PurePosixPath(value) for value in _SOURCE_SNAPSHOT_FILES)
    allowed_directories = tuple(
        PurePosixPath(value) for value in _SOURCE_SNAPSHOT_DIRECTORIES)
    observed_members: set[str] = set()
    with tarfile.open(archive, "r:") as bundle:
        members = bundle.getmembers()
        if not members:
            raise PackageLinuxError("source snapshot archive is empty")
        for member in members:
            relative = PurePosixPath(member.name)
            if (
                relative.parts[:2] == ("main", "papers")
                or relative.parts[:1] == ("papers",)
            ):
                raise PackageLinuxError(
                    "source snapshot archive crossed the sealed source boundary")
            if (
                relative.is_absolute() or not relative.parts
                or ".." in relative.parts
                or member.issym() or member.islnk() or member.isdev() or member.isfifo()
                or not (member.isfile() or member.isdir())
            ):
                raise PackageLinuxError(
                    f"source snapshot archive has unsafe member {member.name!r}")
            normalized = relative.as_posix()
            if normalized in observed_members:
                raise PackageLinuxError(
                    f"source snapshot archive has duplicate member {member.name!r}")
            observed_members.add(normalized)
            generated = (
                relative.name == ".DS_Store"
                or "__pycache__" in relative.parts
                or relative.suffix == ".pyc"
                or any(part in {"build", "reporting"} for part in relative.parts)
            )
            exact_file = relative in allowed_files and member.isfile()
            inside_directory = any(
                relative == root or relative.is_relative_to(root)
                for root in allowed_directories
            )
            structural_parent = member.isdir() and any(
                root.is_relative_to(relative) for root in allowed_directories
            )
            if generated or not (exact_file or inside_directory or structural_parent):
                raise PackageLinuxError(
                    f"source snapshot archive has out-of-closure member {member.name!r}")
        bundle.extractall(destination)
    for relative in _SOURCE_SNAPSHOT_FILES:
        _require_regular(
            destination / relative, f"source snapshot root file {relative}")
    for relative in _SOURCE_SNAPSHOT_DIRECTORIES:
        directory = destination / relative
        if directory.is_symlink() or not directory.is_dir():
            raise PackageLinuxError(
                f"source snapshot does not contain directory {relative}")
    return destination


def _source_snapshot(
    repository: Path, state_root: Path, *, log: TextIO, deadline: float,
) -> dict[str, object]:
    dirty = _git_porcelain_status(repository, log=log, deadline=deadline)
    dirty_rows = [row for row in dirty.splitlines() if row]
    material_dirty = _material_dirty_rows(dirty)
    ignored_generated = [row for row in dirty_rows if row not in material_dirty]
    if material_dirty:
        raise PackageLinuxError(
            "material source snapshot is dirty; commit it before package qualification: "
            + "; ".join(material_dirty[:10]))
    revision = _git(repository, ["rev-parse", "HEAD"], log=log, deadline=deadline)
    source_tree_ref = f"{revision}^{{tree}}"
    tree = _git(
        repository, ["rev-parse", source_tree_ref], log=log, deadline=deadline)
    if not _REVISION.fullmatch(revision) or not _REVISION.fullmatch(tree):
        raise PackageLinuxError("Git revision or source tree is not a full SHA-1")
    ephemeral = Path(tempfile.mkdtemp(prefix="source-", dir=state_root))
    try:
        archive = ephemeral / "source.tar"
        # The positive pathspecs are the complete lifted source closure.  The
        # exclusions keep generated/cache outputs out even when they are
        # accidentally tracked below an otherwise owned source directory.
        _run(
            ["git", "-C", str(repository), "archive", "--format=tar",
             f"--output={archive}", revision, "--", *SOURCE_SNAPSHOT_PATHS,
             *_SOURCE_SNAPSHOT_EXCLUDE_PATHSPECS],
            log=log, deadline=deadline,
        )
        # Keep the archive outside the extracted repository root so no retained
        # build process can consume the archive as a source-tree input.
        source = _safe_extract(archive, ephemeral / "repository")
        (source.parent / ".bioseq-source-revision").write_text(
            revision + "\n", encoding="ascii")
        input_hashes: dict[str, str] = {}
        for relative in _INPUT_FILES:
            path = _require_regular(source / relative, f"snapshot input {relative}")
            input_hashes[relative] = _sha256_file(path)
        return {
            "ephemeral_root": ephemeral,
            "source": source,
            "archive": archive,
            "revision": revision,
            "source_tree": tree,
            "archive_sha256": _sha256_file(archive),
            "input_sha256": input_hashes,
            "ignored_generated_status_rows": ignored_generated,
        }
    except Exception:
        _safe_rmtree(ephemeral, state_root)
        raise


def _validate_snapshot_extension_dso_contract(
    source: Path,
) -> None:
    """Fail before Docker when the source-snapshot Linux DSO producer drifts."""
    helper = _require_regular(
        source / "cmake/add_postgresql_extension.cmake",
        "source snapshot extension DSO producer contract",
    )
    try:
        contents = helper.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise PackageLinuxError(
            f"cannot read source snapshot extension DSO producer contract: {error}"
        ) from error
    marker = 'elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")'
    source_lines = [line.partition("#")[0] for line in contents.splitlines()]
    marker_positions = [
        index for index, line in enumerate(source_lines)
        if line.strip() == marker
    ]
    remainder = (
        "\n".join(source_lines[marker_positions[0] + 1:])
        if len(marker_positions) == 1
        else ""
    )
    nested_depth = 0
    linux_branch_lines: list[str] = []
    terminator = False
    for line in remainder.splitlines():
        code = line.partition("#")[0]
        command = code.strip().lower()
        if command.startswith("if("):
            nested_depth += 1
        elif command.startswith("endif("):
            if nested_depth == 0:
                terminator = True
                break
            nested_depth -= 1
        linux_branch_lines.append(code)
    linux_branch = "\n".join(linux_branch_lines)
    build_id_option = '"LINKER:--build-id=sha1"'
    build_id_reference = '"${_bioseq_dso_elf_build_id_link_option}"'
    if (
        len(marker_positions) != 1
        or not terminator
        or linux_branch.count(build_id_option) != 1
        or linux_branch.count(build_id_reference) != 1
    ):
        raise PackageLinuxError(
            "source snapshot Linux extension DSO producer does not attach exactly "
            "one SHA-1 GNU build-ID option"
        )


def _material_dirty_rows(status: str) -> list[str]:
    material: list[str] = []
    for row in (value for value in status.splitlines() if value):
        # An interpreter cache is generated noise and is never read by
        # the build: git archive supplies the committed byte-for-byte version.
        # Keep every other tracked change fail-closed.
        path = row[3:] if len(row) >= 4 and row[2] == " " else ""
        if " -> " not in path and _GENERATED_BYTECODE.search(path):
            continue
        if row.startswith("?? ") and _IGNORED_UNTRACKED_OUTPUT.search(path):
            continue
        material.append(row)
    return material


def _python_identity(
    executable: str, *, log: TextIO, deadline: float,
) -> dict[str, object]:
    found = shutil.which(executable)
    if found is None:
        raise PackageLinuxError(f"required package bootstrap Python is missing: {executable}")
    resolved = Path(found).resolve(strict=True)
    raw = _run(
        [str(resolved), "-c",
         "import json,platform,sys;print(json.dumps({'version':list(sys.version_info[:3]),'machine':platform.machine()}))"],
        log=log, deadline=deadline, capture=True,
    )
    try:
        details = json.loads(raw)
    except json.JSONDecodeError as error:
        raise PackageLinuxError("bootstrap Python identity is malformed") from error
    version = details.get("version") if isinstance(details, dict) else None
    if not isinstance(version, list) or len(version) != 3 or not (3, 12) <= tuple(version) < (3, 14):
        raise PackageLinuxError("package bootstrap Python must be >=3.12 and <3.14")
    if str(details.get("machine", "")).lower() not in {"arm64", "aarch64"}:
        raise PackageLinuxError("package bootstrap Python is not native ARM64")
    return {
        "path": str(resolved), "sha256": _sha256_file(resolved),
        "version": ".".join(str(item) for item in version),
    }


def _package_python(
    state_root: Path, source: Path, identity: Mapping[str, object], *,
    log: TextIO, deadline: float,
) -> tuple[Path, dict[str, object]]:
    pyproject = source / "infra/packaging/pyproject.toml"
    lock = source / "infra/packaging/poetry.lock"
    project_text = pyproject.read_text(encoding="utf-8")
    lock_text = lock.read_text(encoding="utf-8")
    if (
        f'python-liquid = "{LIQUID_VERSION}"' not in project_text
        or not re.search(
            rf'\[\[package\]\]\s+name = "python-liquid"\s+version = "{re.escape(LIQUID_VERSION)}"',
            lock_text,
        )
    ):
        raise PackageLinuxError("committed Poetry inputs do not pin python-liquid 2.3.0")
    contract = {
        "schema_revision": 1,
        "python": dict(identity),
        "pyproject_sha256": _sha256_file(pyproject),
        "poetry_lock_sha256": _sha256_file(lock),
        "python_liquid": LIQUID_VERSION,
    }
    key = _canonical_sha256(contract)
    environment_root = state_root / "environments"
    environment_root.mkdir(parents=True, exist_ok=True)
    target = environment_root / key
    marker = target / ".bioseq-package-environment.json"
    python = target / "bin/python"
    valid = False
    if marker.is_file() and python.is_file() and not marker.is_symlink():
        try:
            observed = _load_json(marker, "package environment marker", sealed=True)
            _require_document_contract(
                observed,
                label="package environment marker",
                revision=1,
                artifact_kind="bioseq-package-python-environment",
                keys={"contract", "content_sha256"},
            )
            version = _run(
                [str(python), "-c",
                 "import importlib.metadata;print(importlib.metadata.version('python-liquid'))"],
                log=log, deadline=deadline, capture=True,
            ).strip()
            valid = observed.get("contract") == contract and version == LIQUID_VERSION
        except (OSError, PackageLinuxError):
            valid = False
    if not valid:
        if target.exists():
            _safe_rmtree(target, environment_root)
        try:
            _run([str(identity["path"]), "-m", "venv", str(target)],
                 log=log, deadline=deadline)
            _run(
                [str(python), "-m", "pip", "install", "--disable-pip-version-check",
                 f"python-liquid=={LIQUID_VERSION}"],
                log=log, deadline=deadline,
            )
            _write_json_atomic(marker, _sealed({
                "schema_revision": 1,
                "artifact_kind": "bioseq-package-python-environment",
                "contract": contract,
            }))
        except Exception:
            if target.exists():
                _safe_rmtree(target, environment_root)
            raise
    return python, {**contract, "cache_key": key, "path": str(target)}


def _parse_cmake_cache(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line or ":" not in line:
            continue
        lhs, value = line.split("=", 1)
        name, _kind = lhs.split(":", 1)
        if name in values:
            raise PackageLinuxError(f"duplicate CMake cache key {name}")
        values[name] = value
    return values


def _package_file_list(path: Path) -> dict[str, object]:
    rows = [row.strip() for row in path.read_text(encoding="utf-8").splitlines() if row.strip()]
    basenames = [PurePosixPath(row).name for row in rows]
    if len(rows) != 6 or len(set(rows)) != 6 or set(basenames) != EXPECTED_BASENAMES:
        raise PackageLinuxError("retained staging manifest is not the exact six-file payload")
    return {"paths": sorted(rows), "sha256": _sha256_file(path)}


def _configuration_evidence(result: Mapping[str, object]) -> dict[str, object]:
    cache = _require_regular(Path(str(result["cmake_cache"])), "package CMake cache")
    values = _parse_cmake_cache(cache)
    mismatches = {
        key: {"expected": expected, "observed": values.get(key)}
        for key, expected in PACKAGE_CACHE_VALUES.items()
        if values.get(key) != expected
    }
    if mismatches:
        raise PackageLinuxError(f"package CMake profile differs: {mismatches}")
    toolchain_root = Path(str(result["toolchain_cache"])).resolve(strict=True)
    tools: dict[str, object] = {}
    for relative in (
        "llvm-22/bin/clang", "llvm-22/bin/clang++", "llvm-22/bin/llvm-config",
        "cmake/bin/cmake", "vcpkg/vcpkg",
    ):
        candidate = _require_regular(
            (toolchain_root / relative).resolve(strict=True),
            f"toolchain file {relative}")
        tools[relative] = {
            "path": str(candidate), "size_bytes": candidate.stat().st_size,
            "sha256": _sha256_file(candidate),
        }
    return {
        "cmake_cache": {
            "path": str(cache), "size_bytes": cache.stat().st_size,
            "sha256": _sha256_file(cache),
        },
        "profile": {key: values[key] for key in sorted(PACKAGE_CACHE_VALUES)},
        "toolchain_files": tools,
    }


def _copy_atomic(source: Path, destination: Path) -> Path:
    _require_regular(source, "copy source")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and (destination.is_symlink() or not destination.is_file()):
        raise PackageLinuxError(f"copy destination is not regular: {destination}")
    temporary = destination.with_name(f".{destination.name}.{os.getpid()}.tmp")
    try:
        shutil.copyfile(source, temporary)
        with temporary.open("rb") as handle:
            os.fsync(handle.fileno())
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)
    return destination


def _store_content_addressed(
    source: Path, object_root: Path, *, suffix: str,
) -> Path:
    source = _require_regular(source, "content-addressed source")
    digest = _sha256_file(source)
    if not _DIGEST.fullmatch(digest):
        raise AssertionError("computed SHA-256 is malformed")
    destination = object_root / digest[:2] / f"{digest}{suffix}"
    if destination.exists():
        existing = _require_regular(destination, "content-addressed object")
        if _sha256_file(existing) != digest:
            raise PackageLinuxError(
                f"content-addressed object collision at {destination}")
        return existing
    return _copy_atomic(source, destination)


def _load_cached_artifact(
    artifact_root: Path, build_key: str, build_contract: Mapping[str, object],
) -> tuple[Path, dict[str, object]] | None:
    directory = artifact_root / "build-keys" / build_key
    manifest_path = directory / "cache-manifest.json"
    if not manifest_path.is_file() or manifest_path.is_symlink():
        return None
    try:
        manifest = _load_json(manifest_path, "package cache manifest", sealed=True)
        _require_document_contract(
            manifest,
            label="package cache manifest",
            revision=2,
            artifact_kind="bioseq-local-linux-package-cache",
            keys={
                "build_key", "build_contract", "package", "package_files",
                "configuration", "content_sha256",
            },
        )
        if (
            manifest.get("build_key") != build_key
            or manifest.get("build_contract") != build_contract
        ):
            return None
        package = _descendant(
            Path(str(manifest["package"]["path"])), artifact_root, "cached package")
        _require_regular(package, "cached package")
        if _sha256_file(package) != manifest["package"]["sha256"]:
            return None
        package_files = _descendant(
            Path(str(manifest["package_files"]["path"])), artifact_root,
            "cached package-file manifest",
        )
        _require_regular(package_files, "cached package-file manifest")
        if _sha256_file(package_files) != manifest["package_files"]["sha256"]:
            return None
        configuration = manifest.get("configuration")
        if not isinstance(configuration, dict):
            return None
        cache_path = Path(str(configuration["cmake_cache"]["path"]))
        if _sha256_file(_require_regular(cache_path, "cached CMake cache")) != configuration["cmake_cache"]["sha256"]:
            return None
        for value in configuration["toolchain_files"].values():
            tool = _require_regular(Path(str(value["path"])), "cached toolchain file")
            if _sha256_file(tool) != value["sha256"]:
                return None
        return package, manifest
    except (KeyError, TypeError, OSError, PackageLinuxError):
        return None


def _promote_artifact(
    result: Mapping[str, object], artifact_root: Path, build_key: str,
    build_contract: Mapping[str, object], configuration: Mapping[str, object],
) -> tuple[Path, dict[str, object]]:
    directory = artifact_root / "build-keys" / build_key
    if directory.exists() and (directory.is_symlink() or not directory.is_dir()):
        raise PackageLinuxError(f"artifact cache path is unsafe: {directory}")
    directory.mkdir(parents=True, exist_ok=True)
    source_package = _require_regular(Path(str(result["package"])), "built DEB")
    if not source_package.name.endswith("_arm64.deb"):
        raise PackageLinuxError("retained DEB backend did not produce an ARM64 package")
    package = _store_content_addressed(
        source_package, artifact_root / "objects/packages",
        suffix=f"-{source_package.name}")
    source_list = _require_regular(
        Path(str(result["package_files"])), "staged package-file manifest")
    package_files = _store_content_addressed(
        source_list, artifact_root / "objects/package-files", suffix=".list")
    list_evidence = _package_file_list(package_files)
    manifest = _sealed({
        "schema_revision": 2,
        "artifact_kind": "bioseq-local-linux-package-cache",
        "build_key": build_key,
        "build_contract": dict(build_contract),
        "package": {
            "path": str(package), "basename": source_package.name,
            "size_bytes": package.stat().st_size, "sha256": _sha256_file(package),
        },
        "package_files": {
            "path": str(package_files), **list_evidence,
        },
        "configuration": dict(configuration),
    })
    _write_json_atomic(directory / "cache-manifest.json", manifest)
    return package, manifest


def _validate_package(
    *, docker_identity: Mapping[str, object],
    docker_environment: Mapping[str, str], image_id: str, source: Path,
    package: Path, state_root: Path, log: TextIO, deadline: float,
) -> tuple[dict[str, object], Path, Path]:
    validation_root = Path(tempfile.mkdtemp(prefix="validation-", dir=state_root))
    validator = validation_root / "validator.json"
    payload = validation_root / "payload.json"
    script = r"""set -Eeuo pipefail
export DEBIAN_FRONTEND=noninteractive
printf '#!/bin/sh\nexit 101\n' >/usr/sbin/policy-rc.d
chmod 0755 /usr/sbin/policy-rc.d
apt-get update
apt-get install -y --no-install-recommends binutils dpkg python3
python3 /source/infra/packaging/scripts/validate_package.py \
  --artifact /bioseq-postgresql17_4.2.3_arm64.deb \
  --architecture arm64 --postgres-major 17 \
  --postgres-runtime-package postgresql-17 --vulkan-runtime-package libvulkan1 \
  > /output/validator.json
mkdir -p /output/extracted
dpkg-deb -x /bioseq-postgresql17_4.2.3_arm64.deb /output/extracted
python3 /source/build_mcp/package_linux.py --internal-inspect \
  --extracted /output/extracted --validator /output/validator.json \
  --output /output/payload.json
"""
    try:
        docker = _validated_docker_entry(docker_identity)
        argv = [
            str(docker), "run", "--rm", "--init",
            "--mount", f"type=bind,src={source},dst=/source,readonly",
            "--mount", (
                "type=bind,src=" + str(package)
                + ",dst=/bioseq-postgresql17_4.2.3_arm64.deb,readonly"),
            "--mount", f"type=bind,src={validation_root},dst=/output",
            image_id, "/bin/bash", "-c", script,
        ]
        _run(argv, log=log, deadline=deadline, environment=docker_environment)
        validator_value = _load_json(validator, "retained package validator result")
        if (
            validator_value.get("passed") is not True
            or validator_value.get("six_artifacts") is not True
            or validator_value.get("package_architecture") != "arm64"
            or validator_value.get("postgres_major") != POSTGRES_MAJOR
        ):
            raise PackageLinuxError(
                "retained validator did not prove the fixed package contract")
        payload_value = _load_json(
            payload, "extracted package payload receipt", sealed=True)
        _require_document_contract(
            payload_value,
            label="extracted package payload receipt",
            revision=1,
            artifact_kind="bioseq-extracted-deb-payload-qualification",
            keys={
                "architecture", "postgres_major", "exact_six",
                "expected_basenames", "artifacts", "elf", "inspection_tool",
                "validator_sha256", "content_sha256",
            },
        )
        return payload_value, validator, payload
    except Exception:
        _safe_rmtree(validation_root, state_root)
        raise


def run_package_linux(
    *, repository_root: Path, log_handle: TextIO,
    deadline: float, progress: Callable[[str], None] | None = None,
    environment: Mapping[str, str] | None = None,
) -> dict[str, object]:
    """Build or reuse, then re-extract and qualify one fixed native DEB."""

    notify = progress or (lambda _step: None)
    if platform.system() == "Linux":
        from package_native import run_native_packages
        return run_native_packages(repository_root=repository_root,
                                   log_handle=log_handle, deadline=deadline,
                                   progress=notify)
    base_environment = dict(os.environ if environment is None else environment)
    repository = repository_root.resolve(strict=True)
    state_root = repository / "build" / MODE
    state_root.mkdir(parents=True, exist_ok=True)
    report_root = repository / "reporting" / REPORTING_SUBDIR
    report_root.mkdir(parents=True, exist_ok=True)
    lock_path = state_root / "qualification.lock"
    lock_fd = os.open(lock_path, os.O_RDWR | os.O_CREAT, 0o600)
    source_root: Path | None = None
    run_root: Path | None = None
    validation_root: Path | None = None
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as error:
        os.close(lock_fd)
        raise PackageLinuxError("another package-linux qualification owns the lock") from error
    try:
        notify("preflighting native ARM64 package host")
        _native_arm64()
        available = _require_free_space(state_root)
        snapshot = _source_snapshot(
            repository, state_root, log=log_handle, deadline=deadline)
        source_root = Path(str(snapshot["ephemeral_root"]))
        source = Path(str(snapshot["source"]))
        available_after_snapshot = _require_free_space(state_root)

        notify("validating source snapshot extension DSO producer contract")
        _validate_snapshot_extension_dso_contract(source)

        notify("checking pre-existing Docker daemon")
        endpoint, socket_path = _require_healthy_docker(base_environment)
        docker_environment = _docker_environment(base_environment, endpoint)
        docker_raw = shutil.which("docker", path=docker_environment.get("PATH"))
        if docker_raw is None:
            raise PackageLinuxError("Docker CLI is missing after daemon health proof")
        docker_identity = _docker_cli_identity(Path(docker_raw))
        image = _docker_image(
            docker_identity, docker_environment, log=log_handle, deadline=deadline)

        notify("preparing lock-keyed packaging environment")
        python_identity = _python_identity(
            base_environment.get("BIOSEQ_PACKAGE_BOOTSTRAP_PYTHON", "python3.13"),
            log=log_handle, deadline=deadline,
        )
        package_python, python_environment = _package_python(
            state_root, source, python_identity, log=log_handle, deadline=deadline)
        build_contract = {
            "schema_revision": 2,
            "artifact_kind": "bioseq-local-linux-package-build-contract",
            "mode": MODE,
            "revision": snapshot["revision"],
            "source_tree": snapshot["source_tree"],
            "archive_sha256": snapshot["archive_sha256"],
            "input_sha256": snapshot["input_sha256"],
            "image": image,
            "docker_cli": docker_identity,
            "architecture": ARCHITECTURE,
            "postgres_major": POSTGRES_MAJOR,
            "profile_revision": PACKAGE_PROFILE_REVISION,
            "python_environment_key": python_environment["cache_key"],
        }
        build_key = _canonical_sha256(build_contract)
        artifact_root = report_root / "artifacts"
        artifact_root.mkdir(exist_ok=True)
        cached = _load_cached_artifact(artifact_root, build_key, build_contract)
        cache_reused = cached is not None
        if cached is None:
            notify("building fixed Debian 13 PostgreSQL 17 ARM64 package")
            cache_workspace = state_root / "build-caches" / build_key
            cache_workspace.mkdir(parents=True, exist_ok=True)
            keyed_source_root = cache_workspace / (
                f"source-{os.getpid()}-{time.time_ns()}")
            os.replace(source_root, keyed_source_root)
            source_root = keyed_source_root
            source = keyed_source_root / "repository"
            run_root = Path(tempfile.mkdtemp(prefix="run-", dir=state_root))
            result_path = run_root / "build-result.json"
            internal_environment = dict(docker_environment)
            internal_environment.update({
                "BIOSEQ_TEMPLATE_FLATTEN_PROFILE": "all_alphabets_uint64",
                "BIOSEQ_PACKAGE_DOCKER_IDENTITY": json.dumps(
                    docker_identity, sort_keys=True, separators=(",", ":")),
                "SOURCE_DATE_EPOCH": _git(
                    repository, ["show", "-s", "--format=%ct", str(snapshot["revision"])],
                    log=log_handle, deadline=deadline),
            })
            _run(
                [str(package_python), str(source / "build_mcp/package_linux.py"),
                 "--internal-build", "--source", str(source),
                 "--build-root", str(run_root / "cell"),
                 "--revision", str(snapshot["revision"]),
                 "--lock-sha256", str(python_environment["poetry_lock_sha256"]),
                 "--image-id", str(image["id"]), "--result", str(result_path)],
                log=log_handle, deadline=deadline, environment=internal_environment,
            )
            result = _load_json(result_path, "retained DEB build result", sealed=True)
            _require_document_contract(
                result,
                label="retained DEB build result",
                revision=1,
                artifact_kind="bioseq-retained-deb-build-result",
                keys={
                    "package", "package_files", "cmake_cache",
                    "toolchain_cache", "content_sha256",
                },
            )
            configuration = _configuration_evidence(result)
            package, cache_manifest = _promote_artifact(
                result, artifact_root, build_key, build_contract, configuration)
        else:
            notify("reusing identity-matched package cache")
            package, cache_manifest = cached
            configuration = cache_manifest["configuration"]

        notify("extracting and qualifying actual DEB payload")
        payload, validator_path, payload_path = _validate_package(
            docker_identity=docker_identity,
            docker_environment=docker_environment,
            image_id=str(image["id"]), source=source, package=package,
            state_root=state_root, log=log_handle, deadline=deadline,
        )
        validation_root = payload_path.parent
        validator_copy = _store_content_addressed(
            validator_path, artifact_root / "objects/evidence", suffix="-validator.json")
        payload_copy = _store_content_addressed(
            payload_path, artifact_root / "objects/evidence", suffix="-payload.json")
        receipt = _sealed({
            "schema_revision": 2,
            "artifact_kind": "bioseq-local-linux-package-qualification",
            "status": "passed",
            "mode": MODE,
            "source": {
                "revision": snapshot["revision"],
                "source_tree": snapshot["source_tree"],
                "material_source_clean": True,
                "archive_from_captured_revision": True,
                "ignored_generated_status_rows":
                    snapshot["ignored_generated_status_rows"],
                "ignored_generated_status_count": len(
                    snapshot["ignored_generated_status_rows"]),
                "archive_sha256": snapshot["archive_sha256"],
                "input_sha256": snapshot["input_sha256"],
            },
            "host": {
                "architecture": ARCHITECTURE,
                "free_bytes_at_start": available,
                "free_bytes_after_snapshot": available_after_snapshot,
                "docker_socket": str(socket_path),
                "docker_cli": docker_identity,
            },
            "image": image,
            "package_environment": python_environment,
            "build": {
                "build_key": build_key,
                "cache_reused": cache_reused,
                "profile_revision": PACKAGE_PROFILE_REVISION,
                "configuration": configuration,
                "cache_manifest_content_sha256": cache_manifest["content_sha256"],
            },
            "package": {
                "path": str(package),
                "basename": cache_manifest["package"]["basename"],
                "size_bytes": package.stat().st_size,
                "sha256": _sha256_file(package),
                "format": "deb", "architecture": ARCHITECTURE,
                "postgres_major": POSTGRES_MAJOR,
            },
            "payload": {
                "exact_basenames": sorted(EXPECTED_BASENAMES),
                "artifacts": payload["artifacts"],
                "elf": payload["elf"],
                "validator": {
                    "path": str(validator_copy),
                    "sha256": _sha256_file(validator_copy),
                },
                "extraction_receipt": {
                    "path": str(payload_copy),
                    "sha256": _sha256_file(payload_copy),
                    "content_sha256": payload["content_sha256"],
                },
            },
        })
        receipts = report_root / "receipts"
        receipt_path = receipts / f"{receipt['content_sha256']}.json"
        _write_json_atomic(receipt_path, receipt)
        _write_json_atomic(report_root / "current-receipt.json", receipt)
        return {
            "receipt_path": str(receipt_path),
            "receipt_sha256": _sha256_file(receipt_path),
            "receipt_content_sha256": receipt["content_sha256"],
            "package_path": str(package),
            "package_sha256": receipt["package"]["sha256"],
            "cache_reused": cache_reused,
        }
    finally:
        for candidate in (validation_root, run_root, source_root):
            if candidate is not None and candidate.exists():
                _safe_rmtree(candidate, state_root)
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_UN)
        finally:
            os.close(lock_fd)


def _internal_build(arguments: argparse.Namespace) -> int:
    source = Path(arguments.source).resolve(strict=True)
    packaging = source / "infra/packaging"
    sys.path.insert(0, str(packaging))
    from bioseq_packaging.config import build_cells
    from bioseq_packaging.containers import ContainerRunner
    from bioseq_packaging.model import (
        Architecture, ContainerOutput, DistroId, WorkerRequest,
    )
    from bioseq_packaging.packagers import deb
    from bioseq_packaging.templates import TemplateRenderer
    from bioseq_packaging.workspace import WorkspaceManager

    if platform.machine().lower() not in {"arm64", "aarch64"}:
        raise PackageLinuxError("internal package helper is not native ARM64")
    request = WorkerRequest(
        architecture=Architecture.ARM64,
        bucket="local-only.invalid", region="local-only",
        postgres_majors=(POSTGRES_MAJOR,), distro_ids=(DistroId.DEBIAN_13,),
        source_dir=source, build_root=Path(arguments.build_root).resolve(),
        manifest_path=Path(arguments.result).resolve(),
        s3_prefix="local-only/package-linux", aws_bin="disabled",
        container_output=ContainerOutput.STDOUT, fail_fast=True,
        signing_secret_arn=None, source_revision=arguments.revision,
        poetry_lock_sha256=arguments.lock_sha256,
    )
    cells = build_cells(request)
    if len(cells) != 1:
        raise PackageLinuxError("fixed package request did not produce one cell")
    workspace = WorkspaceManager(request)
    cell = workspace.prepare_cell(cells[0])
    deb.render_debian_tree(
        cell, templates=TemplateRenderer(source_date_epoch=os.environ["SOURCE_DATE_EPOCH"]))
    plan = deb.build_plan(cell)
    policy = (
        "printf '#!/bin/sh\\nexit 101\\n' >/usr/sbin/policy-rc.d\n"
        "chmod 0755 /usr/sbin/policy-rc.d\n")
    plan = replace(
        plan, image=arguments.image_id,
        argv=("/bin/bash", "-c", policy + deb.DEB_BUILD_SCRIPT),
    )
    environment = dict(os.environ)
    environment.update({key: str(value) for key, value in deb.build_environment(cell).items()})
    docker_raw = os.environ.get("BIOSEQ_PACKAGE_DOCKER_IDENTITY", "")
    try:
        docker_identity = json.loads(docker_raw)
    except json.JSONDecodeError as error:
        raise PackageLinuxError("sealed Docker CLI identity is missing or invalid") from error
    docker = _validated_docker_entry(docker_identity)
    ContainerRunner(docker_bin=str(docker)).run(plan, environment=environment)
    package = deb.find_artifact(cell)
    package_files = cell.paths.root / "source/_bioseq-package-build/package-files.list"
    result = _sealed({
        "schema_revision": 1,
        "artifact_kind": "bioseq-retained-deb-build-result",
        "package": str(_require_regular(package, "built DEB")),
        "package_files": str(_require_regular(package_files, "package-file manifest")),
        "cmake_cache": str(_require_regular(
            cell.paths.cmake_cache / "build-rooted/CMakeCache.txt", "package CMake cache")),
        "toolchain_cache": str(cell.paths.toolchain_cache.resolve(strict=True)),
    })
    _write_json_atomic(Path(arguments.result).resolve(), result)
    return 0


def _readelf(path: Path, arguments: Sequence[str]) -> str:
    completed = subprocess.run(
        ["readelf", *arguments, str(path)], check=False,
        capture_output=True, text=True, env={**os.environ, "LC_ALL": "C", "LANG": "C"},
    )
    if completed.returncode != 0:
        raise PackageLinuxError(f"readelf failed for {path}: {completed.stderr.strip()}")
    return completed.stdout.replace("\r\n", "\n")


def _internal_inspect(arguments: argparse.Namespace) -> int:
    extracted = Path(arguments.extracted).resolve(strict=True)
    validator_path = _require_regular(Path(arguments.validator), "validator JSON")
    validator = _load_json(validator_path, "validator JSON")
    if validator.get("passed") is not True or validator.get("six_artifacts") is not True:
        raise PackageLinuxError("retained validator did not pass")
    entries: list[tuple[Path, os.stat_result]] = []
    identities: set[tuple[int, int]] = set()
    for candidate in extracted.rglob("*"):
        metadata = candidate.lstat()
        if stat.S_ISDIR(metadata.st_mode):
            continue
        if not stat.S_ISREG(metadata.st_mode):
            raise PackageLinuxError(
                f"package payload entry is not a regular non-symlink file: {candidate}")
        identity = (metadata.st_dev, metadata.st_ino)
        if metadata.st_nlink != 1 or identity in identities:
            raise PackageLinuxError(
                f"package payload entries must be distinct single-link files: {candidate}")
        identities.add(identity)
        entries.append((candidate, metadata))
    relatives = [candidate.relative_to(extracted).as_posix()
                 for candidate, _metadata in entries]
    basenames = [candidate.name for candidate, _metadata in entries]
    if (
        len(entries) != 6 or len(set(relatives)) != 6
        or len(set(basenames)) != 6 or set(basenames) != EXPECTED_BASENAMES
    ):
        raise PackageLinuxError(
            f"extracted DEB payload is not exactly the six extension files: {sorted(relatives)}")
    artifacts = []
    rows = zip(entries, relatives)
    for (candidate, metadata), relative in sorted(rows, key=lambda item: item[1]):
        artifacts.append({
            "path": relative, "basename": candidate.name,
            "size_bytes": metadata.st_size, "sha256": _sha256_file(candidate),
        })
    elf = []
    for dso in sorted(
        candidate for candidate, _metadata in entries if candidate.suffix == ".so"
    ):
        header = _readelf(dso, ["-h"])
        dynamic = _readelf(dso, ["-d"])
        notes = _readelf(dso, ["-n"])
        machine = re.search(r"^\s*Machine:\s*(.+?)\s*$", header, re.MULTILINE)
        if not machine or machine.group(1) != "AArch64":
            raise PackageLinuxError(f"{dso.name} is not an AArch64 ELF DSO")
        dependency_rows = re.findall(r"\(NEEDED\).*?\[([^]]+)\]", dynamic)
        if not dependency_rows or len(dependency_rows) != len(set(dependency_rows)):
            raise PackageLinuxError(
                f"{dso.name} has an empty or duplicate DT_NEEDED roster")
        dependencies = sorted(dependency_rows)
        sibling = "bioseq_cache.so" if dso.name == "bioseq.so" else "bioseq.so"
        if any(PurePosixPath(value).name.startswith(sibling) for value in dependencies):
            raise PackageLinuxError(f"{dso.name} directly depends on sibling {sibling}")
        raw_build_ids = re.findall(
            r"^\s*Build ID:\s*([0-9A-Fa-f]+)\s*$", notes, re.MULTILINE)
        build_ids = sorted({value.lower() for value in raw_build_ids})
        if (
            not build_ids or len(raw_build_ids) != len(build_ids)
            or any(len(value) % 2 or re.fullmatch(r"[0-9a-f]+", value) is None
                   for value in build_ids)
        ):
            raise PackageLinuxError(
                f"{dso.name} has a missing, duplicate, or invalid GNU build ID")
        elf.append({
            "basename": dso.name, "format": "ELF", "architecture": ARCHITECTURE,
            "machine": machine.group(1), "direct_load_dependencies": dependencies,
            "sibling_dependency_absent": True, "gnu_build_ids": build_ids,
            "header_listing_sha256": _sha256_bytes(header.encode("utf-8")),
            "dependency_listing_sha256": _sha256_bytes(dynamic.encode("utf-8")),
            "build_identity_listing_sha256": _sha256_bytes(notes.encode("utf-8")),
        })
    if {row["basename"] for row in elf} != {"bioseq.so", "bioseq_cache.so"}:
        raise PackageLinuxError("payload does not contain both production DSOs")
    readelf = Path(shutil.which("readelf") or "").resolve(strict=True)
    readelf = _require_regular(readelf, "readelf")
    receipt = _sealed({
        "schema_revision": 1,
        "artifact_kind": "bioseq-extracted-deb-payload-qualification",
        "architecture": ARCHITECTURE,
        "postgres_major": POSTGRES_MAJOR,
        "exact_six": True,
        "expected_basenames": sorted(EXPECTED_BASENAMES),
        "artifacts": artifacts,
        "elf": elf,
        "inspection_tool": {
            "path": str(readelf), "size_bytes": readelf.stat().st_size,
            "sha256": _sha256_file(readelf),
        },
        "validator_sha256": _sha256_file(validator_path),
    })
    _write_json_atomic(Path(arguments.output).resolve(), receipt)
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--internal-build", action="store_true")
    mode.add_argument("--internal-inspect", action="store_true")
    parser.add_argument("--source")
    parser.add_argument("--build-root")
    parser.add_argument("--revision")
    parser.add_argument("--lock-sha256")
    parser.add_argument("--image-id")
    parser.add_argument("--result")
    parser.add_argument("--extracted")
    parser.add_argument("--validator")
    parser.add_argument("--output")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        return _internal_build(arguments) if arguments.internal_build else _internal_inspect(arguments)
    except (KeyError, OSError, PackageLinuxError) as error:
        print(f"package-linux qualification failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
