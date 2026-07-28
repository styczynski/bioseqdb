#!/usr/bin/env python3
"""Run DOC0388 evidence against two owned PostgreSQL 17.10 clusters."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import sys
from typing import Sequence

from private_postgres import (
    ClusterCommandError,
    ClusterInputs,
    UniquePrivateCluster,
    _run_checked,
)


_AUTHORIZATION_MARKER = b"bioseq-safety-runtime-v2\n"
_TERMINAL_MARKER_DIRECTORY = "bioseq-safety-terminal-markers"
_PROFILE_NAMES = ("production", "test_indexes")
_POSTGRESQL_VERSION = "PostgreSQL 17.10"
_POSTGRESQL_TOOLS = (
    "postgres",
    "initdb",
    "pg_ctl",
    "createdb",
    "dropdb",
    "psql",
)


class RuntimeEvidenceLifecycleError(RuntimeError):
    """The isolated runtime-evidence lifecycle is incomplete or ambiguous."""


def _regular_file(value: str | Path, label: str) -> Path:
    path = Path(value).expanduser()
    try:
        metadata = path.lstat()
    except OSError as error:
        raise RuntimeEvidenceLifecycleError(
            f"{label} is missing: {path}") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise RuntimeEvidenceLifecycleError(
            f"{label} must be a regular non-symlink file: {path}")
    return path.resolve(strict=True)


def _executable(value: str | Path, label: str) -> Path:
    path = _regular_file(value, label)
    if not os.access(path, os.X_OK):
        raise RuntimeEvidenceLifecycleError(
            f"{label} is not an executable file: {path}")
    return path


def _output_root(value: str | Path) -> Path:
    path = Path(value).expanduser()
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        path.mkdir(parents=True)
        metadata = path.lstat()
    except OSError as error:
        raise RuntimeEvidenceLifecycleError(
            f"cannot inspect output root {path}: {error}") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        raise RuntimeEvidenceLifecycleError(
            f"output root must be a real directory: {path}")
    return path.resolve(strict=True)


def _path_within(path: Path, root: Path) -> bool:
    try:
        path.resolve(strict=False).relative_to(root.resolve(strict=False))
    except (OSError, RuntimeError, ValueError):
        return False
    return True


def _secure_child_directory(
    output_root: Path,
    relative: str | Path,
    label: str,
    *,
    create: bool = True,
) -> Path | None:
    relative_path = Path(relative)
    if (
        relative_path.is_absolute()
        or not relative_path.parts
        or ".." in relative_path.parts
    ):
        raise RuntimeEvidenceLifecycleError(
            f"{label} must be a normalized relative path: {relative_path}")
    current = output_root
    for component in relative_path.parts:
        current = current / component
        try:
            metadata = current.lstat()
        except FileNotFoundError:
            if not create:
                return None
            current.mkdir()
            metadata = current.lstat()
        except OSError as error:
            raise RuntimeEvidenceLifecycleError(
                f"cannot inspect {label} component {current}: {error}"
            ) from error
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(
            metadata.st_mode
        ):
            raise RuntimeEvidenceLifecycleError(
                f"{label} contains a non-directory or symlink component: "
                f"{current}"
            )
        resolved = current.resolve(strict=True)
        if not _path_within(resolved, output_root):
            raise RuntimeEvidenceLifecycleError(
                f"{label} escapes the output root: {current}")
    return current.resolve(strict=True)


def _require_regular_or_missing(path: Path, label: str) -> None:
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return
    except OSError as error:
        raise RuntimeEvidenceLifecycleError(
            f"cannot inspect {label} {path}: {error}") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise RuntimeEvidenceLifecycleError(
            f"{label} must be absent or a regular non-symlink file: {path}")


def _clear_real_directory(path: Path, label: str) -> None:
    metadata = path.lstat()
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        raise RuntimeEvidenceLifecycleError(
            f"{label} is not a real directory: {path}")
    for child in path.iterdir():
        child_metadata = child.lstat()
        if (
            stat.S_ISDIR(child_metadata.st_mode)
            and not stat.S_ISLNK(child_metadata.st_mode)
        ):
            shutil.rmtree(child)
        else:
            child.unlink()


def _atomic_copy(source: Path, destination: Path) -> None:
    parent_metadata = destination.parent.lstat()
    if (
        stat.S_ISLNK(parent_metadata.st_mode)
        or not stat.S_ISDIR(parent_metadata.st_mode)
    ):
        raise RuntimeEvidenceLifecycleError(
            f"copy destination parent is not a real directory: "
            f"{destination.parent}")
    _require_regular_or_missing(destination, "copy destination")
    temporary = destination.with_name(
        destination.name + f".tmp.{os.getpid()}")
    temporary.unlink(missing_ok=True)
    try:
        shutil.copyfile(source, temporary)
        os.chmod(temporary, 0o600)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise RuntimeEvidenceLifecycleError(
            f"cannot hash artifact {path}: {error}") from error
    return digest.hexdigest()


def _case_plan_count(path: Path, label: str) -> int:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RuntimeEvidenceLifecycleError(
            f"cannot read {label} {path}: {error}") from error
    cases = document.get("cases") if isinstance(document, dict) else None
    if not isinstance(cases, list) or not cases:
        raise RuntimeEvidenceLifecycleError(
            f"{label} must contain a nonempty cases array")
    return len(cases)


def _atomic_json(document: dict[str, object], destination: Path) -> None:
    parent_metadata = destination.parent.lstat()
    if (
        stat.S_ISLNK(parent_metadata.st_mode)
        or not stat.S_ISDIR(parent_metadata.st_mode)
    ):
        raise RuntimeEvidenceLifecycleError(
            f"JSON destination parent is not a real directory: "
            f"{destination.parent}")
    _require_regular_or_missing(destination, "JSON destination")
    temporary = destination.with_name(
        destination.name + f".tmp.{os.getpid()}")
    temporary.unlink(missing_ok=True)
    try:
        descriptor = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL,
            0o600,
        )
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(document, stream, indent=2, sort_keys=True)
            stream.write("\n")
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def _write_authorization_marker(data_directory: Path) -> Path:
    marker = data_directory / "bioseq-safety-runtime.authorization"
    marker.unlink(missing_ok=True)
    descriptor = os.open(
        marker,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL,
        0o600,
    )
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(_AUTHORIZATION_MARKER)
    metadata = marker.lstat()
    if (
        stat.S_ISLNK(metadata.st_mode)
        or not stat.S_ISREG(metadata.st_mode)
        or stat.S_IMODE(metadata.st_mode) != 0o600
        or metadata.st_uid != os.getuid()
    ):
        raise RuntimeEvidenceLifecycleError(
            f"authorization marker is not private: {marker}")
    return marker.resolve(strict=True)


def _terminal_marker_directory(data_directory: Path) -> Path:
    try:
        data_metadata = data_directory.lstat()
    except OSError as error:
        raise RuntimeEvidenceLifecycleError(
            f"cannot inspect private data directory {data_directory}: {error}"
        ) from error
    if stat.S_ISLNK(data_metadata.st_mode) or not stat.S_ISDIR(
        data_metadata.st_mode
    ):
        raise RuntimeEvidenceLifecycleError(
            "private data directory must be a real directory: "
            f"{data_directory}"
        )
    canonical_data_directory = data_directory.resolve(strict=True)
    marker_directory = (
        canonical_data_directory / _TERMINAL_MARKER_DIRECTORY
    )
    created = False
    try:
        marker_directory.mkdir(mode=0o700)
        created = True
    except FileExistsError:
        pass
    except OSError as error:
        raise RuntimeEvidenceLifecycleError(
            "cannot create terminal marker directory "
            f"{marker_directory}: {error}"
        ) from error
    if created:
        descriptor = -1
        try:
            descriptor = os.open(
                marker_directory,
                os.O_RDONLY
                | getattr(os, "O_DIRECTORY", 0)
                | getattr(os, "O_NOFOLLOW", 0),
            )
            os.fchmod(descriptor, 0o700)
        except OSError as error:
            raise RuntimeEvidenceLifecycleError(
                "cannot secure terminal marker directory "
                f"{marker_directory}: {error}"
            ) from error
        finally:
            if descriptor >= 0:
                os.close(descriptor)
    try:
        marker_metadata = marker_directory.lstat()
    except OSError as error:
        raise RuntimeEvidenceLifecycleError(
            "cannot inspect terminal marker directory "
            f"{marker_directory}: {error}"
        ) from error
    if (
        stat.S_ISLNK(marker_metadata.st_mode)
        or not stat.S_ISDIR(marker_metadata.st_mode)
        or stat.S_IMODE(marker_metadata.st_mode) != 0o700
        or marker_metadata.st_uid != os.getuid()
    ):
        raise RuntimeEvidenceLifecycleError(
            "terminal marker directory must be a private real directory "
            f"owned by the current user with mode 0700: {marker_directory}"
        )
    canonical_marker_directory = marker_directory.resolve(strict=True)
    if (
        canonical_marker_directory.parent != canonical_data_directory
        or not _path_within(
            canonical_marker_directory,
            canonical_data_directory,
        )
    ):
        raise RuntimeEvidenceLifecycleError(
            "terminal marker directory escapes the private data directory: "
            f"{marker_directory}"
        )
    return canonical_marker_directory


def _run(
    command: Sequence[str],
    *,
    timeout: int,
    log: Path,
    env: dict[str, str] | None = None,
) -> str:
    try:
        return _run_checked(
            command,
            timeout=timeout,
            env=env,
            output_path=log,
        )
    except ClusterCommandError as error:
        raise RuntimeEvidenceLifecycleError(str(error)) from error


def _load_stage_manifest(path: Path, stage_root: Path) -> dict[str, object]:
    manifest_path = _regular_file(path, "stage manifest")
    try:
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RuntimeEvidenceLifecycleError(
            f"cannot read stage manifest {manifest_path}: {error}") from error
    if not isinstance(document, dict) or document.get("schema") != "stage_manifest/v1":
        raise RuntimeEvidenceLifecycleError(
            f"stage manifest has the wrong schema: {manifest_path}")
    version = document.get("postgresql_version")
    if (
        not isinstance(version, str)
        or (
            version != _POSTGRESQL_VERSION
            and not version.startswith(_POSTGRESQL_VERSION + " ")
        )
    ):
        raise RuntimeEvidenceLifecycleError(
            "runtime evidence requires exact PostgreSQL 17.10, "
            f"got {version!r}")
    staged = document.get("staged")
    if not isinstance(staged, dict):
        raise RuntimeEvidenceLifecycleError(
            f"stage manifest has no staged layout: {manifest_path}")
    for key in ("bindir", "sharedir", "pkglibdir"):
        relative = staged.get(key)
        if not isinstance(relative, str) or not relative:
            raise RuntimeEvidenceLifecycleError(
                f"stage manifest staged.{key} is invalid")
        member = (stage_root / relative).resolve(strict=True)
        if not _path_within(member, stage_root) or not member.is_dir():
            raise RuntimeEvidenceLifecycleError(
                f"stage manifest staged.{key} escapes its root")

    expected_tool_paths = document.get("expected_tool_paths")
    if (
        not isinstance(expected_tool_paths, dict)
        or set(expected_tool_paths) != set(_POSTGRESQL_TOOLS)
    ):
        raise RuntimeEvidenceLifecycleError(
            "stage manifest expected_tool_paths does not close over the "
            "required PostgreSQL tool suite"
        )
    signatures = document.get("tool_signatures")
    if not isinstance(signatures, list) or len(signatures) != len(
        _POSTGRESQL_TOOLS
    ):
        raise RuntimeEvidenceLifecycleError(
            "stage manifest tool_signatures does not close over the required "
            "PostgreSQL tool suite"
        )
    signatures_by_tool: dict[str, dict[str, object]] = {}
    bindir_relative = staged["bindir"]
    assert isinstance(bindir_relative, str)
    for signature in signatures:
        if not isinstance(signature, dict) or set(signature) != {
            "tool",
            "staged_path",
            "version",
            "sha256",
        }:
            raise RuntimeEvidenceLifecycleError(
                "stage manifest carries a malformed PostgreSQL tool signature"
            )
        tool = signature["tool"]
        if (
            not isinstance(tool, str)
            or tool not in _POSTGRESQL_TOOLS
            or tool in signatures_by_tool
        ):
            raise RuntimeEvidenceLifecycleError(
                "stage manifest carries a duplicate or unknown PostgreSQL tool"
            )
        signatures_by_tool[tool] = signature

    for tool in _POSTGRESQL_TOOLS:
        expected_relative = str(Path(bindir_relative) / tool)
        declared_relative = expected_tool_paths[tool]
        signature = signatures_by_tool[tool]
        if (
            not isinstance(declared_relative, str)
            or declared_relative != expected_relative
            or signature["staged_path"] != expected_relative
        ):
            raise RuntimeEvidenceLifecycleError(
                f"stage manifest path for PostgreSQL tool {tool} is invalid"
            )
        version_prefix = f"{tool} (PostgreSQL) 17.10"
        tool_version = signature["version"]
        if (
            not isinstance(tool_version, str)
            or (
                tool_version != version_prefix
                and not tool_version.startswith(version_prefix + " ")
            )
        ):
            raise RuntimeEvidenceLifecycleError(
                f"stage manifest requires exact PostgreSQL 17.10 tool "
                f"{tool}, got {tool_version!r}"
            )
        staged_tool_argument = stage_root / declared_relative
        staged_tool = _regular_file(
            staged_tool_argument,
            f"staged PostgreSQL tool {tool}",
        )
        if not _path_within(staged_tool, stage_root):
            raise RuntimeEvidenceLifecycleError(
                f"stage manifest PostgreSQL tool {tool} escapes its root"
            )
        declared_sha256 = signature["sha256"]
        if (
            not isinstance(declared_sha256, str)
            or len(declared_sha256) != 64
            or any(character not in "0123456789abcdef"
                   for character in declared_sha256)
            or _sha256(staged_tool) != declared_sha256
        ):
            raise RuntimeEvidenceLifecycleError(
                f"stage manifest hash for PostgreSQL tool {tool} is invalid"
            )
    return document


def _stage_member(
    document: dict[str, object],
    stage_root: Path,
    key: str,
) -> Path:
    staged = document["staged"]
    assert isinstance(staged, dict)
    relative = staged[key]
    assert isinstance(relative, str)
    result = (stage_root / relative).resolve(strict=True)
    if not _path_within(result, stage_root):
        raise RuntimeEvidenceLifecycleError(
            f"staged {key} escapes {stage_root}")
    return result


def _stage_profile(
    *,
    profile: str,
    cmake: Path,
    pg_config: Path,
    stage_script: Path,
    control_file: Path,
    sql_file: Path,
    dso: Path,
    output_root: Path,
    timeout: int,
) -> tuple[Path, dict[str, object]]:
    stage_parent = _secure_child_directory(
        output_root,
        "stages",
        "profile stage parent",
    )
    assert stage_parent is not None
    stage_root = _secure_child_directory(
        output_root,
        Path("stages") / profile,
        f"{profile} stage root",
    )
    assert stage_root is not None
    manifest = stage_root / "stage-manifest.json"
    aggregate_hash = stage_root / "stage-artifacts.sha256"
    log = output_root / "logs" / f"stage-{profile}.log"
    command = [
        str(cmake),
        f"-DPG_CONFIG={pg_config}",
        f"-DSTAGE_ROOT={stage_root}",
        f"-DSTAGE_PARENT={stage_parent}",
        f"-DCONTROL_FILE={control_file}",
        f"-DINSTALL_SQL={sql_file}",
        f"-DLIBRARY_FILE={dso}",
        "-DEXTENSION_VERSION=1.0",
        f"-DMANIFEST_OUT={manifest}",
        f"-DHASH_OUT={aggregate_hash}",
        "-P",
        str(stage_script),
    ]
    _run(command, timeout=timeout, log=log)
    stage_root = stage_root.resolve(strict=True)
    document = _load_stage_manifest(manifest, stage_root)
    _regular_file(aggregate_hash, f"{profile} stage hash")
    staged_bindir = _stage_member(document, stage_root, "bindir")
    staged_sharedir = _stage_member(document, stage_root, "sharedir")
    staged_pkglibdir = _stage_member(document, stage_root, "pkglibdir")
    staged_artifacts = (
        (
            staged_sharedir / "extension" / control_file.name,
            control_file,
            f"{profile} staged control",
        ),
        (
            staged_sharedir / "extension" / sql_file.name,
            sql_file,
            f"{profile} staged SQL",
        ),
        (
            staged_pkglibdir / dso.name,
            dso,
            f"{profile} staged DSO",
        ),
    )
    for staged_path, source_path, label in staged_artifacts:
        staged_file = _regular_file(staged_path, label)
        if _sha256(staged_file) != _sha256(source_path):
            raise RuntimeEvidenceLifecycleError(
                f"{label} bytes differ from {source_path}")
    for tool in (
        "postgres",
        "initdb",
        "pg_ctl",
        "createdb",
        "dropdb",
        "psql",
    ):
        _executable(
            staged_bindir / tool,
            f"{profile} staged {tool}",
        )
    return stage_root, document


def _cluster_system_identifier(
    cluster: UniquePrivateCluster,
    staged_bindir: Path,
    *,
    timeout: int,
    log: Path,
) -> str:
    output = _run(
        [
            str(staged_bindir / "psql"),
            "-X",
            "--no-align",
            "--tuples-only",
            "-v",
            "ON_ERROR_STOP=1",
            "--host",
            str(cluster.socket_dir),
            "--port",
            str(cluster.port),
            "--username",
            cluster.database_user,
            "--dbname",
            cluster.database,
            "--command",
            "SELECT system_identifier::text "
            "FROM pg_catalog.pg_control_system();",
        ],
        timeout=timeout,
        log=log,
        env=cluster.child_environment(),
    ).strip()
    if not output or "\n" in output or not output.isdecimal():
        raise RuntimeEvidenceLifecycleError(
            f"private cluster returned an invalid system identifier: {output!r}")
    return output


def _cluster_config(
    *,
    profile: str,
    cluster: UniquePrivateCluster,
    system_identifier: str,
    stage_root: Path,
    stage_manifest: dict[str, object],
    control_file: Path,
    control_dso: Path,
    sql_file: Path,
    dso: Path,
    output_root: Path,
) -> Path:
    output_root = output_root.resolve(strict=True)
    staged_bindir = _stage_member(stage_manifest, stage_root, "bindir")
    staged_sharedir = _stage_member(stage_manifest, stage_root, "sharedir")
    staged_pkglibdir = _stage_member(stage_manifest, stage_root, "pkglibdir")
    stage_manifest_path = _regular_file(
        stage_root / "stage-manifest.json",
        f"{profile} stage manifest",
    )
    signatures = stage_manifest.get("tool_signatures")
    if not isinstance(signatures, list):
        raise RuntimeEvidenceLifecycleError(
            f"{profile} stage manifest tool signatures are invalid")
    signatures_by_tool: dict[str, dict[str, object]] = {}
    for signature in signatures:
        if not isinstance(signature, dict):
            raise RuntimeEvidenceLifecycleError(
                f"{profile} stage manifest tool signature is invalid")
        tool = signature.get("tool")
        if (
            not isinstance(tool, str)
            or tool not in _POSTGRESQL_TOOLS
            or tool in signatures_by_tool
        ):
            raise RuntimeEvidenceLifecycleError(
                f"{profile} stage manifest tool signature suite is invalid")
        signatures_by_tool[tool] = signature
    if set(signatures_by_tool) != set(_POSTGRESQL_TOOLS):
        raise RuntimeEvidenceLifecycleError(
            f"{profile} stage manifest tool signature suite is incomplete")
    normalized_signatures = [
        signatures_by_tool[tool]
        for tool in sorted(_POSTGRESQL_TOOLS)
    ]
    signature_suite = (
        json.dumps(
            normalized_signatures,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        )
        + "\n"
    ).encode("utf-8")
    terminal_marker_directory = _terminal_marker_directory(
        cluster.data_dir)
    authorization = _write_authorization_marker(
        terminal_marker_directory.parent)
    server_log_path = _regular_file(
        cluster.server_log,
        f"{profile} private PostgreSQL server log",
    )
    config_directory = _secure_child_directory(
        output_root,
        "cluster-configs",
        "cluster configuration root",
    )
    assert config_directory is not None
    config_path = config_directory / f"{profile}.json"
    document: dict[str, object] = {
        "schema_version": 1,
        "profile": profile,
        "dedicated_safety_cluster": True,
        "connection": {
            "host": str(cluster.socket_dir.resolve(strict=True)),
            "port": str(cluster.port),
            "database": cluster.database,
            "user": cluster.database_user,
            "password": "unused-private-trust-auth",
        },
        "private_stage_root": str(stage_root),
        "expected_data_directory": str(cluster.data_dir.resolve(strict=True)),
        "expected_system_identifier": system_identifier,
        "authorization_file": str(authorization),
        "terminal_marker_directory": str(terminal_marker_directory),
        "server_log_path": str(server_log_path),
        "installed_dso_path": str(staged_pkglibdir / dso.name),
        "installed_control_dso_path": str(
            staged_pkglibdir / control_dso.name),
        "installed_control_path": str(
            staged_sharedir / "extension" / control_file.name),
        "installed_sql_path": str(
            staged_sharedir / "extension" / sql_file.name),
        "stage_manifest_path": str(stage_manifest_path),
        "stage_manifest_sha256": _sha256(stage_manifest_path),
        "tool_signature_suite_sha256": hashlib.sha256(
            signature_suite).hexdigest(),
        "restart_command": [
            str(staged_bindir / "pg_ctl"),
            "--pgdata",
            str(cluster.data_dir.resolve(strict=True)),
            "--wait",
            "--mode=fast",
            "restart",
        ],
    }
    _atomic_json(document, config_path)
    return _regular_file(config_path, f"{profile} cluster configuration")


def _runtime_command(
    arguments: argparse.Namespace,
    paths: dict[str, Path],
    production_config: Path,
    test_indexes_config: Path,
    production_stage_manifest: Path,
    test_indexes_stage_manifest: Path,
    output_root: Path,
) -> list[str]:
    return [
        str(sys.executable),
        str(paths["runtime_producer"]),
        "run",
        "--production-ledger",
        str(paths["production_ledger"]),
        "--test-indexes-ledger",
        str(paths["test_indexes_ledger"]),
        "--production-runtime-inventory",
        str(paths["production_runtime_inventory"]),
        "--test-indexes-runtime-inventory",
        str(paths["test_indexes_runtime_inventory"]),
        "--runtime-inventory-producer-executable",
        str(paths["runtime_inventory_producer_executable"]),
        "--production-sql-manifest",
        str(paths["production_sql_manifest"]),
        "--test-indexes-sql-manifest",
        str(paths["test_indexes_sql_manifest"]),
        "--production-case-plan",
        str(paths["production_case_plan"]),
        "--test-indexes-case-plan",
        str(paths["test_indexes_case_plan"]),
        "--runtime-policy",
        str(paths["runtime_policy"]),
        "--production-dso",
        str(paths["production_dso"]),
        "--test-indexes-dso",
        str(paths["test_indexes_dso"]),
        "--runtime-control-dso",
        str(paths["control_dso"]),
        "--production-test-executable",
        str(paths["production_test_executable"]),
        "--test-indexes-test-executable",
        str(paths["test_indexes_test_executable"]),
        "--production-cluster-config",
        str(production_config),
        "--test-indexes-cluster-config",
        str(test_indexes_config),
        "--production-stage-manifest",
        str(production_stage_manifest),
        "--test-indexes-stage-manifest",
        str(test_indexes_stage_manifest),
        "--cluster-mode",
        "isolated",
        "--output-directory",
        str(output_root / "evidence"),
        "--output-matrix",
        str(output_root / "runtime-matrix.json"),
        "--timeout-seconds",
        str(arguments.timeout_seconds),
    ]


def _runtime_environment(
    cluster: UniquePrivateCluster,
) -> dict[str, str]:
    environment = cluster.child_environment()
    for name in tuple(environment):
        if name.startswith("PG") or name.startswith("BIOSEQ_TEST_PG"):
            environment.pop(name, None)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    return environment


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    for option in (
        "cmake",
        "pg-config",
        "stage-script",
        "config-template",
        "runtime-producer",
        "production-ledger",
        "test-indexes-ledger",
        "production-runtime-inventory",
        "test-indexes-runtime-inventory",
        "runtime-inventory-producer-executable",
        "production-sql-manifest",
        "test-indexes-sql-manifest",
        "production-sql-file",
        "test-indexes-sql-file",
        "control-file",
        "control-dso",
        "runtime-policy",
        "production-case-plan",
        "test-indexes-case-plan",
        "production-dso",
        "test-indexes-dso",
        "production-test-executable",
        "test-indexes-test-executable",
        "output-root",
        "stamp",
    ):
        parser.add_argument(f"--{option}", required=True)
    parser.add_argument(
        "--timeout-seconds",
        required=True,
        type=int,
    )
    return parser


def run(arguments: argparse.Namespace) -> None:
    if arguments.timeout_seconds < 1 or arguments.timeout_seconds > 600:
        raise RuntimeEvidenceLifecycleError(
            "--timeout-seconds must be in the range 1..600")

    paths: dict[str, Path] = {
        "cmake": _executable(arguments.cmake, "CMake executable"),
        "pg_config": _executable(
            arguments.pg_config, "PostgreSQL pg_config"),
    }
    artifact_options = {
        "stage_script": arguments.stage_script,
        "config_template": arguments.config_template,
        "runtime_producer": arguments.runtime_producer,
        "production_ledger": arguments.production_ledger,
        "test_indexes_ledger": arguments.test_indexes_ledger,
        "production_runtime_inventory":
            arguments.production_runtime_inventory,
        "test_indexes_runtime_inventory":
            arguments.test_indexes_runtime_inventory,
        "runtime_inventory_producer_executable":
            arguments.runtime_inventory_producer_executable,
        "production_sql_manifest": arguments.production_sql_manifest,
        "test_indexes_sql_manifest": arguments.test_indexes_sql_manifest,
        "production_sql_file": arguments.production_sql_file,
        "test_indexes_sql_file": arguments.test_indexes_sql_file,
        "control_file": arguments.control_file,
        "control_dso": arguments.control_dso,
        "runtime_policy": arguments.runtime_policy,
        "production_case_plan": arguments.production_case_plan,
        "test_indexes_case_plan": arguments.test_indexes_case_plan,
        "production_dso": arguments.production_dso,
        "test_indexes_dso": arguments.test_indexes_dso,
        "production_test_executable":
            arguments.production_test_executable,
        "test_indexes_test_executable":
            arguments.test_indexes_test_executable,
    }
    for key, value in artifact_options.items():
        paths[key] = _regular_file(
            value, key.replace("_", " "))

    if (
        paths["production_dso"] == paths["test_indexes_dso"]
        or _sha256(paths["production_dso"])
            == _sha256(paths["test_indexes_dso"])
    ):
        raise RuntimeEvidenceLifecycleError(
            "production and test-index DSO paths and bytes must be distinct")
    if paths["control_file"].name != "bioseq.control":
        raise RuntimeEvidenceLifecycleError(
            "control file must be named bioseq.control")
    if (
        paths["control_dso"].name == paths["production_dso"].name
        or paths["control_dso"].name == paths["test_indexes_dso"].name
        or _sha256(paths["control_dso"])
            in {
                _sha256(paths["production_dso"]),
                _sha256(paths["test_indexes_dso"]),
            }
    ):
        raise RuntimeEvidenceLifecycleError(
            "runtime control and profile DSOs must be distinct")
    if (
        paths["production_sql_file"]
        == paths["test_indexes_sql_file"]
    ):
        raise RuntimeEvidenceLifecycleError(
            "profile SQL file paths must be distinct")
    for profile in _PROFILE_NAMES:
        if paths[f"{profile}_sql_file"].name != "bioseq--1.0.sql":
            raise RuntimeEvidenceLifecycleError(
                f"{profile} SQL file must be named bioseq--1.0.sql")

    output_root = _output_root(arguments.output_root)
    writable_roots_by_name: dict[str, Path] = {}
    for name in (
        "evidence",
        "stages",
        "runs",
        "logs",
        "cluster-configs",
    ):
        writable_root = _secure_child_directory(
            output_root,
            name,
            f"{name} writable root",
        )
        assert writable_root is not None
        writable_roots_by_name[name] = writable_root
    for profile in _PROFILE_NAMES:
        _secure_child_directory(
            output_root,
            Path("stages") / profile,
            f"{profile} stage components",
        )
        _secure_child_directory(
            output_root,
            Path("runs") / profile,
            f"{profile} run components",
        )

    reviewed_directory = _secure_child_directory(
        output_root,
        "reviewed",
        "reviewed evidence root",
        create=False,
    )
    reviewed = (
        reviewed_directory
        if reviewed_directory is not None
        else output_root / "reviewed"
    )
    for name in (
        "production-ledger.json",
        "test-indexes-ledger.json",
        "runtime-matrix.json",
    ):
        _require_regular_or_missing(
            reviewed / name,
            f"reviewed {name}",
        )

    stamp_argument = Path(arguments.stamp).expanduser()
    try:
        stamp_metadata = stamp_argument.lstat()
    except FileNotFoundError:
        pass
    else:
        if stat.S_ISLNK(stamp_metadata.st_mode):
            raise RuntimeEvidenceLifecycleError(
                f"candidate stamp must not be a symlink: {stamp_argument}")
    stamp_argument = stamp_argument.resolve(strict=False)
    try:
        stamp_relative = stamp_argument.relative_to(output_root)
    except ValueError as error:
        raise RuntimeEvidenceLifecycleError(
            f"stamp must be inside the output root: {stamp_argument}"
        ) from error
    if not stamp_relative.parts:
        raise RuntimeEvidenceLifecycleError(
            "stamp must name a file below the output root")
    if stamp_relative.parent == Path("."):
        stamp_parent = output_root
    else:
        stamp_parent = _secure_child_directory(
            output_root,
            stamp_relative.parent,
            "stamp parent",
        )
        assert stamp_parent is not None
    stamp = stamp_parent / stamp_relative.name
    _require_regular_or_missing(stamp, "candidate stamp")

    reserved_outputs = {
        output_root / "production-ledger.json",
        output_root / "test-indexes-ledger.json",
        output_root / "runtime-matrix.json",
    }
    for candidate in reserved_outputs:
        _require_regular_or_missing(
            candidate,
            f"candidate leaf {candidate.name}",
        )
    if (
        _path_within(stamp, reviewed)
        or _path_within(stamp, writable_roots_by_name["evidence"])
        or stamp in reserved_outputs
    ):
        raise RuntimeEvidenceLifecycleError(
            f"stamp aliases reviewed or candidate evidence: {stamp}")
    stamp.unlink(missing_ok=True)

    evidence_directory = writable_roots_by_name["evidence"]
    writable_roots = tuple(writable_roots_by_name.values())
    for label, input_path in paths.items():
        if (
            input_path == stamp
            or input_path in reserved_outputs
            or any(
                _path_within(input_path, writable_root)
                for writable_root in writable_roots
            )
        ):
            raise RuntimeEvidenceLifecycleError(
                f"{label.replace('_', ' ')} aliases lifecycle output: "
                f"{input_path}")
    _clear_real_directory(
        evidence_directory,
        "candidate evidence directory",
    )
    (output_root / "runtime-matrix.json").unlink(missing_ok=True)
    _atomic_copy(
        paths["production_ledger"],
        output_root / "production-ledger.json",
    )
    _atomic_copy(
        paths["test_indexes_ledger"],
        output_root / "test-indexes-ledger.json",
    )

    stage_data: dict[str, tuple[Path, dict[str, object]]] = {}
    for profile in _PROFILE_NAMES:
        stage_data[profile] = _stage_profile(
            profile=profile,
            cmake=paths["cmake"],
            pg_config=paths["pg_config"],
            stage_script=paths["stage_script"],
            control_file=paths["control_file"],
            sql_file=paths[f"{profile}_sql_file"],
            dso=paths[f"{profile}_dso"],
            output_root=output_root,
            timeout=arguments.timeout_seconds,
        )

    def make_cluster(profile: str) -> UniquePrivateCluster:
        stage_root, manifest = stage_data[profile]
        return UniquePrivateCluster(
            ClusterInputs(
                bindir=_stage_member(manifest, stage_root, "bindir"),
                database="bioseq_safety_runtime",
                config_template=paths["config_template"],
                output_root=output_root / "runs" / profile,
                run_label=f"doc0388_{profile}",
                library_dirs=(
                    _stage_member(manifest, stage_root, "pkglibdir"),
                ),
                init_command_timeout=arguments.timeout_seconds,
                bootstrap_timeout=arguments.timeout_seconds,
                startup_timeout=arguments.timeout_seconds,
                shutdown_timeout=arguments.timeout_seconds,
            )
        )

    clusters: dict[str, UniquePrivateCluster] = {}
    started: list[UniquePrivateCluster] = []
    try:
        for profile in _PROFILE_NAMES:
            # Allocate the next port only after the prior node is listening;
            # UniquePrivateCluster's bounded free-port probe intentionally
            # releases its socket before PostgreSQL starts.
            clusters[profile] = make_cluster(profile)
            started.append(clusters[profile])
            clusters[profile].start()

        system_identifiers: dict[str, str] = {}
        for profile in _PROFILE_NAMES:
            stage_root, manifest = stage_data[profile]
            system_identifiers[profile] = _cluster_system_identifier(
                clusters[profile],
                _stage_member(manifest, stage_root, "bindir"),
                timeout=arguments.timeout_seconds,
                log=output_root / "logs" / f"identity-{profile}.log",
            )
        if (
            system_identifiers["production"]
            == system_identifiers["test_indexes"]
        ):
            raise RuntimeEvidenceLifecycleError(
                "isolated profile clusters have the same system identifier")
        if (
            clusters["production"].port
            == clusters["test_indexes"].port
            or clusters["production"].data_dir.resolve(strict=True)
            == clusters["test_indexes"].data_dir.resolve(strict=True)
            or clusters["production"].server_log.resolve(strict=True)
            == clusters["test_indexes"].server_log.resolve(strict=True)
        ):
            raise RuntimeEvidenceLifecycleError(
                "isolated profile clusters share a port, data directory, "
                "or server log")

        configs: dict[str, Path] = {}
        for profile in _PROFILE_NAMES:
            stage_root, manifest = stage_data[profile]
            configs[profile] = _cluster_config(
                profile=profile,
                cluster=clusters[profile],
                system_identifier=system_identifiers[profile],
                stage_root=stage_root,
                stage_manifest=manifest,
                control_file=paths["control_file"],
                control_dso=paths["control_dso"],
                sql_file=paths[f"{profile}_sql_file"],
                dso=paths[f"{profile}_dso"],
                output_root=output_root,
            )

        _run(
            [
                str(sys.executable),
                str(paths["runtime_producer"]),
                "self-test",
            ],
            timeout=arguments.timeout_seconds,
            log=output_root / "logs" / "runtime-producer-self-test.log",
            env=_runtime_environment(clusters["production"]),
        )
        _run(
            _runtime_command(
                arguments,
                paths,
                configs["production"],
                configs["test_indexes"],
                stage_data["production"][0] / "stage-manifest.json",
                stage_data["test_indexes"][0] / "stage-manifest.json",
                output_root,
            ),
            timeout=(
                arguments.timeout_seconds
                * (
                    _case_plan_count(
                        paths["production_case_plan"],
                        "production case plan",
                    )
                    + _case_plan_count(
                        paths["test_indexes_case_plan"],
                        "test-indexes case plan",
                    )
                    + 4
                )
                + 300
            ),
            log=output_root / "logs" / "runtime-evidence.log",
            env=_runtime_environment(clusters["production"]),
        )
    finally:
        shutdown_errors: list[str] = []
        for cluster in reversed(started):
            try:
                cluster.stop()
            except Exception as error:  # preserve teardown of the other node
                shutdown_errors.append(str(error))
        if shutdown_errors:
            raise RuntimeEvidenceLifecycleError(
                "private-cluster teardown failed: "
                + "; ".join(shutdown_errors))

    for candidate, label in (
        (output_root / "production-ledger.json", "production candidate ledger"),
        (output_root / "test-indexes-ledger.json", "test-index candidate ledger"),
        (output_root / "runtime-matrix.json", "candidate runtime matrix"),
    ):
        _regular_file(candidate, label)
    if (
        not evidence_directory.is_dir()
        or evidence_directory.is_symlink()
        or not any(evidence_directory.iterdir())
    ):
        raise RuntimeEvidenceLifecycleError(
            f"runtime producer emitted no candidate evidence: "
            f"{evidence_directory}")
    _atomic_json(
        {
            "schema_version": 1,
            "production_ledger_sha256": _sha256(
                output_root / "production-ledger.json"),
            "test_indexes_ledger_sha256": _sha256(
                output_root / "test-indexes-ledger.json"),
            "runtime_matrix_sha256": _sha256(
                output_root / "runtime-matrix.json"),
        },
        stamp,
    )
    _regular_file(stamp, "candidate stamp")


def main(argv: list[str] | None = None) -> int:
    try:
        run(_parser().parse_args(argv))
    except (
        OSError,
        ClusterCommandError,
        RuntimeEvidenceLifecycleError,
    ) as error:
        print(f"run_private_runtime_evidence: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
